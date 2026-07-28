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

// Masque binaire d'autotiling pour les connexions routières (N, S, E, W)
namespace RoadBitmask {
    constexpr uint8_t North = 1 << 0;  // 1
    constexpr uint8_t South = 1 << 1;  // 2
    constexpr uint8_t East  = 1 << 2;  // 4
    constexpr uint8_t West  = 1 << 3;  // 8
}

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
    [[nodiscard]] uint8_t road_bitmask(long ci, long cj) const;

    // Génère les maillages d'asphalte autotilés et subdivisés adaptant le relief.
    [[nodiscard]] CityGridMeshData generate_roads(const WorldPosition& center, double range,
                                                 const HeightSampler& height_fn) const;

    // Génère les trottoirs surélevés sans chevauchement avec normales orientées vers le haut.
    [[nodiscard]] CityGridMeshData generate_sidewalks(const WorldPosition& center, double range,
                                                     const HeightSampler& height_fn) const;

    // Positionne les lampadaires aux bordures exactes des trottoirs.
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
