#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

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
    explicit CityGrid(double cell_size = 24.0, int road_period = 3,
                      double corridor_half_w = 18.0);

    [[nodiscard]] CellRole cell_role(long ci, long cj) const;

    // Génère les quads d'asphalte plats : un patch pleine cellule par cellule ROUTE,
    // posé à terrain + 0,05 m (anti Z-fighting). Rien d'autre ne touche le sol (M44).
    [[nodiscard]] CityGridMeshData generate_roads(const WorldPosition& center, double range,
                                                 const HeightSampler& height_fn) const;

    // Positionne les lampadaires aux bordures des routes.
    [[nodiscard]] std::vector<LampPost> generate_lamppost_positions(const WorldPosition& center,
                                                                    double range,
                                                                    const HeightSampler& height_fn) const;

    // Test d'exclusivité : est-ce qu'une position monde (wx, wz) tombe sur une cellule PARCELLE ?
    [[nodiscard]] bool is_plot(double wx, double wz) const;

    // Test d'exclusivité d'empreinte (footprint) complète pour un bâtiment.
    [[nodiscard]] bool is_plot_footprint(double wx, double wz, double half_w, double half_d) const;

    [[nodiscard]] double cell_size() const { return cell_size_; }

private:
    double cell_size_;
    int road_period_;
    double corridor_half_w_;
};

}  // namespace noire::scene
