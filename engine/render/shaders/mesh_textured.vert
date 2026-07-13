#version 450

// UBO global : matrices caméra + paramètres météo (set 0, partagé avec le pipeline debug).
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;  // rgb = couleur brouillard, a = densité
    vec4 params;           // x = wetness
} u;

layout(push_constant) uniform PushConstants {
    mat4 model;  // déjà relatif à la caméra (origine flottante)
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 cameraRelPos;  // position relative caméra (fog)
layout(location = 2) out vec3 fragNormal;    // normale monde (éclairage)

void main() {
    vec4 rel = object.model * vec4(inPosition, 1.0);
    cameraRelPos = rel.xyz;
    // Normale transformée par la partie rotation du modèle (pas d'échelle non uniforme ici).
    fragNormal = normalize(mat3(object.model) * inNormal);
    fragUV = inUV;
    gl_Position = u.proj * u.view * rel;
}
