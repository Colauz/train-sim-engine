#include "noire/scene/viaduct.hpp"

#include <algorithm>
#include <cmath>

#include "noire/core/terrain.hpp"

namespace noire::scene {

namespace {

// Repère local de la voie en un point de chainage — même convention que track_mesh.cpp :
// `center` est sur le PLAN DE ROULEMENT, tout se construit en dessous.
struct Frame {
    glm::vec3 center;  // relatif à l'origine flottante
    glm::vec3 right;
    glm::vec3 up;
    float u = 0.0f;
};

// Un point du profil transversal, dans le plan (latéral, vertical) de la voie.
struct P2 {
    float s;  // latéral (+ = vers la droite de la voie)
    float t;  // vertical (0 = plan de roulement, négatif = vers le bas)
};

glm::vec3 world_of(const Frame& f, const P2& p) {
    return f.center + f.right * p.s + f.up * p.t;
}

// Extrude un profil FERMÉ le long des frames. Mêmes règles que l'extrude de
// track_mesh.cpp : profil parcouru dans le sens trigonométrique (normale sortante =
// (d.t, -d.s)), sommets jamais partagés (arêtes franches), UV en mètres / période.
void extrude(RailMeshData& out, const std::vector<Frame>& frames, const std::vector<P2>& profile,
             float uv_period) {
    if (frames.size() < 2 || profile.size() < 2) {
        return;
    }
    const std::size_t edges = profile.size();

    std::vector<float> v_coord(profile.size() + 1, 0.0f);
    for (std::size_t j = 0; j < edges; ++j) {
        const P2& p0 = profile[j];
        const P2& p1 = profile[(j + 1) % profile.size()];
        const float len = std::sqrt((p1.s - p0.s) * (p1.s - p0.s) + (p1.t - p0.t) * (p1.t - p0.t));
        v_coord[j + 1] = v_coord[j] + len / uv_period;
    }

    for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
        const Frame& f0 = frames[i];
        const Frame& f1 = frames[i + 1];
        for (std::size_t j = 0; j < edges; ++j) {
            const P2& p0 = profile[j];
            const P2& p1 = profile[(j + 1) % profile.size()];
            const float ds = p1.s - p0.s;
            const float dt = p1.t - p0.t;
            const float len = std::sqrt(ds * ds + dt * dt);
            if (len < 1e-6f) {
                continue;
            }
            const glm::vec3 normal = glm::normalize(f0.right * (dt / len) + f0.up * (-ds / len));

            const glm::vec3 a = world_of(f0, p0);
            const glm::vec3 b = world_of(f1, p0);
            const glm::vec3 c = world_of(f1, p1);
            const glm::vec3 d = world_of(f0, p1);

            const glm::vec3 tangent = glm::normalize(b - a);
            const float w = glm::dot(glm::cross(normal, tangent), d - a) < 0.0f ? -1.0f : 1.0f;
            const glm::vec4 tangent4(tangent, w);

            const auto base = static_cast<std::uint32_t>(out.vertices.size());
            out.vertices.push_back(render::MeshVertex{a, normal, {f0.u, v_coord[j]}, tangent4});
            out.vertices.push_back(render::MeshVertex{b, normal, {f1.u, v_coord[j]}, tangent4});
            out.vertices.push_back(render::MeshVertex{c, normal, {f1.u, v_coord[j + 1]}, tangent4});
            out.vertices.push_back(render::MeshVertex{d, normal, {f0.u, v_coord[j + 1]}, tangent4});
            out.indices.insert(out.indices.end(),
                               {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
}

// Boîte orientée (pile) : 6 faces, normales explicites, sommets non partagés — même
// fonction que l'add_box de track_mesh.cpp (les traverses), reprise telle quelle.
void add_box(RailMeshData& out, const glm::vec3& center, const glm::vec3& right,
             const glm::vec3& forward, const glm::vec3& up, const glm::vec3& half,
             float uv_period) {
    const glm::vec3 x = right * half.x;
    const glm::vec3 y = up * half.y;
    const glm::vec3 z = forward * half.z;

    const struct {
        glm::vec3 n, uu, vv, offset;
        float ul, vl;
    } faces[6] = {
        {up, right, forward, y, half.x, half.z},
        {-up, right, -forward, -y, half.x, half.z},
        {right, forward, up, x, half.z, half.y},
        {-right, -forward, up, -x, half.z, half.y},
        {forward, right, up, z, half.x, half.y},
        {-forward, -right, up, -z, half.x, half.y},
    };

    for (const auto& f : faces) {
        const glm::vec3 c = center + f.offset;
        const glm::vec3 du = f.uu * f.ul;
        const glm::vec3 dv = f.vv * f.vl;
        const glm::vec3 a = c - du - dv;
        const glm::vec3 b = c + du - dv;
        const glm::vec3 cc = c + du + dv;
        const glm::vec3 d = c - du + dv;

        const glm::vec3 tangent = glm::normalize(f.uu);
        const float w = glm::dot(glm::cross(f.n, tangent), f.vv) < 0.0f ? -1.0f : 1.0f;
        const glm::vec4 tangent4(tangent, w);
        const float us = f.ul * 2.0f / uv_period;
        const float vs = f.vl * 2.0f / uv_period;

        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        out.vertices.push_back(render::MeshVertex{a, f.n, {0.0f, 0.0f}, tangent4});
        out.vertices.push_back(render::MeshVertex{b, f.n, {us, 0.0f}, tangent4});
        out.vertices.push_back(render::MeshVertex{cc, f.n, {us, vs}, tangent4});
        out.vertices.push_back(render::MeshVertex{d, f.n, {0.0f, vs}, tangent4});
        out.indices.insert(out.indices.end(),
                           {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

}  // namespace

RailMeshData generate_viaduct(const TrackSource& track, const Terrain& terrain, double x_start,
                              double x_end, const WorldPosition& origin,
                              const ViaductProfile& profile) {
    RailMeshData out;
    const double span = x_end - x_start;
    if (span <= 0.0) {
        return out;
    }
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const float uv_period = 4.0f;

    // --- Tablier : poutre-caisson extrudée le long de la spline -------------------
    const int count = std::max(2, static_cast<int>(std::ceil(span / profile.step)) + 1);
    std::vector<Frame> frames;
    frames.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double x = x_start + (static_cast<double>(i) / (count - 1)) * span;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(x, pos_world, tangent);
        Frame f;
        f.center = glm::vec3(pos_world - origin);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        f.right = glm::normalize(glm::cross(forward, world_up));
        f.up = glm::normalize(glm::cross(f.right, forward));
        f.u = static_cast<float>((x - x_start) / uv_period);
        frames.push_back(f);
    }

    const float hw = profile.deck_half_width;
    const float top = profile.deck_top_y;
    const float bottom = profile.deck_top_y - profile.deck_thickness;
    // Caisson fermé, sens trigonométrique vu depuis l'amont : le dessous d'abord.
    const std::vector<P2> deck = {
        {-hw, bottom}, {hw, bottom}, {hw, top}, {-hw, top},
    };
    extrude(out, frames, deck, uv_period);

    // --- Piles (M41/M44) : extrusion dynamique entre terrain.height(X,Z) - 2.0 et le pont ---
    // Grille ABSOLUE de chainage (comme les poteaux caténaire) : deux fenêtres qui se
    // recouvrent replacent les mêmes piles aux mêmes endroits, rien ne saute.
    const long k0 = static_cast<long>(std::ceil(x_start / profile.pillar_spacing));
    const long k1 = static_cast<long>(std::floor(x_end / profile.pillar_spacing));
    for (long k = k0; k <= k1; ++k) {
        const double x = static_cast<double>(k) * profile.pillar_spacing;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(x, pos_world, tangent);

        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));

        // Pile centrale du viaduc : étirée entre (terrain.height - 2.0) et le dessous du tablier (à ciel ouvert)
        const double ground = terrain.height(pos_world.x, pos_world.z) - 2.0;
        const double deck_under = pos_world.y + static_cast<double>(bottom);
        const double height = deck_under - ground;
        if (height > 0.5) {
            const glm::vec3 mid = glm::vec3(
                glm::dvec3(pos_world.x, ground + height * 0.5, pos_world.z) - origin);
            add_box(out, mid, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(profile.pillar_half_width, static_cast<float>(height) * 0.5f,
                              profile.pillar_half_width),
                    uv_period);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Gare aérienne (M47/M48) : quais + verrière localisée + façades de quai vitrées +
// signalétique suspendue. Ailleurs la ligne est à ciel ouvert.
// ---------------------------------------------------------------------------
StationMeshes generate_station(const TrackSource& track, double s_center,
                               const WorldPosition& origin, const StationProfile& profile) {
    StationMeshes meshes;
    RailMeshData& out = meshes.concrete;
    const double s0 = s_center - profile.length * 0.5;
    const double s1 = s_center + profile.length * 0.5;
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const float uv_period = 4.0f;

    const int count = std::max(2, static_cast<int>(std::ceil((s1 - s0) / profile.step)) + 1);
    std::vector<Frame> frames;
    frames.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double s = s0 + (static_cast<double>(i) / (count - 1)) * (s1 - s0);
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(s, pos_world, tangent);
        Frame f;
        f.center = glm::vec3(pos_world - origin);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        f.right = glm::normalize(glm::cross(forward, world_up));
        f.up = glm::normalize(glm::cross(f.right, forward));
        f.u = static_cast<float>((s - s0) / uv_period);
        frames.push_back(f);
    }

    // Quais : caissons fermés de part et d'autre du tablier, dessus à +1,10 m.
    const float in = profile.platform_inner;
    const float out_w = profile.platform_outer;
    const float top = profile.platform_top;
    const float bottom = -0.80f;  // affleure le dessus du tablier
    for (const float sign : {-1.0f, 1.0f}) {
        const float a = sign * in, b = sign * out_w;
        const std::vector<P2> platform = {
            {a, bottom}, {b, bottom}, {b, top}, {a, top},
        };
        extrude(out, frames, platform, uv_period);
    }

    // Verrière : dalle plane au-dessus des deux quais, sur la longueur de la gare
    // SEULEMENT — jamais au-delà.
    const float roof = profile.roof_y;
    const std::vector<P2> canopy = {
        {-out_w, roof}, {out_w, roof}, {out_w, roof + profile.roof_thickness},
        {-out_w, roof + profile.roof_thickness},
    };
    extrude(out, frames, canopy, uv_period);

    // Colonnes de verrière : en rive extérieure des quais, grille régulière.
    const long k0 = static_cast<long>(std::ceil(s0 / profile.column_spacing));
    const long k1 = static_cast<long>(std::floor(s1 / profile.column_spacing));
    for (long k = k0; k <= k1; ++k) {
        const double s = static_cast<double>(k) * profile.column_spacing;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(s, pos_world, tangent);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));

        for (const float sign : {-1.0f, 1.0f}) {
            const glm::vec3 base = glm::vec3(pos_world - origin) +
                                   right * (sign * (out_w - profile.column_half));
            const double h = static_cast<double>(roof) - static_cast<double>(top);
            const glm::vec3 mid = base + glm::vec3(0.0f, top + static_cast<float>(h) * 0.5f, 0.0f);
            add_box(out, mid, right, forward, glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(profile.column_half, static_cast<float>(h) * 0.5f,
                              profile.column_half),
                    uv_period);
        }
    }

    // M48 — Façades de quai (platform screen doors, style Tokyo) : bande VITRÉE
    // continue le long du bord de chaque quai (elle s'aligne avec la rame quelle que
    // soit sa position d'arrêt), cadres opaques réguliers tous les 2,5 m.
    const float psd = profile.platform_inner + profile.psd_offset;
    for (const float sign : {-1.0f, 1.0f}) {
        const float a = sign * psd;
        const float b = sign * (psd + 0.06f);  // vitrage de 6 cm
        const std::vector<P2> glass_band = {
            {a, top}, {b, top}, {b, top + profile.psd_height}, {a, top + profile.psd_height},
        };
        extrude(meshes.glass, frames, glass_band, uv_period);
    }
    const long f0 = static_cast<long>(std::ceil(s0 / 2.5));
    const long f1 = static_cast<long>(std::floor(s1 / 2.5));
    for (long k = f0; k <= f1; ++k) {
        const double s = static_cast<double>(k) * 2.5;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(s, pos_world, tangent);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
        for (const float sign : {-1.0f, 1.0f}) {
            const glm::vec3 mid = glm::vec3(pos_world - origin) +
                                  right * (sign * (psd + 0.03f)) +
                                  glm::vec3(0.0f, top + profile.psd_height * 0.5f, 0.0f);
            add_box(out, mid, right, forward, glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(0.08f, profile.psd_height * 0.5f, 0.08f), uv_period);
        }
    }

    // M48 — Panneaux d'affichage suspendus sous la verrière, au-dessus de chaque quai.
    const long p0 = static_cast<long>(std::ceil(s0 / profile.sign_spacing));
    const long p1 = static_cast<long>(std::floor(s1 / profile.sign_spacing));
    for (long k = p0; k <= p1; ++k) {
        const double s = static_cast<double>(k) * profile.sign_spacing;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(s, pos_world, tangent);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
        for (const float sign : {-1.0f, 1.0f}) {
            const glm::vec3 mid = glm::vec3(pos_world - origin) +
                                  right * (sign * ((in + out_w) * 0.5f)) +
                                  glm::vec3(0.0f, roof - 0.75f, 0.0f);
            add_box(meshes.signs, mid, right, forward, glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(0.60f, 0.25f, 0.06f), uv_period);
        }
    }
    return meshes;
}

}  // namespace noire::scene
