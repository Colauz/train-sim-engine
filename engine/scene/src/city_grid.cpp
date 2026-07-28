#include "noire/scene/city_grid.hpp"

#include <cmath>
#include <algorithm>

namespace noire::scene {

namespace {

void add_quad(CityGridMeshData& out, const glm::vec3& p0, const glm::vec3& p1,
              const glm::vec3& p2, const glm::vec3& p3, const glm::vec3& normal) {
    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);

    out.vertices.push_back(render::MeshVertex{p0, normal, {0.0f, 0.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p1, normal, {1.0f, 0.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p2, normal, {1.0f, 1.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p3, normal, {0.0f, 1.0f}, tangent});

    out.indices.push_back(base);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
}

inline int emod(long a, int m) {
    const int r = static_cast<int>(a % m);
    return r < 0 ? r + m : r;
}

}  // namespace

CityGrid::CityGrid(double cell_size, int road_period)
    : cell_size_(cell_size), road_period_(road_period) {}

CellRole CityGrid::cell_role(long ci, long cj) const {
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

bool CityGrid::is_plot_footprint(double wx, double wz, double half_w, double half_d) const {
    if (!is_plot(wx, wz)) return false;
    if (!is_plot(wx - half_w, wz - half_d)) return false;
    if (!is_plot(wx + half_w, wz - half_d)) return false;
    if (!is_plot(wx + half_w, wz + half_d)) return false;
    if (!is_plot(wx - half_w, wz + half_d)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Routes (M44/M46) : un quad d'asphalte plat par cellule ROUTE, à terrain + 0,05 m.
// Subdivision 4x4 pour épouser le relief — et pour CONTOURNER LES PILIERS : tout
// sous-quad dont la boîte touche l'empreinte d'un pilier est omis (ÉTAPE 2 du
// pipeline M46 : une route passe sous le viaduc, jamais à travers le béton).
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_roads(const WorldPosition& center, double range,
                                           const HeightSampler& height_fn,
                                           const std::vector<PillarBox>& pillars) const {
    CityGridMeshData out;

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    constexpr int patch_div = 4;

    auto hits_pillar = [&](double wx0, double wz0, double wx1, double wz1) {
        for (const PillarBox& p : pillars) {
            if (wx0 < p.x + p.half && wx1 > p.x - p.half &&
                wz0 < p.z + p.half && wz1 > p.z - p.half) {
                return true;
            }
        }
        return false;
    };

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Road) {
                continue;
            }

            const double cell_wx0 = static_cast<double>(ci) * cell_size_;
            const double cell_wz0 = static_cast<double>(cj) * cell_size_;
            const double dx = cell_size_ / static_cast<double>(patch_div);

            for (int i = 0; i < patch_div; ++i) {
                for (int j = 0; j < patch_div; ++j) {
                    const double wx0 = cell_wx0 + static_cast<double>(i) * dx;
                    const double wx1 = wx0 + dx;
                    const double wz0 = cell_wz0 + static_cast<double>(j) * dx;
                    const double wz1 = wz0 + dx;

                    if (hits_pillar(wx0, wz0, wx1, wz1)) {
                        continue;  // la chaussée s'arrête autour du pilier
                    }

                    const double wy01 = height_fn(wx0, wz1) + 0.05;
                    const double wy11 = height_fn(wx1, wz1) + 0.05;
                    const double wy10 = height_fn(wx1, wz0) + 0.05;
                    const double wy00 = height_fn(wx0, wz0) + 0.05;

                    const glm::vec3 p0(static_cast<float>(wx0 - center.x), static_cast<float>(wy01 - center.y), static_cast<float>(wz1 - center.z));
                    const glm::vec3 p1(static_cast<float>(wx1 - center.x), static_cast<float>(wy11 - center.y), static_cast<float>(wz1 - center.z));
                    const glm::vec3 p2(static_cast<float>(wx1 - center.x), static_cast<float>(wy10 - center.y), static_cast<float>(wz0 - center.z));
                    const glm::vec3 p3(static_cast<float>(wx0 - center.x), static_cast<float>(wy00 - center.y), static_cast<float>(wz0 - center.z));

                    glm::vec3 normal = glm::cross(p2 - p0, p1 - p0);
                    if (glm::length(normal) > 1e-4f) {
                        normal = glm::normalize(normal);
                    } else {
                        normal = glm::vec3(0.0f, 1.0f, 0.0f);
                    }

                    add_quad(out, p0, p1, p2, p3, normal);
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Alignement des lampadaires sur les bordures
// ---------------------------------------------------------------------------
std::vector<LampPost> CityGrid::generate_lamppost_positions(const WorldPosition& center,
                                                             double range,
                                                             const HeightSampler& height_fn) const {
    std::vector<LampPost> posts;
    const double half = cell_size_ * 0.5;
    const double sw_offset = 2.0;  // retrait de 2 m depuis le bord de la parcelle

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

            auto add_post = [&](double wx, double wz, float yaw) {
                const double wy = height_fn(wx, wz) + 0.20;
                posts.push_back({wx, wy, wz, yaw});
            };

            if (cell_role(ci - 1, cj) == CellRole::Road) {
                add_post(cx - half + sw_offset, cz, 1.5708f);
            }
            if (cell_role(ci + 1, cj) == CellRole::Road) {
                add_post(cx + half - sw_offset, cz, -1.5708f);
            }
            if (cell_role(ci, cj - 1) == CellRole::Road) {
                add_post(cx, cz - half + sw_offset, 0.0f);
            }
            if (cell_role(ci, cj + 1) == CellRole::Road) {
                add_post(cx, cz + half - sw_offset, 3.14159f);
            }
        }
    }
    return posts;
}

}  // namespace noire::scene
