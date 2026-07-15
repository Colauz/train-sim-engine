#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/global_ubo.glsl"

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
