// Coeur d'éclairage PBR — SOURCE DE VÉRITÉ UNIQUE, partagée par tous les shaders de
// surface (mesh_textured, terrain, ...).
//
// Il vit ici pour une raison concrète : ce fichier contient Cook-Torrance, les ombres
// PCF, l'IBL, les SH, le brouillard et l'ACES. Le dupliquer dans chaque shader de
// surface garantirait qu'ils divergent — et une divergence d'éclairage entre le terrain
// et la voie serait visible à la couture exacte où ils se rejoignent.
//
// Ce que l'includeur DOIT fournir : rien. Ce fichier déclare lui-même le set 0 (UBO,
// cascades d'ombre) et le set 2 (environnement préfiltré) ; l'includeur ne déclare que
// son set 1 (ses propres textures).
#ifndef NOIRE_PBR_GLSL
#define NOIRE_PBR_GLSL

#include "common/global_ubo.glsl"

// set 0, binding 1 : les cascades d'ombre. sampler2DShadow => la comparaison de
// profondeur est faite par le MATÉRIEL, et le filtrage LINEAR interpole les 4 résultats
// de comparaison (PCF 2x2 gratuit).
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMaps[2];

// set 2 : l'environnement PRÉFILTRÉ GGX en compute. Ce n'est PAS le ciel brut : chaque
// mip est un niveau de rugosité intégré par importance sampling, donc le mapping
// rugosité -> lod est EXACT par construction. Le soleil en a été retiré au chargement —
// il est porté par u.sunDirection/sunColor, l'y laisser le compterait deux fois.
layout(set = 2, binding = 0) uniform samplerCube envMap;

const float kPi = 3.14159265359;

// Plancher de rugosité : une GGX parfaitement lisse concentre tout le soleil sur un
// point sub-pixel => spéculaire à plusieurs milliers, scintillant d'une frame à l'autre.
const float kMinRoughness = 0.045;

// Météo. Mouillé = un film d'eau : il lisse la surface (chute BRUTALE de la rugosité) et
// piège la lumière par réflexion interne, ce qui assombrit l'albédo.
const float kWetRoughnessScale = 0.15;
const float kWetAlbedoScale = 0.35;

// PCF 3x3 : 9 comparaisons espacées d'un texel, chacune déjà filtrée 2x2 par le matériel
// => 6x6 effectif. `map` est un PARAMÈTRE : glslang inline la fonction, donc l'indice du
// sampler reste constant à la compilation — pas d'indexation dynamique d'un tableau de
// samplers (qui exigerait une feature dédiée).
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
    if (viewDepth > u.cascadeSplits.y) {
        return 1.0;  // au-delà de la dernière cascade : plus d'ombre portée
    }
    int cascade = viewDepth <= u.cascadeSplits.x ? 0 : 1;

    // `rel` est relatif caméra et lightViewProj est cadrée dans ce MÊME espace flottant :
    // aucune reconstruction de position monde n'est nécessaire.
    vec4 lightSpace = u.lightViewProj[cascade] * vec4(rel, 1.0);
    vec3 projected = lightSpace.xyz / lightSpace.w;
    if (projected.z > 1.0) {
        return 1.0;
    }
    // La passe d'ombre est ORTHOGRAPHIQUE et garde la convention de profondeur NORMALE
    // (le reverse-Z du M9 ne concerne que la caméra : une ortho est déjà linéaire, il ne
    // lui apporterait rien). D'où le test `> 1.0` et pas `< 0.0`.
    vec2 uv = projected.xy * 0.5 + 0.5;
    return cascade == 0 ? pcf3x3(shadowMaps[0], uv, projected.z)
                        : pcf3x3(shadowMaps[1], uv, projected.z);
}

// --- BRDF Cook-Torrance -----------------------------------------------------------
// `roughness` est la rugosité PERCEPTUELLE de glTF ; GGX travaille sur alpha =
// roughness^2 (c'est cette mise au carré qui rend le paramètre linéaire à l'oeil).
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float geometrySchlickGGX(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

// Le k de la lumière DIRECTE ((r+1)^2/8) diffère de celui de l'IBL.
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

// Irradiance du ciel depuis les SH9 (Ramamoorthi & Hanrahan 2001, eq. 13). Rend
// l'IRRADIANCE E(n), PAS une radiance : le diffus lambertien vaut albedo/PI * E.
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

// Terme BRDF de l'environnement (Lazarov, variante compacte de Karis) : remplace la LUT
// 2D du split-sum par quelques ALU. Rend (A, B) tels que le spéculaire vaut F0*A + B.
vec2 envBRDFApprox(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// ACES filmique (Narkowicz) : ramène le HDR vers 0..1 en préservant la teinte des hautes
// lumières. DOIT rester identique à celui du ciel (skybox.frag), sinon une marche nette
// apparaîtrait à l'horizon.
vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Normale perturbée par une normal map, dans l'espace monde. `mapSample` est le texel
// BRUT (0..1) — la décompression se fait ici.
vec3 shadingNormal(vec3 geoNormal, vec4 tangent4, vec3 mapSample, float normalScale) {
    vec3 N = normalize(geoNormal);
    vec3 T = normalize(tangent4.xyz);
    // Gram-Schmidt : l'interpolation entre sommets désaligne T de N.
    T = normalize(T - N * dot(N, T));
    vec3 B = cross(N, T) * tangent4.w;  // handedness glTF (w = ±1)

    // Décompression [0,1] -> [-1,1]. La normal map est échantillonnée en UNORM (pas
    // SRGB) : ce sont des octets bruts, les décoder en sRGB tordrait la normale.
    vec3 n = mapSample * 2.0 - 1.0;
    n.xy *= normalScale;  // glTF : normalScale ne pèse que sur XY
    return normalize(mat3(T, B, N) * n);
}

// Éclaire une surface entièrement résolue et rend la couleur FINALE (tonemappée).
// `cameraRelPos` : position du fragment relative à la caméra (origine flottante).
vec3 shadeSurface(vec3 albedo, float metallic, float roughness, vec3 N, vec3 cameraRelPos) {
    // --- Météo : le film d'eau agit sur le MATÉRIAU, avant tout éclairage ---
    float wetness = u.params.x;
    roughness = max(mix(roughness, roughness * kWetRoughnessScale, wetness), kMinRoughness);
    albedo *= mix(1.0, kWetAlbedoScale, wetness);

    // La caméra EST l'origine de l'espace relatif : le vecteur vue est l'opposé de la
    // position du fragment.
    vec3 V = normalize(-cameraRelPos);
    vec3 L = normalize(u.sunDirection.xyz);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);  // jamais 0 : il divise le dénominateur
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // F0 : 0.04 pour les diélectriques ; pour un métal, c'est l'albédo qui TEINTE la
    // réflexion (et le diffus meurt).
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // --- Soleil directionnel : Cook-Torrance ---
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(VdotH, F0);
    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);  // ce que Fresnel réfléchit ne diffuse pas

    float viewDepth = -(u.view * vec4(cameraRelPos, 1.0)).z;
    float shadow = sunShadow(cameraRelPos, viewDepth);

    // sunColor est réglé pour qu'une surface blanche lambertienne face au soleil lise
    // ~1.0 : l'irradiance vaut donc PI * sunColor, et ce PI annule exactement le 1/PI du
    // diffus. L'ombre ne masque QUE le direct — le ciel, lui, traverse.
    vec3 radiance = u.sunColor.rgb * kPi;
    vec3 direct = (kD * albedo / kPi + specular) * radiance * NdotL * shadow;

    // --- Ambiante : IBL ---
    float envMips = float(textureQueryLevels(envMap) - 1);
    // Les SH d'ordre 2 font du ringing et peuvent devenir négatives : une irradiance
    // négative n'a aucun sens physique.
    vec3 irradiance = max(shIrradiance(N), vec3(0.0));
    vec3 Famb = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kDamb = (vec3(1.0) - Famb) * (1.0 - metallic);
    // E est une IRRADIANCE : le 1/PI du lambert s'applique bel et bien ici, contrairement
    // au direct où il est annulé par la calibration de sunColor.
    vec3 ambientDiffuse = kDamb * albedo * irradiance / kPi;

    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(envMap, R, roughness * envMips).rgb;
    vec2 ab = envBRDFApprox(NdotV, roughness);
    vec3 ambientSpecular = prefiltered * (F0 * ab.x + ab.y);

    vec3 color = direct + ambientDiffuse + ambientSpecular;

    // --- Brouillard, AVANT le tone mapping ---
    // La couleur du brouillard EST le ciel dans la direction du regard, lu à un mip flou :
    // la géométrie lointaine se fond dans le ciel réellement derrière elle. Le fond étant
    // la skybox passée au MÊME ACES, le mélange en HDR linéaire est sans raccord.
    float dist = length(cameraRelPos);
    float fog = clamp(1.0 - exp(-u.fogColorDensity.a * dist), 0.0, 1.0);
    vec3 fogColor = textureLod(envMap, normalize(cameraRelPos), envMips * 0.5).rgb;
    color = mix(color, fogColor, fog);

    // Aucune correction gamma manuelle : la swapchain est en VK_FORMAT_B8G8R8A8_SRGB, le
    // matériel encode à l'écriture.
    return acesFilm(color);
}

#endif  // NOIRE_PBR_GLSL
