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
    // Irradiance du ciel en harmoniques sphériques d'ordre 2 (M8 étape 6b). vec4 et
    // NON vec3 : en std140 un tableau a un stride de 16 octets quoi qu'il arrive —
    // déclarer vec3[9] désaligne tout silencieusement. Seul .rgb porte l'information.
    vec4 sh[9];
} u;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 cameraRelPos;

layout(location = 0) out vec4 outColor;

void main() {
    float wetness = u.params.x;

    // Mouillé : plus sombre et légèrement bleuté (les rails perdent en luminosité).
    vec3 base = fragColor * (1.0 - 0.45 * wetness) + vec3(0.02, 0.03, 0.05) * wetness;

    // Brouillard de distance exponentiel (la distance = norme de la position relative caméra).
    float dist = length(cameraRelPos);
    float fog = clamp(1.0 - exp(-u.fogColorDensity.a * dist), 0.0, 1.0);

    vec3 color = mix(base, u.fogColorDensity.rgb, fog);
    outColor = vec4(color, 1.0);
}
