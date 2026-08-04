#pragma once

namespace noire {
class Terrain;
}

namespace noire::scene {

// M51 — LE CORRIDOR SANITAIRE. Les distances qui décident de l'occupation du sol.
// Elles vivent ici, en un seul exemplaire : c'est ce qui rend la règle vérifiable.
struct ExclusionZones {
    double track_clearance = 25.0;    // règle 1 : emprise de la voie
    double station_clearance = 45.0;  // règle 2 : emprise d'une gare (plus large)
    double station_spacing = 2000.0;  // entraxe des gares (grille absolue de chainage)
    double station_length = 150.0;    // longueur des quais
};

// L'espace au point monde (wx, wz) est-il libre de toute emprise ferroviaire ?
//
// C'est LE test d'occupation spatiale du moteur, et il est unique : tout ce qui se
// sème dans le monde doit passer par lui. Le désastre qu'il corrige venait
// précisément de règles concurrentes — le semis d'immeubles connaissait la voie mais
// IGNORAIT les gares, qu'il engloutissait donc consciencieusement.
//
//   Règle 1 — LA VOIE : à moins de `track_clearance` de l'axe, c'est occupé. Le
//   viaduc, ses piles et le gabarit de la rame vivent là.
//   Règle 2 — LA GARE : à moins de `station_clearance`, c'est occupé. La règle
//   s'applique à toute l'EMPRISE du quai (±`station_length`/2 de chainage autour du
//   centre), et pas au seul point central : une gare fait 150 m de long, et un rayon
//   pris au centre seul aurait laissé les têtes de quai retomber à 25 m — des tours
//   contre la verrière. La zone est donc une CAPSULE autour du segment de quai, qui
//   contient strictement le disque demandé.
//
// `footprint` est le rayon de l'objet à poser (0 = un point) : on écarte alors son
// EMPRISE et pas son centre, sinon un immeuble déborde dans le corridor par un mur.
//
// Fonction PURE (Terrain::distance_to_track est pure et thread-safe) : elle est donc
// appelable depuis un worker, comme tous les générateurs de la couche scene.
[[nodiscard]] bool is_space_clear(const Terrain& terrain, double wx, double wz,
                                  double footprint = 0.0, const ExclusionZones& zones = {});

}  // namespace noire::scene
