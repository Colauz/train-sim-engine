#include "noire/scene/city_grid.hpp"

#include <cmath>
#include <algorithm>

namespace noire::scene {

// ---------------------------------------------------------------------------
// Helpers géométriques
// ---------------------------------------------------------------------------
namespace {

void add_quad(CityGridMeshData& out, const glm::vec3& p0, const glm::vec3& p1,
              const glm::vec3& p2, const glm::vec3& p3, const glm::vec3& normal) {
    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec3 n = normal;

    out.vertices.push_back(render::MeshVertex{p0, n, {0.0f, 0.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p1, n, {1.0f, 0.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p2, n, {1.0f, 1.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p3, n, {0.0f, 1.0f}, tangent});

    out.indices.push_back(base);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
}

void add_box(CityGridMeshData& out, float x0, float y0, float z0,
             float x1, float y1, float z1) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 dn(0.0f, -1.0f, 0.0f);
    const glm::vec3 nx_neg(-1.0f, 0.0f, 0.0f);
    const glm::vec3 nx_pos( 1.0f, 0.0f, 0.0f);
    const glm::vec3 nz_neg(0.0f, 0.0f, -1.0f);
    const glm::vec3 nz_pos(0.0f, 0.0f,  1.0f);

    // Dessus
    add_quad(out, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, up);
    // Côtés
    add_quad(out, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, nx_neg);
    add_quad(out, {x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, nx_pos);
    add_quad(out, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, nz_pos);
    add_quad(out, {x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, nz_neg);
}

// Modulo euclidien (toujours >= 0) pour les indices de cellule négatifs.
inline int emod(long a, int m) {
    const int r = static_cast<int>(a % m);
    return r < 0 ? r + m : r;
}

}  // namespace

// ---------------------------------------------------------------------------
// CityGrid
// ---------------------------------------------------------------------------

CityGrid::CityGrid(double cell_size, int road_period, double corridor_half_w)
    : cell_size_(cell_size), road_period_(road_period), corridor_half_w_(corridor_half_w) {}

CellRole CityGrid::cell_role(long ci, long cj) const {
    // Le centre-monde de la cellule (ci, cj) en Z.
    const double cz_center = (static_cast<double>(cj) + 0.5) * cell_size_;
    if (std::abs(cz_center) < corridor_half_w_) {
        return CellRole::Corridor;
    }
    // Une cellule est une ROUTE si son indice modulo road_period_ == 0 sur l'un des axes.
    if (emod(ci, road_period_) == 0 || emod(cj, road_period_) == 0) {
        return CellRole::Road;
    }
    return CellRole::Plot;
}

bool CityGrid::is_plot(double wx, double wz) const {
    const long ci = static_cast<long>(std::floor(wx / cell_size_));
    const long cj = static_cast<long>(std::floor(wz / cell_size_));
    return cell_role(ci, cj) == CellRole::Plot;
}

// ---------------------------------------------------------------------------
// Routes : un quad plat par cellule de type Road, posé à Y = 0.01.
// Coordonnées en espace caméra-relatif (center → 0, 0, 0).
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_roads(const WorldPosition& center, double range) const {
    CityGridMeshData out;
    const float y = 0.01f;  // juste au-dessus du sol pour éviter le z-fighting
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Road) {
                continue;
            }
            // Coins de la cellule en espace caméra-relatif.
            const float x0 = static_cast<float>(static_cast<double>(ci) * cell_size_ - center.x);
            const float x1 = static_cast<float>(static_cast<double>(ci + 1) * cell_size_ - center.x);
            const float z0 = static_cast<float>(static_cast<double>(cj) * cell_size_ - center.z);
            const float z1 = static_cast<float>(static_cast<double>(cj + 1) * cell_size_ - center.z);

            add_quad(out, {x0, y, z1}, {x1, y, z1}, {x1, y, z0}, {x0, y, z0}, up);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Trottoirs : bordure surélevée (15 cm) sur chaque bord d'une cellule PARCELLE
// qui touche une cellule ROUTE. Largeur du trottoir : 2 m.
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_sidewalks(const WorldPosition& center, double range) const {
    CityGridMeshData out;
    const float sw_w = 2.0f;     // largeur du trottoir en mètres
    const float y0 = 0.01f;      // base (niveau du sol)
    const float y1 = 0.16f;      // dessus (surélevé de 15 cm)

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    const auto cs = static_cast<float>(cell_size_);

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Plot) {
                continue;
            }
            const float x0f = static_cast<float>(static_cast<double>(ci) * cell_size_ - center.x);
            const float z0f = static_cast<float>(static_cast<double>(cj) * cell_size_ - center.z);

            // Bord -X : si la cellule (ci-1, cj) est une Route
            if (cell_role(ci - 1, cj) == CellRole::Road) {
                add_box(out, x0f, y0, z0f, x0f + sw_w, y1, z0f + cs);
            }
            // Bord +X
            if (cell_role(ci + 1, cj) == CellRole::Road) {
                add_box(out, x0f + cs - sw_w, y0, z0f, x0f + cs, y1, z0f + cs);
            }
            // Bord -Z
            if (cell_role(ci, cj - 1) == CellRole::Road) {
                add_box(out, x0f, y0, z0f, x0f + cs, y1, z0f + sw_w);
            }
            // Bord +Z
            if (cell_role(ci, cj + 1) == CellRole::Road) {
                add_box(out, x0f, y0, z0f + cs - sw_w, x0f + cs, y1, z0f + cs);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Lampadaires : un par bord de cellule PARCELLE qui touche une ROUTE, le long
// de l'axe du bord, espacés tous les ~cell_size mètres. Le lampadaire est
// centré sur le trottoir, orienté vers la route.
// ---------------------------------------------------------------------------
std::vector<LampPost> CityGrid::generate_lamppost_positions(const WorldPosition& center,
                                                             double range) const {
    std::vector<LampPost> posts;
    const double half = cell_size_ * 0.5;
    const double sw_offset = 1.0;  // au milieu du trottoir (2 m de large)

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Plot) {
                continue;
            }
            const double cx = (static_cast<double>(ci) + 0.5) * cell_size_;
            const double cz = (static_cast<double>(cj) + 0.5) * cell_size_;

            // Bord -X (route à gauche) : lampadaire au milieu du bord, orienté vers -X
            if (cell_role(ci - 1, cj) == CellRole::Road) {
                posts.push_back({cx - half + sw_offset, cz, 1.5708f});  // face -X
            }
            // Bord +X
            if (cell_role(ci + 1, cj) == CellRole::Road) {
                posts.push_back({cx + half - sw_offset, cz, -1.5708f});  // face +X
            }
            // Bord -Z
            if (cell_role(ci, cj - 1) == CellRole::Road) {
                posts.push_back({cx, cz - half + sw_offset, 0.0f});  // face -Z
            }
            // Bord +Z
            if (cell_role(ci, cj + 1) == CellRole::Road) {
                posts.push_back({cx, cz + half - sw_offset, 3.14159f});  // face +Z
            }
        }
    }
    return posts;
}

}  // namespace noire::scene
