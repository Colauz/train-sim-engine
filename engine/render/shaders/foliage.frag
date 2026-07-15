#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/pbr.glsl"

// set = 1 : matériau ordinaire (3 textures). Le feuillage n'utilise que la base color —
// mais pour son ALPHA autant que pour sa couleur.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;

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

// Seuil de découpe (alphaCutoff glTF). Binaire : notre pipeline n'a pas de blending trié,
// seulement ce test.
const float kAlphaCutoff = 0.5;

void main() {
    vec4 base = texture(baseColorMap, fragUV) * object.baseColorFactor;

    // LE DISCARD. Il détoure les feuilles dans le quad — sans lui, un arbre serait quatre
    // rectangles de papier vert. Son coût est réel : il INTERDIT l'early-z sur ce
    // pipeline (le GPU ne peut pas rejeter un fragment avant d'avoir su s'il survit au
    // test), d'où le dessin de la végétation APRÈS le terrain opaque : ainsi le
    // depth-test rejette au moins ce qui est caché par le sol.
    if (base.a < kAlphaCutoff) {
        discard;
    }

    vec3 mr = texture(metallicRoughnessMap, fragUV).rgb;
    float metallic = clamp(mr.b * object.pbrFactors.x, 0.0, 1.0);
    float roughness = clamp(mr.g * object.pbrFactors.y, kMinRoughness, 1.0);

    vec3 N = shadingNormal(fragNormal, fragTangent, texture(normalMap, fragUV).rgb,
                           object.pbrFactors.z);
    // Une carte de feuillage se voit des DEUX côtés : on retourne la normale vers la
    // caméra, sinon la moitié des plans est noire — ils tournent le dos au ciel, et notre
    // ambiante vient entièrement de l'IBL, qui lit N.
    if (!gl_FrontFacing) {
        N = -N;
    }

    outColor = vec4(shadeSurface(base.rgb, metallic, roughness, N, cameraRelPos), 1.0);
}
