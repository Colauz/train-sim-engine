#version 450

// Layout canonique : cf. mesh.vert.
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;   // rgb = couleur brouillard, a = densité
    vec4 params;            // x = wetness
    vec4 sunDirection;      // xyz = direction VERS le soleil (normalisée)
    mat4 lightViewProj[2];  // une matrice par cascade d'ombre (kShadowCascades)
    vec4 cascadeSplits;     // x,y = fin de chaque cascade (distance en espace vue)
} u;

// set = 1 : matériau. Combined image sampler, propre à chaque DrawItem.
layout(set = 1, binding = 0) uniform sampler2D baseColor;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 cameraRelPos;
layout(location = 2) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 albedo = texture(baseColor, fragUV).rgb;

    // Éclairage directionnel simple : donne du relief au modèle texturé. Le soleil
    // vient de l'UBO — même direction que celle qui cadre les cascades d'ombre.
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(u.sunDirection.xyz);
    float diffuse = max(dot(N, L), 0.0);
    vec3 lit = albedo * (0.35 + 0.65 * diffuse);

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
