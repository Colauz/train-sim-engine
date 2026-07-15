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

// set = 1 : matériau. Combined image sampler, propre à chaque DrawItem.
layout(set = 1, binding = 0) uniform sampler2D baseColor;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 cameraRelPos;
layout(location = 2) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

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

void main() {
    vec3 albedo = texture(baseColor, fragUV).rgb;

    // Éclairage directionnel : le soleil vient de l'UBO — même direction que celle
    // qui cadre les cascades d'ombre.
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(u.sunDirection.xyz);
    float diffuse = max(dot(N, L), 0.0);

    // Profondeur de VUE (le long de l'axe de visée) : c'est l'unité dans laquelle
    // cascadeSplits est exprimé. La vue n'ayant pas de translation, `cameraRelPos`
    // suffit à la calculer.
    float viewDepth = -(u.view * vec4(cameraRelPos, 1.0)).z;
    float shadow = sunShadow(cameraRelPos, viewDepth);

    // Ambiante = lumière du ciel : on lui donne la teinte du brouillard, qui EST la
    // couleur du ciel. Le direct est le seul terme occulté par l'ombre.
    vec3 ambient = albedo * u.fogColorDensity.rgb * u.sunColor.a;
    vec3 direct = albedo * u.sunColor.rgb * diffuse * shadow;
    vec3 lit = ambient + direct;

    // --- Ambiance M6 réintégrée : humidité + brouillard de distance ---
    float wetness = u.params.x;
    // Mouillé : plus sombre et légèrement bleuté.
    vec3 base = lit * (1.0 - 0.45 * wetness) + vec3(0.02, 0.03, 0.05) * wetness;

    // Brouillard exponentiel (distance = norme de la position relative caméra).
    float dist = length(cameraRelPos);
    float fog = clamp(1.0 - exp(-u.fogColorDensity.a * dist), 0.0, 1.0);

    vec3 color = mix(base, u.fogColorDensity.rgb, fog);
    outColor = vec4(color, 1.0);
}
