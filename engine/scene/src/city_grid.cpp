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

void add_sloped_box(CityGridMeshData& out,
                    const glm::vec3& b0, const glm::vec3& b1, const glm::vec3& b2, const glm::vec3& b3,
                    const glm::vec3& t0, const glm::vec3& t1, const glm::vec3& t2, const glm::vec3& t3) {
    // b0..b3 = coins de la base, t0..t3 = coins du dessus
    const glm::vec3 n_up = glm::normalize(glm::cross(t2 - t0, t1 - t0));
    const glm::vec3 n_front = glm::normalize(glm::cross(t1 - t0, b0 - t0));
    const glm::vec3 n_right = glm::normalize(glm::cross(t2 - t1, b1 - t1));
    const glm::vec3 n_back  = glm::normalize(glm::cross(t3 - t2, b2 - t2));
    const glm::vec3 n_left  = glm::normalize(glm::cross(t0 - t3, b3 - t3));

    // Dessus
    add_quad(out, t0, t1, t2, t3, n_up);
    // Côtés (front, right, back, left)
    add_quad(out, b0, b1, t1, t0, n_front);
    add_quad(out, b1, b2, t2, t1, n_right);
    add_quad(out, b2, b3, t3, t2, n_back);
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

bool CityGrid::is_plot(double wx, double wz) const {
    const long ci = static_cast<long>(std::floor(wx / cell_size_));
    const long cj = static_cast<long>(std::floor(wz / cell_size_));
    return cell_role(ci, cj) == CellRole::Plot;
}

// ---------------------------------------------------------------------------
// Routes subdivisées adaptant l'altitude du terrain
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_roads(const WorldPosition& center, double range,
                                           const HeightSampler& height_fn) const {
    CityGridMeshData out;

    const long ci_min = static_cast<long>(std::floor((center.x - range) / cell_size_));
    const long ci_max = static_cast<long>(std::floor((center.x + range) / cell_size_));
    const long cj_min = static_cast<long>(std::floor((center.z - range) / cell_size_));
    const long cj_max = static_cast<long>(std::floor((center.z + range) / cell_size_));

    const int subdiv = 6;  // Subdivise chaque cellule de 24m en 6x6 dalles de 4m
    const double sub_size = cell_size_ / static_cast<double>(subdiv);

    for (long ci = ci_min; ci <= ci_max; ++ci) {
        for (long cj = cj_min; cj <= cj_max; ++cj) {
            if (cell_role(ci, cj) != CellRole::Road) {
                continue;
            }

            const double cell_wx0 = static_cast<double>(ci) * cell_size_;
            const double cell_wz0 = static_cast<double>(cj) * cell_size_;

            for (int si = 0; si < subdiv; ++si) {
                for (int sj = 0; sj < subdiv; ++sj) {
                    const double wx0 = cell_wx0 + static_cast<double>(si) * sub_size;
                    const double wx1 = cell_wx0 + static_cast<double>(si + 1) * sub_size;
                    const double wz0 = cell_wz0 + static_cast<double>(sj) * sub_size;
                    const double wz1 = cell_wz0 + static_cast<double>(sj + 1) * sub_size;

                    // Échantillonnage de la hauteur du relief + 0.05m anti-z-fighting
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
// Trottoirs surélevés subdivisés épousant le relief
// ---------------------------------------------------------------------------
CityGridMeshData CityGrid::generate_sidewalks(const WorldPosition& center, double range,
                                               const HeightSampler& height_fn) const {
    CityGridMeshData out;
    const double sw_w = 2.0;       // largeur du trottoir (2m)
    const double sw_h = 0.15;      // hauteur surélevée (15cm)
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
// Positionnement 3D des lampadaires avec l'altitude du relief
// ---------------------------------------------------------------------------
std::vector<LampPost> CityGrid::generate_lamppost_positions(const WorldPosition& center,
                                                             double range,
                                                             const HeightSampler& height_fn) const {
    std::vector<LampPost> posts;
    const double half = cell_size_ * 0.5;
    const double sw_offset = 1.0;

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
                const double wy = height_fn(wx, wz) + 0.20;  // sur le trottoir
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
