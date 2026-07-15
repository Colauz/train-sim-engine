#include "noire/scene/track_mesh.hpp"

#include <algorithm>
#include <cmath>

namespace noire::scene {

namespace {

// Une section transversale du rail : 4 coins + le centre de la section, qui sert à
// orienter les normales vers l'EXTÉRIEUR sans avoir à raisonner sur le winding.
struct Ring {
    glm::vec3 bl, br, tr, tl;
    glm::vec3 mid;
    float u = 0.0f;  // coordonnée de texture le long du rail
};

Ring make_ring(const glm::vec3& center, const glm::vec3& right, float half_width, float height,
               float u) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    Ring ring;
    ring.bl = center - right * half_width;
    ring.br = center + right * half_width;
    ring.tr = center + right * half_width + up * height;
    ring.tl = center - right * half_width + up * height;
    ring.mid = center + up * (height * 0.5f);
    ring.u = u;
    return ring;
}

// Émet un quad (a,b,c,d) où a->b suit le rail (u croissant) et a->d traverse la face
// (v croissant). Normale et tangente sont déduites de la GÉOMÉTRIE : la normale est
// retournée si besoin pour pointer à l'opposé de `mid`, donc toujours vers l'extérieur.
void add_quad(RailMeshData& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
              const glm::vec3& d, float u_a, float u_b, const glm::vec3& mid) {
    const glm::vec3 tangent = glm::normalize(b - a);    // sens des u
    const glm::vec3 across = glm::normalize(d - a);     // sens des v
    glm::vec3 normal = glm::normalize(glm::cross(tangent, across));

    const glm::vec3 quad_center = (a + b + c + d) * 0.25f;
    if (glm::dot(normal, quad_center - mid) < 0.0f) {
        normal = -normal;
    }
    // Handedness glTF : la bitangente reconstruite par cross(N,T)*w doit suivre v.
    const float w = glm::dot(glm::cross(normal, tangent), across) < 0.0f ? -1.0f : 1.0f;
    const glm::vec4 tangent4(tangent, w);

    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    out.vertices.push_back(render::MeshVertex{a, normal, {u_a, 0.0f}, tangent4});
    out.vertices.push_back(render::MeshVertex{b, normal, {u_b, 0.0f}, tangent4});
    out.vertices.push_back(render::MeshVertex{c, normal, {u_b, 1.0f}, tangent4});
    out.vertices.push_back(render::MeshVertex{d, normal, {u_a, 1.0f}, tangent4});
    out.indices.insert(out.indices.end(),
                       {base, base + 1, base + 2, base, base + 2, base + 3});
}

// Relie deux sections par les 4 faces du rail. Les sommets ne sont PAS partagés entre
// faces : chaque face garde sa propre normale => arêtes franches (un rail est anguleux).
void connect(RailMeshData& out, const Ring& a, const Ring& b) {
    const glm::vec3 mid = (a.mid + b.mid) * 0.5f;
    add_quad(out, a.bl, b.bl, b.br, a.br, a.u, b.u, mid);  // dessous
    add_quad(out, a.br, b.br, b.tr, a.tr, a.u, b.u, mid);  // flanc
    add_quad(out, a.tr, b.tr, b.tl, a.tl, a.u, b.u, mid);  // dessus (table de roulement)
    add_quad(out, a.tl, b.tl, b.bl, a.bl, a.u, b.u, mid);  // flanc opposé
}

}  // namespace

RailMeshData generate_rail_mesh(const TrackSource& track, double x_start, double x_end,
                                const WorldPosition& origin, const RailProfile& profile) {
    RailMeshData out;
    const double span = x_end - x_start;
    if (span <= 0.0) {
        return out;
    }

    const double step = (profile.sample_step > 0.01) ? profile.sample_step : 1.0;
    const int count = std::max(2, static_cast<int>(std::ceil(span / step)) + 1);
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const float half_gauge = static_cast<float>(profile.gauge * 0.5);
    const float uv_period = profile.uv_period > 0.01f ? profile.uv_period : 1.0f;

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
        // u suit le CHAINAGE absolu : deux tuiles voisines raccordent sans couture.
        const float u = static_cast<float>((x - x_start) / static_cast<double>(uv_period));

        rail_a.push_back(make_ring(center + right * half_gauge, right, profile.rail_half_width,
                                   profile.rail_height, u));
        rail_b.push_back(make_ring(center - right * half_gauge, right, profile.rail_half_width,
                                   profile.rail_height, u));
    }

    // 2 rails x 4 faces x 4 sommets (et 6 indices) par intervalle.
    const auto segments = static_cast<std::size_t>(count - 1);
    out.vertices.reserve(segments * 2 * 4 * 4);
    out.indices.reserve(segments * 2 * 4 * 6);
    for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(count); ++i) {
        connect(out, rail_a[i], rail_a[i + 1]);
        connect(out, rail_b[i], rail_b[i + 1]);
    }
    return out;
}

}  // namespace noire::scene
