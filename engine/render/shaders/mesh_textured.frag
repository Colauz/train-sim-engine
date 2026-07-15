#version 450

// Layout canonique : cf. mesh.vert.
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;   // rgb = couleur brouillard, a = densité
    vec4 params;            // x = wetness
    vec4 sunDirection;      // xyz = direction VERS le soleil (normalisée)
    vec4 sunColor;          // rgb = couleur/intensité du soleil, a = intensité ambiante
    mat4 lightViewProj[2];  // une matrice par cascade d'ombre (kShadowCascades)
    vec4 cascadeSplits;     // x,y = fin de chaque cascade (distance en espace vue)
} u;

// set = 0, binding 1 : les cascades d'ombre. sampler2DShadow => la comparaison de
// profondeur est faite par le MATÉRIEL, et le filtrage LINEAR interpole les 4
// résultats de comparaison (PCF 2x2 gratuit).
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMaps[2];

// set = 1 : LE MATÉRIAU (glTF metallic-roughness), propre à chaque DrawItem.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;

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

// Rebond du sol dans l'ambiante hémisphérique, relatif à la lumière du ciel.
const float kGroundBounce = 0.30;

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

// Ciel au-dessus, rebond du sol en dessous. Y est l'axe vertical du monde : `dir` vit
// dans l'espace relatif caméra, qui n'est qu'une TRANSLATION du monde (pas de rotation)
// — sa composante y est donc bien la verticale monde.
vec3 hemisphere(vec3 dir, vec3 sky, vec3 ground) {
    return mix(ground, sky, clamp(dir.y * 0.5 + 0.5, 0.0, 1.0));
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

    // --- Ambiante hémisphérique ---
    // Le brouillard EST la couleur du ciel ; sunColor.a en porte l'intensité (l'app la
    // monte sous la pluie : couvert = lumière diffuse). Pour un hémisphère uniforme de
    // radiance L, l'irradiance vaut PI*L et le 1/PI du diffus l'annule : `skyLight`
    // s'applique donc tel quel, ce qui préserve le calibrage d'avant l'étape 4.
    vec3 skyLight = u.fogColorDensity.rgb * u.sunColor.a;
    vec3 groundLight = skyLight * kGroundBounce;

    vec3 Famb = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kDamb = (vec3(1.0) - Famb) * (1.0 - metallic);

    // Faute d'env map préfiltrée, l'hémisphère joue l'environnement : on l'échantillonne
    // dans la direction miroir, ramenée vers N à mesure que la surface se dégrade — un
    // flou de réflexion du pauvre. C'est ce qui empêche nos métaux d'être noirs à l'ombre.
    vec3 R = reflect(-V, N);
    vec3 envSpec = hemisphere(normalize(mix(R, N, roughness)), skyLight, groundLight);

    // Pas de carte d'occlusion dans nos matériaux (notre convention glTF déclare le canal
    // R de metallicRoughness inutilisé, on ne peut donc pas y lire une AO fiable).
    // L'occlusion vient de l'hémisphère lui-même : une face tournée vers le sol ne voit
    // que le rebond sombre, pas le ciel. C'est une AO directionnelle, gratuite et honnête.
    vec3 ambient = kDamb * albedo * hemisphere(N, skyLight, groundLight) + Famb * envSpec;

    // --- Sortie : HDR -> LDR ---
    vec3 color = acesFilm(direct + ambient);

    // Brouillard exponentiel, appliqué APRÈS le tone mapping : le fond est nettoyé avec
    // fogColor BRUTE (renderer.cpp : background_color_ = fog_color_density), sans ACES.
    // Mélanger avant ferait converger la géométrie lointaine vers acesFilm(fog) != fog,
    // soit une couture nette à l'horizon, là où le sol s'arrête.
    float dist = length(cameraRelPos);
    float fog = clamp(1.0 - exp(-u.fogColorDensity.a * dist), 0.0, 1.0);
    color = mix(color, u.fogColorDensity.rgb, fog);

    // AUCUNE correction gamma manuelle : la swapchain est en VK_FORMAT_B8G8R8A8_SRGB
    // (swapchain.cpp), le matériel encode à l'écriture. Un pow(1/2.2) ici délaverait tout.
    outColor = vec4(color, 1.0);
}
