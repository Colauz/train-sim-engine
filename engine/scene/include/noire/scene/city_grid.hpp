#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

using HeightSampler = std::function<double(double, double)>;

// M46 — Carte d'occupation spatiale. Chaque cellule de la grille a UN rôle, décidé
// dans l'ordre strict : (1) VIADUC/PILIER d'abord, (2) ROUTE ensuite (autorisée sous
// le viaduc, jamais sur un pilier), (3) IMMEUBLE seulement sur une cellule VIDE.
enum class CellRole : std::uint8_t {
    Road,
    Plot,
};

// Boîte d'occupation au sol d'un pilier de viaduc (ÉTAPE 1 du pipeline M46) : la
// chaussée s'arrête autour, jamais à travers le béton.
struct PillarBox {
    double x;
    double z;
    double half;  // demi-côté de l'empreinte, marge de sécurité comprise
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
    explicit CityGrid(double cell_size = 30.0, int road_period = 3);

    [[nodiscard]] CellRole cell_role(long ci, long cj) const;

    // Génère les quads d'asphalte plats : un patch pleine cellule par cellule ROUTE,
    // posé à terrain + 0,05 m (anti Z-fighting). Les sous-quads qui toucheraient
    // l'empreinte d'un PILIER sont omis : la route contourne le béton (M46).
    [[nodiscard]] CityGridMeshData generate_roads(const WorldPosition& center, double range,
                                                 const HeightSampler& height_fn,
                                                 const std::vector<PillarBox>& pillars) const;

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
};

}  // namespace noire::scene
