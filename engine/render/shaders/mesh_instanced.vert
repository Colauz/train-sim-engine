#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/global_ubo.glsl"
#include "common/foliage.glsl"

// Miroir de TexturedPushConstants. `model` place le GROUPE (l'origine de la tuile) ;
// c'est le tampon d'instances qui place chaque arbre à l'intérieur.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbrFactors;
} object;

// binding 0 : le maillage, un jeu par SOMMET (l'arbre, lu une seule fois).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

// binding 1 : les instances, un jeu par ARBRE (VK_VERTEX_INPUT_RATE_INSTANCE).
layout(location = 4) in vec4 instPositionScale;  // xyz = position relative, w = échelle
layout(location = 5) in vec4 instRotationPhase;  // x = lacet, y = phase de vent

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 cameraRelPos;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragTangent;

void main() {
    mat3 rot = foliageYaw(instRotationPhase.x);
    // Échelle + lacet + vent, depuis common/foliage.glsl : shadow_instanced.vert appelle
    // la MÊME fonction, c'est ce qui garantit que l'ombre suit le balancement.
    vec3 local = foliageLocal(inPosition, instPositionScale, instRotationPhase, u.params.y);

    vec4 rel = object.model * vec4(instPositionScale.xyz + local, 1.0);
    cameraRelPos = rel.xyz;

    // Les normales suivent le lacet mais PAS le vent : à 22 cm de débattement, l'erreur
    // d'inclinaison est de l'ordre du degré. Recalculer une normale déplacée coûterait
    // une dérivée par sommet pour un gain invisible — et la faire mal ferait clignoter
    // l'éclairage à chaque oscillation.
    fragNormal = normalize(mat3(object.model) * (rot * inNormal));
    fragTangent = vec4(normalize(mat3(object.model) * (rot * inTangent.xyz)), inTangent.w);
    fragUV = inUV;
    gl_Position = u.proj * u.view * rel;
}
