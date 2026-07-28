#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

// Sampling function for terrain elevation at world coordinates (wx, wz).
using HeightSampler = std::function<double(double, double)>;

enum class CellRole : std::uint8_t {
    Road,
    Plot,
    Corridor  // zone d'exclusion du viaduc
};

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

class CityGrid {
public:
    // cell_size : côté d'une cellule en mètres (par ex. 24 m).
    // road_period : une colonne/rangée sur N est une route (par ex. 3 => 1 route, 2 parcelles).
    // corridor_half_w : demi-largeur du boulevard central (zone d'exclusion du viaduc).
    explicit CityGrid(double cell_size = 24.0, int road_period = 3,
                      double corridor_half_w = 18.0);

    [[nodiscard]] CellRole cell_role(long ci, long cj) const;

    // Génère les maillages d'asphalte subdivisés en sub-patches adaptant le relief du terrain.
    [[nodiscard]] CityGridMeshData generate_roads(const WorldPosition& center, double range,
                                                 const HeightSampler& height_fn) const;

    // Génère les trottoirs subdivisés aux frontières ROUTE ↔ PARCELLE épousant la hauteur du sol.
    [[nodiscard]] CityGridMeshData generate_sidewalks(const WorldPosition& center, double range,
                                                     const HeightSampler& height_fn) const;

    // Retourne les positions 3D des lampadaires avec l'altitude du sol échantillonnée.
    [[nodiscard]] std::vector<LampPost> generate_lamppost_positions(const WorldPosition& center,
                                                                    double range,
                                                                    const HeightSampler& height_fn) const;

    // Test : est-ce qu'une position monde (wx, wz) tombe sur une cellule PARCELLE ?
    [[nodiscard]] bool is_plot(double wx, double wz) const;

    [[nodiscard]] double cell_size() const { return cell_size_; }

private:
    double cell_size_;
    int road_period_;
    double corridor_half_w_;
};

}  // namespace noire::scene
