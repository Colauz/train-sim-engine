#pragma once

#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

struct RailProfile {
    double gauge = 1.435;
    float rail_half_width = 0.06f;
    float rail_height = 0.12f;
    double sample_step = 1.0;  // pas d'échantillonnage le long de la voie (m)
    glm::vec3 color{0.55f, 0.57f, 0.62f};
};

// Extrude deux rails le long de la voie sur la plage de chainage [x_start, x_end].
// Sommets exprimés RELATIVEMENT à `origin` (float) => compatible origine flottante,
// chaque chunk ayant sa propre origine. Fonction PURE (aucun état, aucune API GPU)
// => appelable depuis un worker thread.
[[nodiscard]] std::vector<render::Vertex> generate_rail_mesh(const TrackSource& track,
                                                             double x_start, double x_end,
                                                             const WorldPosition& origin,
                                                             const RailProfile& profile = {});

}  // namespace noire::scene
