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
// PAS subdivisible. 4 sommets, 2 triangles, une couleur unie, aucun échantillonnage
// de hauteur, aucune tuile, aucun joint — donc aucun Z-fighting possible.
//
// Le quad est engendré UNE FOIS puis simplement RÉ-ANCRÉ (sa matrice modèle suit le
// train par pas grossiers) : il ne se régénère jamais. `half_extent` doit dépasser
// le plan lointain de la caméra (10 km) pour que sa rive ne soit jamais visible.
[[nodiscard]] RailMeshData generate_ground_plane(double half_extent);

}  // namespace noire::scene
