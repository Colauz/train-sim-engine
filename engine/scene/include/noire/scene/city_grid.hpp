#pragma once

#include <cstdint>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

struct CityGridMeshData {
    std::vector<render::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;

    [[nodiscard]] bool valid() const { return !vertices.empty() && !indices.empty(); }
};

class CityGrid {
public:
    CityGrid() = default;

    // Engendre la chaussée en asphalte sombre autour d'une origine monde.
    [[nodiscard]] CityGridMeshData generate_roads(const WorldPosition& center, double range,
                                                 double cell_size, double track_corridor_w) const;

    // Engendre les trottoirs surélevés en béton clair autour d'une origine monde.
    [[nodiscard]] CityGridMeshData generate_sidewalks(const WorldPosition& center, double range,
                                                     double cell_size, double track_corridor_w) const;
};

}  // namespace noire::scene
