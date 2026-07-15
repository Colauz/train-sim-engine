#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/foliage.glsl"

// Fragment shader de la passe d'ombre, pour le feuillage UNIQUEMENT.
//
// Il n'écrit aucune couleur — la passe est depth-only. Il n'existe que pour son
// `discard` : sans lui, la carte de profondeur enregistrerait le quad ENTIER, et chaque
// plan de feuillage projetterait au sol l'ombre d'un rectangle parfait. Toute la
// silhouette gagnée par l'alpha dans la vue caméra serait perdue dans l'ombre.
//
// C'est aussi la raison pour laquelle la géométrie opaque garde son pipeline SANS
// fragment shader : un shader qui ne fait rien coûterait l'early-z (le GPU ne peut plus
// écrire la profondeur avant d'avoir su si le fragment survit) sur tout le reste de la
// scène, pour rien.
//
// set = 1 : le matériau, comme partout ailleurs dans ce moteur. Le set 0 est déclaré dans
// le layout mais JAMAIS lié : ce shader n'y touche pas, et Vulkan n'exige de liaison que
// pour les bindings réellement utilisés.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;

layout(push_constant) uniform PushConstants {
    mat4 lightMvp;
    vec4 windParams;
    vec4 baseColorFactor;
} object;

layout(location = 0) in vec2 fragUV;

void main() {
    // Convention glTF : alpha = texture * facteur. Le seuil vient de common/foliage.glsl,
    // partagé avec foliage.frag — les deux découpes DOIVENT coïncider, sinon l'ombre ne
    // correspond plus à la silhouette qui la projette.
    const float alpha = texture(baseColorMap, fragUV).a * object.baseColorFactor.a;
    if (alpha < kFoliageAlphaCutoff) {
        discard;
    }
}
