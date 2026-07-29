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

// M47 — Gare aérienne (le reste de la ligne reste à ciel ouvert). Engendre, centré
// sur le chainage `s_center` : deux quais latéraux (plateformes à +1,10 m au-dessus
// du plan de roulement, soit ~10,5 m au-dessus du sol), une verrière de `length`
// mètres de long UNIQUEMENT au-dessus des quais, et ses colonnes. Mêmes conventions
// que generate_viaduct (plan de roulement = 0, origine flottante, fonction pure).
struct StationProfile {
    double length = 150.0;      // longueur des quais et de la verrière (m)
    double step = 10.0;         // pas de tessellation le long de la voie (m)
    float platform_inner = 4.6f;  // bord de quai = rive du tablier
    float platform_outer = 8.0f;  // rive extérieure du quai (3,4 m de large)
    float platform_top = 1.10f;   // dessus du quai au-dessus du plan de roulement
    float roof_y = 5.5f;        // intrados de la verrière
    float roof_thickness = 0.35f;
    float column_half = 0.18f;  // section des colonnes de verrière
    double column_spacing = 15.0;
};

[[nodiscard]] RailMeshData generate_station(const TrackSource& track, double s_center,
                                            const WorldPosition& origin,
                                            const StationProfile& profile = {});

}  // namespace noire::scene
