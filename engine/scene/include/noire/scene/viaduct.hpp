#pragma once

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire {
class Terrain;
}

namespace noire::scene {

// Viaduc urbain (M31) : tablier de béton sous la voie + piles descendant au sol naturel.
//
// CONVENTION VERTICALE : comme track_mesh et catenary — la courbe rendue par
// TrackSource::sample EST le PLAN DE ROULEMENT. Le tablier se construit donc en dessous
// (son dessus affleure le pied du ballast, à -0,80 m), et les piles partent du dessous
// du tablier pour atteindre Terrain::height.
struct ViaductProfile {
    double step = 5.0;            // pas de tessellation du tablier le long de la voie
    float deck_half_width = 4.6f; // demi-largeur du tablier (le ballast fait 3,4 m)
    float deck_top_y = -0.80f;    // dessus du tablier = base du ballast
    float deck_thickness = 1.30f; // poutre-caisson béton
    double pillar_spacing = 35.0; // entraxe des piles, sur grille ABSOLUE de chainage
    float pillar_half_width = 1.6f;  // section du fût (pile-box)
    float pillar_embed = 4.0f;    // la pile s'enfonce sous le terrain (jamais de jour)
};

// Engendre le viaduc sur la plage de chainage [x_start, x_end]. Sommets exprimés
// RELATIVEMENT à `origin` (origine flottante). Fonction PURE (aucun état, aucune API
// GPU) => appelable depuis un worker, comme generate_track_mesh. Le terrain n'est lu
// que pour l'ancrage des piles (Terrain::height est pure et thread-safe).
[[nodiscard]] RailMeshData generate_viaduct(const TrackSource& track, const Terrain& terrain,
                                            double x_start, double x_end,
                                            const WorldPosition& origin,
                                            const ViaductProfile& profile = {});

}  // namespace noire::scene
