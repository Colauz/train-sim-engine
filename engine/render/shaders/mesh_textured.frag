#version 450

#extension GL_GOOGLE_include_directive : require
// Tout l'éclairage (Cook-Torrance, ombres PCF, IBL/SH, brouillard, ACES) vit dans cet
// include, partagé avec terrain.frag. Ce shader ne fait plus que RÉSOUDRE son matériau.
#include "common/pbr.glsl"

// set = 1 : LE MATÉRIAU (glTF metallic-roughness), propre à chaque DrawItem.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;

// Miroir exact de TexturedPushConstants (renderer.cpp) : cf. mesh_textured.vert.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbrFactors;  // x = metallic, y = roughness, z = normalScale
    vec4 emissiveFactor;  // rgb = émissif HDR (M31)
} object;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 cameraRelPos;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;

layout(location = 0) out vec4 outColor;

void main() {
    // Convention glTF : facteur * texture. Sans texture, le secours 1x1 laisse le facteur
    // seul décider (ex. le gris acier des rails, ou l'alpha 0.35 du verre PBR).
    vec4 baseColor = texture(baseColorMap, fragUV) * object.baseColorFactor;
    vec3 albedo = baseColor.rgb;
    float alpha = baseColor.a;
    vec3 mr = texture(metallicRoughnessMap, fragUV).rgb;  // G = roughness, B = metallic
    float metallic = clamp(mr.b * object.pbrFactors.x, 0.0, 1.0);
    float roughness = clamp(mr.g * object.pbrFactors.y, kMinRoughness, 1.0);

    vec3 N = shadingNormal(fragNormal, fragTangent, texture(normalMap, fragUV).rgb,
                           object.pbrFactors.z);

    // Émissif (M31) : convention glTF (facteur * texture), modulé par la NUIT
    // (u.skyParams.x) : les fenêtres et les néons veillent à ~15 % le jour et flambent
    // dès que le soleil se couche. L'ACES, en aval, gère le HDR.
    vec3 emissive = texture(emissiveMap, fragUV).rgb * object.emissiveFactor.rgb *
                    mix(0.15, 1.0, u.skyParams.x);

    outColor = vec4(shadeSurface(albedo, metallic, roughness, N, cameraRelPos, emissive),
                    alpha);
}
