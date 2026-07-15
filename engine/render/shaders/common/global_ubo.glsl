// Bloc d'uniformes global (set 0, binding 0) — SOURCE DE VÉRITÉ UNIQUE.
//
// Miroir exact de GpuFrameUniforms (renderer.cpp). Il était auparavant recopié à
// l'identique dans 5 shaders, et la moindre évolution de l'UBO obligeait à patcher les
// 5 : un oubli aurait décalé silencieusement TOUS les champs suivants (std140 ne
// prévient de rien). Toute évolution se fait désormais ICI, et nulle part ailleurs.
#ifndef NOIRE_GLOBAL_UBO_GLSL
#define NOIRE_GLOBAL_UBO_GLSL

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;   // rgb = couleur de fond (fallback), a = densité du brouillard
    vec4 params;            // x = wetness
    vec4 sunDirection;      // xyz = direction VERS le soleil (normalisée), extraite du HDR
    vec4 sunColor;          // rgb = couleur/intensité du soleil. a inutilisé (cf. sh)
    mat4 lightViewProj[2];  // une matrice par cascade d'ombre (kShadowCascades)
    vec4 cascadeSplits;     // x,y = fin de chaque cascade (distance en espace vue)
    // Irradiance du ciel en harmoniques sphériques d'ordre 2. vec4 et NON vec3 : en
    // std140 un tableau a un stride de 16 octets quoi qu'il arrive — déclarer vec3[9]
    // désaligne tout silencieusement. Seul .rgb porte l'information.
    vec4 sh[9];
} u;

#endif  // NOIRE_GLOBAL_UBO_GLSL
