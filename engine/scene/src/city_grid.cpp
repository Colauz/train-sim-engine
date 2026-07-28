#include "noire/scene/city_grid.hpp"

#include <cmath>
#include <algorithm>
#include <iostream>

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

void add_sloped_box(CityGridMeshData& out,
                    const glm::vec3& b0, const glm::vec3& b1, const glm::vec3& b2, const glm::vec3& b3,
                    const glm::vec3& t0, const glm::vec3& t1, const glm::vec3& t2, const glm::vec3& t3) {
    // Normale supérieure STRICTEMENT orientée vers le haut (0, 1, 0) pour un éclairage uniforme sans artefacts PBR
    const glm::vec3 n_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 n_right = glm::normalize(glm::cross(t2 - t1, b1 - t1));
    const glm::vec3 n_left  = glm::normalize(glm::cross(t0 - t3, b3 - t3));

    // Dessus du trottoir
    add_quad(out, t0, t1, t2, t3, n_up);
    // Bordures longitudinales extérieures uniquement (pas de cloisons transversales internes pour éviter les artefacts code-barres)
    add_quad(out, b1, b2, t2, t1, n_right);
    add_quad(out, b3, b0, t0, t3, n_left);
}

inline int emod(long a, int m) {
    const int r = static_cast<int>(a % m);
    return r < 0 ? r + m : r;
}

}  // namespace

CityGrid::CityGrid(double cell_size, int road_period, double corridor_half_w)
    : cell_size_(cell_size), road_period_(road_period), corridor_half_w_(corridor_half_w) {}

CellRole CityGrid::cell_role(long ci, long cj) const {
    const double cz_center = (static_cast<double>(cj) + 0.5) * cell_size_;
    if (std::abs(cz_center) < corridor_half_w_) {
        return CellRole::Corridor;
    }
    if (emod(ci, road_period_) == 0 || emod(cj, road_period_) == 0) {
        return CellRole::Road;
    }
    return CellRole::Plot;
}

uint8_t CityGrid::road_bitmask(long ci, long cj) const {
    if (cell_role(ci, cj) != CellRole::Road) {
        return 0;
    }
    uint8_t mask = 0;
    if (cell_role(ci, cj + 1) == CellRole::Road) mask |= RoadBitmask::North;
    if (cell_role(ci, cj - 1) == CellRole::Road) mask |= RoadBitmask::South;
    if (cell_role(ci + 1, cj) == CellRole::Road) mask |= RoadBitmask::East;
    if (cell_role(ci - 1, cj) == CellRole::Road) mask |= RoadBitmask::West;
    return mask;
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
// Smart Routing / Autotiling des Routes
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_roads(const WorldPosition& center, double range,
                                           const HeightSampler& height_fn) const {
    CityGridMeshData out;

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    const int subdiv = 6;
    const double sub_size = cell_size_ / static_cast<double>(subdiv);
    const double margin = 4.0;  // Largeur de trottoir réservée (chaussée de 16m)

    auto emit_road_patch = [&](double rx0, double rx1, double rz0, double rz1) {
        const int patch_div = 4;
        const double dx = (rx1 - rx0) / static_cast<double>(patch_div);
        const double dz = (rz1 - rz0) / static_cast<double>(patch_div);

        for (int i = 0; i < patch_div; ++i) {
            for (int j = 0; j < patch_div; ++j) {
                const double wx0 = rx0 + static_cast<double>(i) * dx;
                const double wx1 = rx0 + static_cast<double>(i + 1) * dx;
                const double wz0 = rz0 + static_cast<double>(j) * dz;
                const double wz1 = rz0 + static_cast<double>(j + 1) * dz;

                const double terrain_y = height_fn(wx0, wz0);
                const double wy01 = height_fn(wx0, wz1) + 0.05;
                const double wy11 = height_fn(wx1, wz1) + 0.05;
                const double wy10 = height_fn(wx1, wz0) + 0.05;
                const double wy00 = terrain_y + 0.05;

                static int debug_topo_prints = 0;
                if (debug_topo_prints < 5) {
                    std::cout << "[DEBUG TOPO] World X: " << wx0 << " | World Z: " << wz0
                              << " | Terrain Y calculé: " << terrain_y
                              << " | Route Y calculée: " << wy00 << std::endl;
                    debug_topo_prints++;
                }

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
    };

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Road) {
                continue;
            }

            const uint8_t mask = road_bitmask(ci, cj);
            const double cell_wx0 = static_cast<double>(ci) * cell_size_;
            const double cell_wz0 = static_cast<double>(cj) * cell_size_;
            const double cell_wx1 = cell_wx0 + cell_size_;
            const double cell_wz1 = cell_wz0 + cell_size_;

            const bool is_n = (mask & RoadBitmask::North) != 0;
            const bool is_s = (mask & RoadBitmask::South) != 0;
            const bool is_e = (mask & RoadBitmask::East) != 0;
            const bool is_w = (mask & RoadBitmask::West) != 0;

            const int conn_count = (is_n ? 1 : 0) + (is_s ? 1 : 0) + (is_e ? 1 : 0) + (is_w ? 1 : 0);

            // Carrefours (3 ou 4 voies) ou intersections
            if (conn_count >= 3 || (is_n && is_s && is_e && is_w)) {
                emit_road_patch(cell_wx0, cell_wx1, cell_wz0, cell_wz1);
            }
            // Ligne droite Nord-Sud
            else if (is_n || is_s) {
                if (is_e) { // Virage / T-Junction
                    emit_road_patch(cell_wx0 + margin, cell_wx1, cell_wz0, cell_wz1);
                } else if (is_w) {
                    emit_road_patch(cell_wx0, cell_wx1 - margin, cell_wz0, cell_wz1);
                } else {
                    emit_road_patch(cell_wx0 + margin, cell_wx1 - margin, cell_wz0, cell_wz1);
                }
            }
            // Ligne droite Est-Ouest
            else if (is_e || is_w) {
                emit_road_patch(cell_wx0, cell_wx1, cell_wz0 + margin, cell_wz1 - margin);
            }
            // Par défaut
            else {
                emit_road_patch(cell_wx0 + margin, cell_wx1 - margin, cell_wz0 + margin, cell_wz1 - margin);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Trottoirs ajustés sans chevauchement avec normales supérieures (0,1,0)
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_sidewalks(const WorldPosition& center, double range,
                                               const HeightSampler& height_fn) const {
    CityGridMeshData out;
    const double sw_w = 4.0;       // Largeur du trottoir (4m)
    const double sw_h = 0.15;      // Surélévation du trottoir (15cm)
    const int subdiv = 6;
    const double sub_size = cell_size_ / static_cast<double>(subdiv);

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Plot) {
                continue;
            }

            const double cell_wx0 = static_cast<double>(ci) * cell_size_;
            const double cell_wz0 = static_cast<double>(cj) * cell_size_;

            auto emit_edge_segment = [&](double wx0, double wz0, double wx1, double wz1,
                                         double off_x, double off_z) {
                const double wx0_i = wx0 + off_x;
                const double wz0_i = wz0 + off_z;
                const double wx1_i = wx1 + off_x;
                const double wz1_i = wz1 + off_z;

                const double h00 = height_fn(wx0, wz0) + 0.05;
                const double h10 = height_fn(wx1, wz1) + 0.05;
                const double h01 = height_fn(wx0_i, wz0_i) + 0.05;
                const double h11 = height_fn(wx1_i, wz1_i) + 0.05;

                const glm::vec3 b0(static_cast<float>(wx0 - center.x), static_cast<float>(h00 - center.y), static_cast<float>(wz0 - center.z));
                const glm::vec3 b1(static_cast<float>(wx1 - center.x), static_cast<float>(h10 - center.y), static_cast<float>(wz1 - center.z));
                const glm::vec3 b2(static_cast<float>(wx1_i - center.x), static_cast<float>(h11 - center.y), static_cast<float>(wz1_i - center.z));
                const glm::vec3 b3(static_cast<float>(wx0_i - center.x), static_cast<float>(h01 - center.y), static_cast<float>(wz0_i - center.z));

                const glm::vec3 t0 = b0 + glm::vec3(0.0f, static_cast<float>(sw_h), 0.0f);
                const glm::vec3 t1 = b1 + glm::vec3(0.0f, static_cast<float>(sw_h), 0.0f);
                const glm::vec3 t2 = b2 + glm::vec3(0.0f, static_cast<float>(sw_h), 0.0f);
                const glm::vec3 t3 = b3 + glm::vec3(0.0f, static_cast<float>(sw_h), 0.0f);

                add_sloped_box(out, b0, b1, b2, b3, t0, t1, t2, t3);
            };

            // Bord -X
            if (cell_role(ci - 1, cj) == CellRole::Road) {
                for (int s = 0; s < subdiv; ++s) {
                    const double wz_a = cell_wz0 + static_cast<double>(s) * sub_size;
                    const double wz_b = cell_wz0 + static_cast<double>(s + 1) * sub_size;
                    emit_edge_segment(cell_wx0, wz_a, cell_wx0, wz_b, sw_w, 0.0);
                }
            }
            // Bord +X
            if (cell_role(ci + 1, cj) == CellRole::Road) {
                for (int s = 0; s < subdiv; ++s) {
                    const double wz_a = cell_wz0 + static_cast<double>(s) * sub_size;
                    const double wz_b = cell_wz0 + static_cast<double>(s + 1) * sub_size;
                    emit_edge_segment(cell_wx0 + cell_size_, wz_a, cell_wx0 + cell_size_, wz_b, -sw_w, 0.0);
                }
            }
            // Bord -Z
            if (cell_role(ci, cj - 1) == CellRole::Road) {
                for (int s = 0; s < subdiv; ++s) {
                    const double wx_a = cell_wx0 + static_cast<double>(s) * sub_size;
                    const double wx_b = cell_wx0 + static_cast<double>(s + 1) * sub_size;
                    emit_edge_segment(wx_a, cell_wz0, wx_b, cell_wz0, 0.0, sw_w);
                }
            }
            // Bord +Z
            if (cell_role(ci, cj + 1) == CellRole::Road) {
                for (int s = 0; s < subdiv; ++s) {
                    const double wx_a = cell_wx0 + static_cast<double>(s) * sub_size;
                    const double wx_b = cell_wx0 + static_cast<double>(s + 1) * sub_size;
                    emit_edge_segment(wx_a, cell_wz0 + cell_size_, wx_b, cell_wz0 + cell_size_, 0.0, -sw_w);
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
    const double sw_offset = 2.0;  // Au milieu du trottoir de 4m

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
