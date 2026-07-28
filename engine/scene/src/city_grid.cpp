#include "noire/scene/city_grid.hpp"

#include <cmath>
#include <algorithm>

namespace noire::scene {

namespace {

void add_quad(CityGridMeshData& out, const glm::vec3& p0, const glm::vec3& p1,
              const glm::vec3& p2, const glm::vec3& p3, const glm::vec3& normal,
              const glm::vec2& uv_scale = glm::vec2(1.0f)) {
    const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);

    out.vertices.push_back(render::MeshVertex{p0, normal, glm::vec2(0.0f, 0.0f) * uv_scale, tangent});
    out.vertices.push_back(render::MeshVertex{p1, normal, glm::vec2(1.0f, 0.0f) * uv_scale, tangent});
    out.vertices.push_back(render::MeshVertex{p2, normal, glm::vec2(1.0f, 1.0f) * uv_scale, tangent});
    out.vertices.push_back(render::MeshVertex{p3, normal, glm::vec2(0.0f, 1.0f) * uv_scale, tangent});

    out.indices.push_back(base);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
}

void add_box(CityGridMeshData& out, float x0, float y0, float z0, float x1, float y1, float z1) {
    const glm::vec3 n_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 n_left(-1.0f, 0.0f, 0.0f);
    const glm::vec3 n_right(1.0f, 0.0f, 0.0f);
    const glm::vec3 n_front(0.0f, 0.0f, 1.0f);
    const glm::vec3 n_back(0.0f, 0.0f, -1.0f);

    // Dessus
    add_quad(out, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, n_up);
    // Côtés
    add_quad(out, {x0, y0, z1}, {x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}, n_left);
    add_quad(out, {x1, y0, z0}, {x1, y0, z1}, {x1, y1, z1}, {x1, y1, z0}, n_right);
    add_quad(out, {x1, y0, z1}, {x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1}, n_front);
    add_quad(out, {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, n_back);
}

}  // namespace

CityGridMeshData CityGrid::generate_roads(const WorldPosition& center, double range,
                                           double cell_size, double track_corridor_w) const {
    CityGridMeshData out;
    const float y_road = 0.02f;  // légèrement au-dessus du sol naturel pour éviter le z-fighting

    const long cx0 = static_cast<long>(std::floor(center.x / cell_size));
    const long cz0 = static_cast<long>(std::floor(center.z / cell_size));
    const auto steps = static_cast<long>(std::ceil(range / cell_size));

    const float road_half_w = 5.0f;          // 10m de largeur pour les rues
    const float boulevard_half_w = static_cast<float>(track_corridor_w * 0.5); // boulevard central

    // 1. Boulevard central sous le viaduc (longitudinal)
    const float min_x = static_cast<float>(center.x - range);
    const float max_x = static_cast<float>(center.x + range);

    add_quad(out,
             {min_x, y_road, -boulevard_half_w},
             {max_x, y_road, -boulevard_half_w},
             {max_x, y_road, boulevard_half_w},
             {min_x, y_road, boulevard_half_w},
             {0.0f, 1.0f, 0.0f},
             glm::vec2((max_x - min_x) / 10.0f, boulevard_half_w * 2.0f / 10.0f));

    // 2. Rues transversales en damier (perpendiculaires au viaduc)
    for (long ci = cx0 - steps; ci <= cx0 + steps; ++ci) {
        const float rx = static_cast<float>(static_cast<double>(ci) * cell_size - center.x);
        const float rz_min = static_cast<float>(-range);
        const float rz_max = static_cast<float>(range);

        add_quad(out,
                 {rx - road_half_w, y_road, rz_min},
                 {rx + road_half_w, y_road, rz_min},
                 {rx + road_half_w, y_road, rz_max},
                 {rx - road_half_w, y_road, rz_max},
                 {0.0f, 1.0f, 0.0f},
                 glm::vec2(road_half_w * 2.0f / 10.0f, (rz_max - rz_min) / 10.0f));
    }

    // 3. Rues longitudinales secondaires bordant les pâtés de maisons
    for (long cj = cz0 - steps; cj <= cz0 + steps; ++cj) {
        const float rz = static_cast<float>(static_cast<double>(cj) * cell_size - center.z);
        if (std::abs(rz) < boulevard_half_w) {
            continue;  // déjà couvert par le boulevard central
        }

        add_quad(out,
                 {min_x, y_road, rz - road_half_w},
                 {max_x, y_road, rz - road_half_w},
                 {max_x, y_road, rz + road_half_w},
                 {min_x, y_road, rz + road_half_w},
                 {0.0f, 1.0f, 0.0f},
                 glm::vec2((max_x - min_x) / 10.0f, road_half_w * 2.0f / 10.0f));
    }

    return out;
}

CityGridMeshData CityGrid::generate_sidewalks(const WorldPosition& center, double range,
                                               double cell_size, double track_corridor_w) const {
    CityGridMeshData out;
    const float y0 = 0.02f;
    const float y1 = 0.17f;  // bordure surélevée de 15 cm

    const long cx0 = static_cast<long>(std::floor(center.x / cell_size));
    const long cz0 = static_cast<long>(std::floor(center.z / cell_size));
    const auto steps = static_cast<long>(std::ceil(range / cell_size));

    const float road_half_w = 5.0f;
    const float boulevard_half_w = static_cast<float>(track_corridor_w * 0.5);
    const float sw_width = 3.0f;  // 3m de trottoir bordant chaque bloc

    for (long ci = cx0 - steps; ci <= cx0 + steps; ++ci) {
        for (long cj = cz0 - steps; cj <= cz0 + steps; ++cj) {
            const float bx0 = static_cast<float>(static_cast<double>(ci) * cell_size - center.x) + road_half_w;
            const float bx1 = static_cast<float>(static_cast<double>(ci + 1) * cell_size - center.x) - road_half_w;
            const float bz0 = static_cast<float>(static_cast<double>(cj) * cell_size - center.z) + road_half_w;
            const float bz1 = static_cast<float>(static_cast<double>(cj + 1) * cell_size - center.z) - road_half_w;

            if (bx1 <= bx0 || bz1 <= bz0) {
                continue;
            }
            // Saute les blocs traversant le boulevard du viaduc
            if (std::abs((bz0 + bz1) * 0.5f) < boulevard_half_w) {
                continue;
            }

            // Trottoir entourant le bloc
            // Bord Nord/Sud/Est/Ouest du pavé
            add_box(out, bx0, y0, bz0, bx1, y1, bz0 + sw_width);
            add_box(out, bx0, y0, bz1 - sw_width, bx1, y1, bz1);
            add_box(out, bx0, y0, bz0 + sw_width, bx0 + sw_width, y1, bz1 - sw_width);
            add_box(out, bx1 - sw_width, y0, bz0 + sw_width, bx1, y1, bz1 - sw_width);
        }
    }

    return out;
}

}  // namespace noire::scene
