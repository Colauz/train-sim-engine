#pragma once

#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/spline.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

struct RailProfile {
    double gauge = 1.435;            // écartement standard (m), entre axes des rails
    float rail_half_width = 0.06f;   // demi-largeur d'un rail (m)
    float rail_height = 0.12f;       // hauteur d'un rail (m)
    double sample_step = 1.0;        // pas d'échantillonnage le long de la voie (m)
    glm::vec3 color{0.55f, 0.57f, 0.62f};
};

// Extrude deux rails le long de la spline (blocs rectangulaires). Les sommets sont
// exprimés RELATIVEMENT à `origin` et convertis en float => compatible origine
// flottante. À appeler UNE SEULE FOIS (à l'initialisation), jamais par frame.
[[nodiscard]] std::vector<render::Vertex> generate_rail_mesh(const Spline& spline,
                                                             const WorldPosition& origin,
                                                             const RailProfile& profile = {});

}  // namespace noire::scene
