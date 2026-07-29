#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

using HeightSampler = std::function<double(double, double)>;

struct CityGridMeshData {
    std::vector<render::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;

    [[nodiscard]] bool valid() const { return !vertices.empty() && !indices.empty(); }
};

struct LampPost {
    double wx;  // position monde X
    double wy;  // position monde Y (échantillonnée sur le relief)
    double wz;  // position monde Z
    float yaw;  // rotation autour de Y
};

// M47 — Grande Avenue Centrale : la ville s'organise AUTOUR du viaduc, comme à Tokyo.
// L'avenue suit EXACTEMENT la spline de la voie, posée sur le terrain ; les piliers
// sont plantés dans son bitume (plus de zigzags ni de contournements). Des rues
// perpendiculaires régulières croisent l'avenue pour former des carrefours propres.
struct AvenueProfile {
    double step = 5.0;            // pas de tessellation le long de la voie (m)
    double road_half = 10.0;      // demi-largeur de chaussée de l'avenue (20 m)
    double sidewalk_w = 2.5;      // largeur des trottoirs (m)
    double sidewalk_h = 0.15;     // surélévation des trottoirs (m)
    double cross_spacing = 150.0; // entraxe ABSOLU des rues perpendiculaires (m)
    double cross_half = 6.0;      // demi-largeur de chaussée des rues (12 m)
    double cross_length = 600.0;  // portée latérale des rues de part et d'autre (m)
    double lamp_spacing = 30.0;   // entraxe des lampadaires le long de l'avenue (m)
};

// Une rue perpendiculaire (pour l'exclusion au semis des immeubles).
struct CrossStreet {
    glm::dvec3 point;    // point de la voie au chainage de croisement
    glm::dvec3 right;    // direction horizontale de la rue (unitaire)
    double half_length;  // portée de la rue de chaque côté de la voie
    double half_width;   // demi-largeur totale (chaussée + trottoirs + marge)
};

struct AvenueData {
    CityGridMeshData roadway;              // avenue + chaussées des rues (gris foncé)
    CityGridMeshData sidewalks;            // trottoirs surélevés (béton clair)
    std::vector<LampPost> lamps;           // lampadaires réguliers sur les trottoirs
    std::vector<CrossStreet> cross;        // rues, pour l'exclusion des immeubles
};

// Engendre l'avenue et ses rues sur la plage de chainage [s_start, s_end]. Sommets
// exprimés RELATIVEMENT à `origin` (origine flottante). Fonction PURE : aucun état,
// aucune API GPU — appelable depuis un worker. `height_fn` donne l'altitude du sol.
[[nodiscard]] AvenueData generate_avenue(const TrackSource& track,
                                         const HeightSampler& height_fn,
                                         double s_start, double s_end,
                                         const WorldPosition& origin,
                                         const AvenueProfile& profile = {});

}  // namespace noire::scene
