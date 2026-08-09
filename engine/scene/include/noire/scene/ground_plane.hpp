#pragma once

#include "noire/core/math.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire::scene {

// M51 — LE SOL UNIFIÉ. Un seul et unique quad, horizontal, à Y = 0.
//
// Ce qui a été supprimé et pourquoi : le sol était engendré CELLULE PAR CELLULE
// (une nappe de mailles de 50 m qui épousait le relief, re-semée autour du train)
// et il se superposait au geo-clipmap du terrain. Deux surfaces distantes de
// quelques centimètres, vues à des centaines de mètres : la profondeur n'a plus
// assez de bits pour les départager, et le sol devenait un damier gris/vert
// clignotant parsemé de « plaques de béton ». C'était du Z-fighting, pas du
// contenu — et aucune valeur de décalage vertical ne pouvait le corriger, puisque
// le problème n'était pas la hauteur mais le FAIT MÊME d'avoir deux sols.
//
// La règle est donc devenue structurelle, et ce fichier l'incarne : le sol n'est
// PAS subdivisible. 4 sommets, 2 triangles, aucun échantillonnage de hauteur,
// aucune tuile, aucun joint — donc aucun Z-fighting possible.
//
// Le quad est engendré UNE FOIS puis simplement RÉ-ANCRÉ (sa matrice modèle suit le
// train par pas grossiers) : il ne se régénère jamais. `half_extent` doit dépasser
// le plan lointain de la caméra (10 km) pour que sa rive ne soit jamais visible.
//
// M53 — DES UV, ENFIN. Le M51 avait laissé les UV constants, en concluant d'un sol
// en damier qu'une texture au sol était impossible ici. Ce n'était pas la texture le
// problème, c'étaient les DEUX SOLS ; la règle « un seul sol » suffit, et elle est
// respectée. `uv_period` est le côté, en mètres, de la tuile de texture : les UV
// valent donc (x / période, z / période), la seule convention qui rende un enrobé à
// son échelle réelle. Doit être > 0.
//
// ATTENTION : le pas de ré-ancrage doit être un MULTIPLE ENTIER de `uv_period`. Les
// UV sont solidaires du maillage, donc du repère ré-ancré : si le pas ne tombe pas
// sur une période entière, la texture SAUTE d'une fraction de tuile à chaque
// ré-ancrage — un glissement du sol entier, tous les kilomètres. L'appelant le
// vérifie par static_assert (cf. kGroundUvPeriod dans application.cpp).
[[nodiscard]] RailMeshData generate_ground_plane(double half_extent, double uv_period);

}  // namespace noire::scene
