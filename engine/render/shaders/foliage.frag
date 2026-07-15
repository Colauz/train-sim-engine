#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/pbr.glsl"
#include "common/foliage.glsl"

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

void main() {
    vec4 base = texture(baseColorMap, fragUV) * object.baseColorFactor;

    // LE DISCARD. Il détoure les feuilles dans le quad — sans lui, un arbre serait quatre
    // rectangles de papier vert. Son coût est réel : il INTERDIT l'early-z sur ce
    // pipeline (le GPU ne peut pas rejeter un fragment avant d'avoir su s'il survit au
    // test), d'où le dessin de la végétation APRÈS le terrain opaque : ainsi le
    // depth-test rejette au moins ce qui est caché par le sol.
    if (base.a < kFoliageAlphaCutoff) {
        discard;
    }

    vec3 mr = texture(metallicRoughnessMap, fragUV).rgb;
    float metallic = clamp(mr.b * object.pbrFactors.x, 0.0, 1.0);
    float roughness = clamp(mr.g * object.pbrFactors.y, kMinRoughness, 1.0);

    vec3 N = shadingNormal(fragNormal, fragTangent, texture(normalMap, fragUV).rgb,
                           object.pbrFactors.z);
    // PAS de retournement selon gl_FrontFacing, contrairement à ce qu'on ferait pour une
    // surface ordinaire. La normale de gen_tree.py n'est PAS perpendiculaire à la carte :
    // elle est gonflée à 75 % vers le HAUT (c'est une normale de VOLUME, qui décrit un
    // bouquet de folioles, pas un plan de carton). Aucune carte ne tourne donc le dos au
    // ciel, et la retourner la faisait pointer vers le SOL : NdotL négatif et irradiance
    // IBL prise sous terre => la moitié du feuillage rendait NOIR. C'était la vraie cause
    // des arbres charbon, bien avant l'albédo.

    // Le pipeline instancié sert AUSSI les poteaux caténaire (M12), qui ne veulent ni
    // diffus enveloppé ni transmission : c'est le matériau qui tranche, via pbrFactors.w.
    outColor = vec4(
        shadeSurfaceEx(base.rgb, metallic, roughness, N, cameraRelPos, object.pbrFactors.w), 1.0);
}
