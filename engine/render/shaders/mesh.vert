#version 450

// UBO global : matrices caméra + météo + soleil/ombres. Ce bloc est le layout
// CANONIQUE (std140), déclaré à l'identique dans tous les shaders et miroir exact
// de GpuFrameUniforms (renderer.cpp) : toute évolution se répercute des deux côtés.
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

layout(push_constant) uniform PushConstants {
    mat4 model;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 cameraRelPos;  // position relative caméra (pour le fog)

void main() {
    // model est déjà relatif à la caméra (origine flottante) => sa norme = distance caméra.
    vec4 rel = object.model * vec4(inPosition, 1.0);
    cameraRelPos = rel.xyz;
    gl_Position = u.proj * u.view * rel;
    fragColor = inColor;
}
