#pragma once

#include <cstdint>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

// --- Système de Grille Urbaine (M38) ----------------------------------------
// Le monde sous le viaduc est divisé en cellules de taille fixe. Chaque cellule
// a un rôle unique : ROUTE ou PARCELLE. Les routes forment un damier régulier ;
// les bâtiments ne peuvent apparaître que sur les parcelles.
//
// Grille en X (le long de la voie) :
//   Chaque N-ième colonne est une ROUTE ; les colonnes intermédiaires sont des
//   PARCELLES.
//
// Grille en Z (perpendiculaire à la voie) :
//   Chaque M-ième rangée est une ROUTE ; les rangées intermédiaires sont des
//   PARCELLES.
//
// Un boulevard central (corridor du viaduc) n'est PAS une cellule — c'est un
// espace vide exclu du damier (pas de route, pas de bâtiment).

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

    // Génère les quads d'asphalte pour toutes les cellules ROUTE dans un rayon autour de `center`.
    // Les coordonnées sont en espace caméra-relatif (centre = 0,0,0). Y = 0.01.
    [[nodiscard]] CityGridMeshData generate_roads(const WorldPosition& center, double range) const;

    // Génère les trottoirs (bordures surélevées) aux frontières ROUTE ↔ PARCELLE.
    [[nodiscard]] CityGridMeshData generate_sidewalks(const WorldPosition& center, double range) const;

    // Retourne les positions de lampadaires aux frontières ROUTE ↔ PARCELLE, le long des rues.
    [[nodiscard]] std::vector<LampPost> generate_lamppost_positions(const WorldPosition& center,
                                                                    double range) const;

    // Test : est-ce qu'une position monde (wx, wz) tombe sur une cellule PARCELLE ?
    [[nodiscard]] bool is_plot(double wx, double wz) const;

    [[nodiscard]] double cell_size() const { return cell_size_; }

private:
    double cell_size_;
    int road_period_;
    double corridor_half_w_;
};

}  // namespace noire::scene
