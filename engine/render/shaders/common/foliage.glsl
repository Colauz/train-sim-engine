// Transformation d'une instance de végétation : échelle, lacet, et VENT.
//
// Ceci est un include, et surtout PAS un copier-coller, pour une raison précise : la
// même transformation doit être appliquée par mesh_instanced.vert (vue caméra) ET par
// shadow_instanced.vert (vue soleil). Si les deux divergent d'un iota — une constante
// retouchée d'un côté seulement — l'arbre se met à osciller pendant que son ombre reste
// plantée au sol, et l'illusion s'effondre. Le partage rend cette divergence IMPOSSIBLE.
//
// Le seuil alpha vit ici pour la même raison : foliage.frag découpe les feuilles à 0.5,
// shadow_foliage.frag doit découper EXACTEMENT les mêmes, sinon l'ombre ne correspond
// plus à la silhouette qui la projette.
#ifndef NOIRE_FOLIAGE_GLSL
#define NOIRE_FOLIAGE_GLSL

// Vent de Champagne : lent et ample, pas une tempête.
const float kWindSpeed = 1.35;
const float kWindAmplitude = 0.22;  // débattement de la cime, en mètres
const float kTreeHeight = 7.2;      // cf. tools/gen_tree.py

// Découpe du feuillage (alphaCutoff glTF). Binaire : pas de blending trié, ce test seul.
const float kFoliageAlphaCutoff = 0.5;

mat3 foliageYaw(float yaw) {
    float c = cos(yaw);
    float s = sin(yaw);
    return mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
}

// Position d'un sommet DANS le repère de l'instance : échelle, lacet, puis vent.
//   inPosition        : sommet du maillage d'arbre (lu une fois, partagé par toutes)
//   instPositionScale : xyz = position relative au groupe, w = échelle
//   instRotationPhase : x = lacet (rad), y = phase de vent, z = AMPLITUDE du vent
//                       (1 = arbre, 0 = mât rigide : un poteau caténaire d'acier partage ce
//                        pipeline et ne doit pas ployer)
//   t                 : temps (s)
//
// La phase vient de la POSITION MONDE de l'instance (calculée au semis) : sans elle,
// toute la forêt ondulerait à l'unisson comme un ballet, ce qui trahit le procédé
// instantanément.
// Le poids est le CARRÉ de la hauteur relative du sommet : nul au pied (le tronc est
// planté, il ne glisse pas sur le sol) et maximal à la cime. Le carré, plutôt qu'un
// linéaire, concentre le mouvement dans le feuillage et raidit le bas du tronc.
vec3 foliageLocal(vec3 inPosition, vec4 instPositionScale, vec4 instRotationPhase, float t) {
    vec3 local = foliageYaw(instRotationPhase.x) * (inPosition * instPositionScale.w);

    float h = clamp(local.y / (kTreeHeight * instPositionScale.w), 0.0, 1.0);
    float weight = h * h;
    float phase = instRotationPhase.y;
    // Deux fréquences incommensurables : une seule sinusoïde se lit comme un métronome.
    float sway = (sin(t * kWindSpeed + phase) + 0.45 * sin(t * kWindSpeed * 2.37 + phase * 1.7));
    sway *= instRotationPhase.z;  // 0 => rigide
    local.x += sway * kWindAmplitude * weight;
    local.z += sway * kWindAmplitude * 0.55 * weight;
    return local;
}

#endif  // NOIRE_FOLIAGE_GLSL
