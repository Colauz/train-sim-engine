#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/global_ubo.glsl"

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

// Vent de Champagne : lent et ample, pas une tempête.
const float kWindSpeed = 1.35;
const float kWindAmplitude = 0.22;  // débattement de la cime, en mètres
const float kTreeHeight = 7.2;      // cf. tools/gen_tree.py

void main() {
    float yaw = instRotationPhase.x;
    float c = cos(yaw);
    float s = sin(yaw);
    mat3 rot = mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);

    vec3 local = rot * (inPosition * instPositionScale.w);

    // --- Vent ---------------------------------------------------------------
    // La phase vient de la POSITION MONDE de l'instance (calculée au semis) : sans elle,
    // toute la forêt ondulerait à l'unisson comme un ballet, ce qui trahit le procédé
    // instantanément.
    // Le poids est le CARRÉ de la hauteur relative du sommet : nul au pied (le tronc est
    // planté, il ne glisse pas sur le sol) et maximal à la cime. Le carré, plutôt qu'un
    // linéaire, concentre le mouvement dans le feuillage et raidit le bas du tronc.
    float t = u.params.y;  // temps (s), fourni par l'app
    float h = clamp(local.y / (kTreeHeight * instPositionScale.w), 0.0, 1.0);
    float weight = h * h;
    float phase = instRotationPhase.y;
    // Deux fréquences incommensurables : une seule sinusoïde se lit comme un métronome.
    float sway = (sin(t * kWindSpeed + phase) + 0.45 * sin(t * kWindSpeed * 2.37 + phase * 1.7));
    local.x += sway * kWindAmplitude * weight;
    local.z += sway * kWindAmplitude * 0.55 * weight;

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
