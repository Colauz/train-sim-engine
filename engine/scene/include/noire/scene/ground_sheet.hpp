#pragma once

#include <functional>

#include "noire/core/math.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire::scene {

using HeightSampler = std::function<double(double, double)>;

// M48 — Nappe de sol urbain : un simple maillage subdivisé qui épouse le terrain
// (+2 cm, anti Z-fighting avec l'herbe), dessiné en gris béton foncé. Décision de
// game design : plus de routes, plus de trottoirs, plus de lampadaires — le sol de
// la ville est UNE dalle, les immeubles poussent dessus. Fonction PURE.
[[nodiscard]] RailMeshData generate_ground_sheet(const HeightSampler& height_fn,
                                                 const WorldPosition& center,
                                                 double range, double cell = 25.0);

}  // namespace noire::scene
