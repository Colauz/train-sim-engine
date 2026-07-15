#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/global_ubo.glsl"

// set = 0, binding 1 : les cascades d'ombre. sampler2DShadow => la comparaison de
// profondeur est faite par le MATÉRIEL, et le filtrage LINEAR interpole les 4
// résultats de comparaison (PCF 2x2 gratuit).
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMaps[2];

// set = 1 : LE MATÉRIAU (glTF metallic-roughness), propre à chaque DrawItem.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;

// set = 2 : l'environnement PRÉFILTRÉ GGX en compute (M8 étape 7). Ce n'est PAS le ciel
// brut : chaque mip est un niveau de rugosité intégré par importance sampling, donc le
// mapping rugosité -> lod ci-dessous est EXACT par construction (mip i = rugosité
// i/(mips-1)) et non plus une heuristique sur des filtres boîte.
// Le soleil en a été retiré au chargement — il est porté par u.sunDirection/sunColor,
// l'y laisser le compterait deux fois (une fois en spéculaire d'env, une fois en direct).
layout(set = 2, binding = 0) uniform samplerCube envMap;

// Miroir exact de TexturedPushConstants (renderer.cpp) : cf. mesh_textured.vert.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbrFactors;  // x = metallic, y = roughness, z = normalScale
} object;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 cameraRelPos;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265359;

// Plancher de rugosité : une GGX parfaitement lisse concentre tout le soleil sur un
// point sub-pixel => spéculaire à plusieurs milliers, scintillant d'une frame à l'autre.
const float kMinRoughness = 0.045;

// Météo. Mouillé = un film d'eau : il lisse la surface (chute BRUTALE de la rugosité)
// et piège la lumière par réflexion interne, ce qui assombrit l'albédo.
const float kWetRoughnessScale = 0.15;  // x0.15 : béton mat (0.9) -> quasi miroir (0.14)
const float kWetAlbedoScale = 0.35;

// PCF 3x3 : 9 comparaisons espacées d'un texel, chacune déjà filtrée 2x2 par le
// matériel => 6x6 effectif. `map` est un PARAMÈTRE : glslang inline la fonction,
// donc l'indice du sampler reste constant à la compilation — pas d'indexation
// dynamique d'un tableau de samplers (qui exigerait une feature dédiée).
float pcf3x3(sampler2DShadow map, vec2 uv, float ref) {
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(map, vec3(uv + vec2(x, y) * texel, ref));
        }
    }
    return sum / 9.0;
}

// 1.0 = pleinement éclairé, 0.0 = totalement à l'ombre.
float sunShadow(vec3 rel, float viewDepth) {
    // Au-delà de la dernière cascade (kShadowDistance) : plus d'ombre portée.
    if (viewDepth > u.cascadeSplits.y) {
        return 1.0;
    }
    int cascade = viewDepth <= u.cascadeSplits.x ? 0 : 1;

    // `rel` est relatif caméra et lightViewProj est cadrée dans ce MÊME espace
    // flottant : aucune reconstruction de position monde n'est nécessaire.
    // L'ortho donne w = 1, mais on divise par acquit de conscience.
    vec4 lightSpace = u.lightViewProj[cascade] * vec4(rel, 1.0);
    vec3 projected = lightSpace.xyz / lightSpace.w;
    if (projected.z > 1.0) {
        return 1.0;  // au-delà du plan lointain de la cascade
    }

    // Profondeur déjà dans 0..1 (convention Vulkan). XY : NDC -> UV sans inverser Y,
    // car la passe d'ombre a rasterisé avec cette même convention (ndc.y = -1 => v = 0).
    vec2 uv = projected.xy * 0.5 + 0.5;

    return cascade == 0 ? pcf3x3(shadowMaps[0], uv, projected.z)
                        : pcf3x3(shadowMaps[1], uv, projected.z);
}

// --- BRDF Cook-Torrance -----------------------------------------------------------
// Convention : `roughness` est la rugosité PERCEPTUELLE de glTF ; GGX travaille sur
// alpha = roughness^2 (c'est cette mise au carré qui rend le paramètre linéaire à l'oeil).

// Distribution des micro-facettes : GGX / Trowbridge-Reitz.
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float geometrySchlickGGX(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

// Auto-masquage/ombrage des micro-facettes : Smith + Schlick-GGX. Le k de la lumière
// DIRECTE ((r+1)^2/8) diffère de celui de l'IBL : on n'a que du direct ici.
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return geometrySchlickGGX(NdotV, k) * geometrySchlickGGX(NdotL, k);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Variante rugueuse (Karis) pour l'ambiante : sans elle, une surface mate garde un
// liseré de Fresnel aussi vif qu'un miroir sur ses silhouettes.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Irradiance du ciel reconstruite depuis les SH9 (Ramamoorthi & Hanrahan 2001, eq. 13).
// Rend l'IRRADIANCE E(n), pas une radiance : le diffus lambertien vaut albedo/PI * E.
// Les coefficients ont été projetés au chargement sur le ciel PRIVÉ DE SON SOLEIL.
vec3 shIrradiance(vec3 n) {
    const float c1 = 0.429043;
    const float c2 = 0.511664;
    const float c3 = 0.743125;
    const float c4 = 0.886227;
    const float c5 = 0.247708;
    vec3 L00 = u.sh[0].rgb;
    vec3 L1m1 = u.sh[1].rgb;
    vec3 L10 = u.sh[2].rgb;
    vec3 L1p1 = u.sh[3].rgb;
    vec3 L2m2 = u.sh[4].rgb;
    vec3 L2m1 = u.sh[5].rgb;
    vec3 L20 = u.sh[6].rgb;
    vec3 L2p1 = u.sh[7].rgb;
    vec3 L22 = u.sh[8].rgb;
    return c1 * L22 * (n.x * n.x - n.y * n.y) + c3 * L20 * n.z * n.z + c4 * L00 - c5 * L20 +
           2.0 * c1 * (L2m2 * n.x * n.y + L2p1 * n.x * n.z + L2m1 * n.y * n.z) +
           2.0 * c2 * (L1p1 * n.x + L1m1 * n.y + L10 * n.z);
}

// Terme BRDF de l'environnement, approximation analytique de Lazarov (Call of Duty),
// dans la variante compacte de Karis. Remplace la LUT 2D du split-sum par quelques ALU :
// une texture et sa passe de génération en moins. Rend (A, B) tels que le spéculaire
// intégré vaut F0 * A + B.
vec2 envBRDFApprox(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// ACES filmique (approximation de Narkowicz) : ramène le HDR vers 0..1 en préservant
// la teinte des hautes lumières, là où un simple clamp les aplatirait en blanc pur.
vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Normale perturbée par la normal map, dans l'espace monde.
vec3 shadingNormal() {
    vec3 N = normalize(fragNormal);
    vec3 T = normalize(fragTangent.xyz);
    // Gram-Schmidt : l'interpolation entre sommets désaligne T de N. On la ré-orthogonalise
    // ici plutôt que de faire confiance au barycentre.
    T = normalize(T - N * dot(N, T));
    vec3 B = cross(N, T) * fragTangent.w;  // handedness glTF (w = ±1)

    // Décompression [0,1] -> [-1,1]. La normal map est échantillonnée en UNORM (pas SRGB) :
    // ce sont des octets bruts, les décoder en sRGB tordrait la normale.
    vec3 n = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
    // glTF : normalScale ne pèse que sur XY (Z reste 1) — c'est l'inclinaison qu'on dose.
    n.xy *= object.pbrFactors.z;

    return normalize(mat3(T, B, N) * n);
}

void main() {
    // --- Matériau (convention glTF metallic-roughness) ---
    // Sans texture, le secours 1x1 laisse le facteur seul décider (ex. le gris acier
    // des rails) : blanc pour l'albédo, (_, 1, 0) pour metal/rough, plate pour la normale.
    vec3 albedo = texture(baseColorMap, fragUV).rgb * object.baseColorFactor.rgb;
    vec3 mr = texture(metallicRoughnessMap, fragUV).rgb;  // G = roughness, B = metallic
    float metallic = clamp(mr.b * object.pbrFactors.x, 0.0, 1.0);
    float roughness = clamp(mr.g * object.pbrFactors.y, kMinRoughness, 1.0);

    // --- Météo : le film d'eau agit sur le MATÉRIAU, avant tout éclairage ---
    float wetness = u.params.x;
    roughness = max(mix(roughness, roughness * kWetRoughnessScale, wetness), kMinRoughness);
    albedo *= mix(1.0, kWetAlbedoScale, wetness);

    vec3 N = shadingNormal();
    // La caméra EST l'origine de l'espace relatif (origine flottante) : le vecteur vue
    // est donc directement l'opposé de la position du fragment.
    vec3 V = normalize(-cameraRelPos);
    vec3 L = normalize(u.sunDirection.xyz);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);  // jamais 0 : il divise le dénominateur de Cook-Torrance
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // F0 : réflectance à incidence normale. 0.04 pour les diélectriques (verre, peinture,
    // béton) ; pour un métal, c'est l'albédo qui TEINTE la réflexion (et le diffus meurt).
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // --- Soleil directionnel : Cook-Torrance ---
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(VdotH, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 1e-4);
    // Conservation d'énergie : ce que Fresnel réfléchit ne peut pas diffuser. Un métal
    // pur n'a AUCUN diffus.
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // Profondeur de VUE (le long de l'axe de visée) : c'est l'unité dans laquelle
    // cascadeSplits est exprimé. La vue n'ayant pas de translation, `cameraRelPos`
    // suffit à la calculer.
    float viewDepth = -(u.view * vec4(cameraRelPos, 1.0)).z;
    float shadow = sunShadow(cameraRelPos, viewDepth);

    // sunColor est réglé côté app pour qu'une surface blanche lambertienne face au soleil
    // lise ~1.0 : l'irradiance correspondante vaut donc PI * sunColor, et ce PI annule
    // exactement le 1/PI du terme diffus. Le spéculaire, lui, en profite légitimement.
    vec3 radiance = u.sunColor.rgb * kPi;
    // L'ombre ne masque QUE le direct — l'ambiante est la lumière du ciel, elle traverse.
    vec3 direct = (kD * albedo / kPi + specular) * radiance * NdotL * shadow;

    // --- Ambiante : IBL (étape 6b) ---
    // Le dernier mip utile de la chaîne. textureQueryLevels rend le NOMBRE de niveaux.
    float envMips = float(textureQueryLevels(envMap) - 1);

    // Diffus : irradiance du ciel en SH9. Les SH peuvent devenir légèrement négatives
    // (ringing de la troncature à l'ordre 2) — un max() suffit, une irradiance négative
    // n'a aucun sens physique et donnerait des pixels noirs.
    vec3 irradiance = max(shIrradiance(N), vec3(0.0));

    vec3 Famb = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kDamb = (vec3(1.0) - Famb) * (1.0 - metallic);
    // E est une IRRADIANCE : le 1/PI du lambert s'applique donc bel et bien ici
    // (contrairement au direct, où il est annulé par la calibration de sunColor).
    vec3 ambientDiffuse = kDamb * albedo * irradiance / kPi;

    // Spéculaire : chaque mip EST une rugosité, préfiltrée par importance sampling GGX
    // (étape 7). Le lod est donc une lecture directe de la table, plus une approximation.
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(envMap, R, roughness * envMips).rgb;
    vec2 ab = envBRDFApprox(NdotV, roughness);
    vec3 ambientSpecular = prefiltered * (F0 * ab.x + ab.y);

    // Pas de carte d'occlusion dans nos matériaux (notre convention glTF déclare le canal
    // R de metallicRoughness inutilisé) : l'occlusion directionnelle vient des SH
    // elles-mêmes, qui savent qu'une face tournée vers le sol voit moins de ciel.
    vec3 ambient = ambientDiffuse + ambientSpecular;

    // --- Sortie ---
    vec3 color = direct + ambient;

    // Brouillard exponentiel, désormais AVANT le tone mapping. Ce que l'étape 4 ne
    // pouvait pas faire : le fond était alors la couleur de brouillard BRUTE, non
    // tonemappée, et mélanger en HDR aurait ouvert une couture à l'horizon. Maintenant
    // que le fond est la skybox passée au MÊME ACES, le mélange linéaire est à la fois
    // plus physique et sans raccord. Et la couleur du brouillard EST le ciel dans la
    // direction du regard, lu à un mip flou : la géométrie lointaine se fond dans le ciel
    // réellement derrière elle, plus clair vers le soleil.
    float dist = length(cameraRelPos);
    float fog = clamp(1.0 - exp(-u.fogColorDensity.a * dist), 0.0, 1.0);
    vec3 fogColor = textureLod(envMap, normalize(cameraRelPos), envMips * 0.5).rgb;
    color = mix(color, fogColor, fog);

    // AUCUNE correction gamma manuelle : la swapchain est en VK_FORMAT_B8G8R8A8_SRGB
    // (swapchain.cpp), le matériel encode à l'écriture. Un pow(1/2.2) ici délaverait tout.
    outColor = vec4(acesFilm(color), 1.0);
}
