#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/foliage.glsl"

// Passe d'ombre de la VÉGÉTATION. Le shadow.vert ordinaire ne peut pas s'en charger : il
// n'a qu'un binding de sommets, donc il dessinerait UN arbre à l'origine du groupe au
// lieu des centaines semées.
//
// Le temps arrive par PUSH CONSTANT et non par le set 0 : ce set porte aussi les
// sampler2DShadow, or on est justement en train d'écrire dedans. Le lier ici serait au
// mieux inutile, au pire un aller-retour de layout sur une image en cours d'écriture.
layout(push_constant) uniform PushConstants {
    mat4 lightMvp;         // lightViewProj * model (model = origine du GROUPE)
    vec4 windParams;       // x = temps (s)
    vec4 baseColorFactor;  // lu par le fragment, pour le test alpha
} object;

// binding 0 : le maillage, un jeu par SOMMET. normale et tangente ne servent pas au
// depth-only, mais le binding décrit un MeshVertex complet : le stride doit correspondre.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

// binding 1 : les instances, un jeu par ARBRE.
layout(location = 4) in vec4 instPositionScale;
layout(location = 5) in vec4 instRotationPhase;

layout(location = 0) out vec2 fragUV;

void main() {
    // EXACTEMENT la transformation de mesh_instanced.vert, vent compris — c'est tout
    // l'intérêt du include partagé : sans le vent ici, l'arbre se balancerait pendant que
    // son ombre resterait figée au sol.
    const vec3 local =
        foliageLocal(inPosition, instPositionScale, instRotationPhase, object.windParams.x);

    fragUV = inUV;
    gl_Position = object.lightMvp * vec4(instPositionScale.xyz + local, 1.0);
}
