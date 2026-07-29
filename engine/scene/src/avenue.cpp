#include "noire/scene/avenue.hpp"

#include <algorithm>
#include <cmath>

namespace noire::scene {

namespace {

void add_quad(CityGridMeshData& out, const glm::vec3& p0, const glm::vec3& p1,
              const glm::vec3& p2, const glm::vec3& p3) {
    // Normale STRICTEMENT verticale : chaussée et trottoirs s'éclairent uniformément,
    // sans artefacts PBR même quand le terrain ondule sous le quad.
    const glm::vec3 n(0.0f, 1.0f, 0.0f);
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
    const auto base = static_cast<std::uint32_t>(out.vertices.size());

    out.vertices.push_back(render::MeshVertex{p0, n, {0.0f, 0.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p1, n, {1.0f, 0.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p2, n, {1.0f, 1.0f}, tangent});
    out.vertices.push_back(render::MeshVertex{p3, n, {0.0f, 1.0f}, tangent});

    out.indices.insert(out.indices.end(),
                       {base, base + 1, base + 2, base, base + 2, base + 3});
}

// Un tronçon de bande de roulement posée sur le terrain, entre les chainages s0 et s1,
// centrée sur `center`/`right`, de demi-largeur `half`. `lift` = surélévation (0,05
// pour la chaussée, 0,05 + 0,15 pour un trottoir). Les Y sont échantillonnés au sol.
void emit_strip(CityGridMeshData& out, const HeightSampler& height_fn,
                const WorldPosition& origin, const glm::dvec3& c0, const glm::dvec3& r0,
                const glm::dvec3& c1, const glm::dvec3& r1, double off_a, double off_b,
                double lift) {
    const glm::dvec3 a0 = c0 + r0 * off_a, b0 = c0 + r0 * off_b;
    const glm::dvec3 a1 = c1 + r1 * off_a, b1 = c1 + r1 * off_b;
    const double ya0 = height_fn(a0.x, a0.z) + lift, yb0 = height_fn(b0.x, b0.z) + lift;
    const double ya1 = height_fn(a1.x, a1.z) + lift, yb1 = height_fn(b1.x, b1.z) + lift;

    const auto rel = [&](double x, double y, double z) {
        return glm::vec3(glm::dvec3(x, y, z) - origin);
    };
    add_quad(out, rel(a0.x, ya0, a0.z), rel(a1.x, ya1, a1.z), rel(b1.x, yb1, b1.z),
             rel(b0.x, yb0, b0.z));
}

}  // namespace

AvenueData generate_avenue(const TrackSource& track, const HeightSampler& height_fn,
                           double s_start, double s_end, const WorldPosition& origin,
                           const AvenueProfile& profile) {
    AvenueData out;
    if (s_end <= s_start) {
        return out;
    }
    const glm::dvec3 world_up(0.0, 1.0, 0.0);
    const double lift_road = 0.05;  // anti Z-fighting avec l'herbe
    const double lift_sw = lift_road + profile.sidewalk_h;

    auto frame_at = [&](double s, glm::dvec3& pos, glm::dvec3& right) {
        glm::dvec3 tangent;
        track.sample(s, pos, tangent);
        right = glm::normalize(glm::cross(tangent, world_up));
    };

    // --- Rues perpendiculaires : grille ABSOLUE de chainage (rien ne saute entre
    // deux fenêtres). Elles sont droites : direction = normale de la voie au point de
    // croisement, prolongée de cross_length de part et d'autre.
    const long k0 = static_cast<long>(std::ceil(s_start / profile.cross_spacing));
    const long k1 = static_cast<long>(std::floor(s_end / profile.cross_spacing));
    std::vector<double> cross_s;
    for (long k = k0; k <= k1; ++k) {
        const double s = static_cast<double>(k) * profile.cross_spacing;
        glm::dvec3 pos, right;
        frame_at(s, pos, right);
        out.cross.push_back({pos, right, profile.cross_length,
                             profile.cross_half + profile.sidewalk_w + 1.0});
        cross_s.push_back(s);
    }

    auto near_cross = [&](double s) {
        for (const double cs : cross_s) {
            if (std::abs(s - cs) < profile.cross_half + profile.step) {
                return true;  // le carrefour est pavé par la rue, pas par l'avenue
            }
        }
        return false;
    };

    // --- L'avenue elle-même : suit la spline, les piliers y sont plantés (pas de
    // perçage, pas de zigzag). Aux carrefours, la chaussée de l'avenue s'interrompt
    // et cède la place à celle de la rue (même altitude => pas de Z-fighting).
    const int count =
        std::max(2, static_cast<int>(std::ceil((s_end - s_start) / profile.step)) + 1);
    for (int i = 0; i + 1 < count; ++i) {
        const double s0 = s_start + (static_cast<double>(i) / (count - 1)) * (s_end - s_start);
        const double s1 =
            s_start + (static_cast<double>(i + 1) / (count - 1)) * (s_end - s_start);

        glm::dvec3 c0, r0, c1, r1;
        frame_at(s0, c0, r0);
        frame_at(s1, c1, r1);

        if (!near_cross(0.5 * (s0 + s1))) {
            emit_strip(out.roadway, height_fn, origin, c0, r0, c1, r1,
                       -profile.road_half, profile.road_half, lift_road);
        }
        // Trottoirs des deux côtés, surélevés de 15 cm, continus même aux carrefours
        // (ils longent l'avenue ; la rue passe entre chaussée et trottoir).
        emit_strip(out.sidewalks, height_fn, origin, c0, r0, c1, r1,
                   profile.road_half, profile.road_half + profile.sidewalk_w, lift_sw);
        emit_strip(out.sidewalks, height_fn, origin, c0, r0, c1, r1,
                   -profile.road_half - profile.sidewalk_w, -profile.road_half, lift_sw);
    }

    // --- Chaussées et trottoirs des rues perpendiculaires.
    for (const CrossStreet& cs : out.cross) {
        const int segs = static_cast<int>(std::ceil(2.0 * profile.cross_length / 10.0));
        const glm::dvec3 perp = glm::normalize(glm::dvec3(-cs.right.z, 0.0, cs.right.x));
        for (int i = 0; i < segs; ++i) {
            const double t0 = -profile.cross_length +
                              (2.0 * profile.cross_length) * (static_cast<double>(i) / segs);
            const double t1 = -profile.cross_length +
                              (2.0 * profile.cross_length) * (static_cast<double>(i + 1) / segs);
            const glm::dvec3 c0 = cs.point + cs.right * t0;
            const glm::dvec3 c1 = cs.point + cs.right * t1;

            emit_strip(out.roadway, height_fn, origin, c0, perp, c1, perp,
                       -profile.cross_half, profile.cross_half, lift_road);
            // Les trottoirs de rue s'arrêtent au bord de l'avenue (pas de dalle de
            // béton posée en travers de la chaussée principale).
            const double gap = profile.road_half + profile.sidewalk_w + 1.0;
            if (std::abs(0.5 * (t0 + t1)) > gap) {
                emit_strip(out.sidewalks, height_fn, origin, c0, perp, c1, perp,
                           profile.cross_half, profile.cross_half + profile.sidewalk_w,
                           lift_sw);
                emit_strip(out.sidewalks, height_fn, origin, c0, perp, c1, perp,
                           -profile.cross_half - profile.sidewalk_w, -profile.cross_half,
                           lift_sw);
            }
        }

        // Lampadaires de rue : tous les 40 m, au milieu du trottoir, en alternance.
        const double lamp_gap = profile.road_half + profile.sidewalk_w + 1.0;
        int side = 1;
        for (double t = -profile.cross_length + 20.0; t < profile.cross_length;
             t += 40.0, side = -side) {
            if (std::abs(t) < lamp_gap) {
                continue;
            }
            const glm::dvec3 p = cs.point + cs.right * t +
                                 perp * (static_cast<double>(side) *
                                         (profile.cross_half + profile.sidewalk_w * 0.5));
            out.lamps.push_back({p.x, height_fn(p.x, p.z) + 0.20, p.z, 0.0f});
        }
    }

    // --- Lampadaires de l'avenue : grille ABSOLUE, deux côtés en quinconce.
    const double lamp_off = profile.road_half + profile.sidewalk_w * 0.5;
    const long l0 = static_cast<long>(std::ceil(s_start / profile.lamp_spacing));
    const long l1 = static_cast<long>(std::floor(s_end / profile.lamp_spacing));
    for (long k = l0; k <= l1; ++k) {
        const double s = static_cast<double>(k) * profile.lamp_spacing;
        glm::dvec3 pos, right;
        frame_at(s, pos, right);
        const double side = (k % 2 == 0) ? lamp_off : -lamp_off;
        const glm::dvec3 p = pos + right * side;
        out.lamps.push_back({p.x, height_fn(p.x, p.z) + 0.20, p.z, 0.0f});
    }

    return out;
}

}  // namespace noire::scene
