#include "noire/scene/track_mesh.hpp"

#include <algorithm>
#include <cmath>

namespace noire::scene {

namespace {

struct Ring {
    glm::vec3 bl, br, tr, tl;
};

Ring make_ring(const glm::vec3& center, const glm::vec3& right, float half_width, float height) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    Ring ring;
    ring.bl = center - right * half_width;
    ring.br = center + right * half_width;
    ring.tr = center + right * half_width + up * height;
    ring.tl = center - right * half_width + up * height;
    return ring;
}

void add_quad(std::vector<render::Vertex>& out, const glm::vec3& a, const glm::vec3& b,
              const glm::vec3& c, const glm::vec3& d, const glm::vec3& color) {
    out.push_back({a, color});
    out.push_back({b, color});
    out.push_back({c, color});
    out.push_back({a, color});
    out.push_back({c, color});
    out.push_back({d, color});
}

void connect(std::vector<render::Vertex>& out, const Ring& a, const Ring& b,
             const glm::vec3& color) {
    add_quad(out, a.bl, b.bl, b.br, a.br, color * 0.75f);
    add_quad(out, a.br, b.br, b.tr, a.tr, color);
    add_quad(out, a.tr, b.tr, b.tl, a.tl, color * 1.10f);
    add_quad(out, a.tl, b.tl, b.bl, a.bl, color);
}

}  // namespace

std::vector<render::Vertex> generate_rail_mesh(const TrackSource& track, double x_start,
                                               double x_end, const WorldPosition& origin,
                                               const RailProfile& profile) {
    std::vector<render::Vertex> vertices;
    const double span = x_end - x_start;
    if (span <= 0.0) {
        return vertices;
    }

    const double step = (profile.sample_step > 0.01) ? profile.sample_step : 1.0;
    const int count = std::max(2, static_cast<int>(std::ceil(span / step)) + 1);
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const float half_gauge = static_cast<float>(profile.gauge * 0.5);

    std::vector<Ring> rail_a;
    std::vector<Ring> rail_b;
    rail_a.reserve(static_cast<std::size_t>(count));
    rail_b.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const double x = x_start + (static_cast<double>(i) / (count - 1)) * span;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(x, pos_world, tangent);

        const glm::vec3 center = glm::vec3(pos_world - origin);  // monde double -> local float
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));

        rail_a.push_back(make_ring(center + right * half_gauge, right, profile.rail_half_width,
                                   profile.rail_height));
        rail_b.push_back(make_ring(center - right * half_gauge, right, profile.rail_half_width,
                                   profile.rail_height));
    }

    vertices.reserve(static_cast<std::size_t>(count - 1) * 2 * 4 * 6);
    for (int i = 0; i + 1 < count; ++i) {
        const std::size_t a = static_cast<std::size_t>(i);
        connect(vertices, rail_a[a], rail_a[a + 1], profile.color);
        connect(vertices, rail_b[a], rail_b[a + 1], profile.color);
    }
    return vertices;
}

}  // namespace noire::scene
