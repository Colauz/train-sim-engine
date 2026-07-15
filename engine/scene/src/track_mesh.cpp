#include "noire/scene/track_mesh.hpp"

#include <algorithm>
#include <cmath>

namespace noire::scene {

namespace {

// Repère local de la voie en un point de chainage. `center` est sur le PLAN DE ROULEMENT
// (cf. la convention en tête de track_mesh.hpp) : tout se construit en dessous.
struct Frame {
    glm::vec3 center;
    glm::vec3 right;
    glm::vec3 up;
    float u = 0.0f;  // coordonnée de texture le long de la voie
};

// Un point du profil transversal, dans le plan (lateral, vertical) de la voie.
struct P2 {
    float s;  // latéral (+ = vers la droite de la voie)
    float t;  // vertical (0 = plan de roulement, négatif = vers le bas)
};

glm::vec3 world_of(const Frame& f, const P2& p, float lateral_offset) {
    return f.center + f.right * (p.s + lateral_offset) + f.up * p.t;
}

// Extrude un profil le long des frames.
//
// NORMALES : elles viennent du WINDING du profil, PAS d'un « à l'opposé du centre ».
// L'ancienne astuce (M8) marchait sur une boîte convexe mais ment sur un profil en I :
// la face supérieure du patin pointerait vers le bas, puisqu'elle est plus proche du
// bord que du centre de section. Ici, profil parcouru dans le sens TRIGONOMÉTRIQUE
// (s vers la droite, t vers le haut) => la normale sortante d'une arête d est
// (d.t, -d.s). Vérification : arête du bas parcourue vers +s => normale (0,-1), soit
// vers le bas. Correct.
//
// Les sommets ne sont JAMAIS partagés entre faces : chaque quad porte sa propre normale,
// donc toutes les arêtes restent franches (un rail est anguleux, pas lissé).
void extrude(RailMeshData& out, const std::vector<Frame>& frames, const std::vector<P2>& profile,
             bool closed, float lateral_offset, float uv_period) {
    if (frames.size() < 2 || profile.size() < 2) {
        return;
    }
    const std::size_t edges = closed ? profile.size() : profile.size() - 1;

    // v = distance cumulée LE LONG DU PROFIL / période : la texture garde la même
    // échelle physique en travers qu'en long.
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
                continue;  // arête dégénérée : rien à extruder
            }
            // Normale sortante 2D, ramenée dans le repère monde de la voie.
            const glm::vec3 normal =
                glm::normalize(f0.right * (dt / len) + f0.up * (-ds / len));

            const glm::vec3 a = world_of(f0, p0, lateral_offset);
            const glm::vec3 b = world_of(f1, p0, lateral_offset);
            const glm::vec3 c = world_of(f1, p1, lateral_offset);
            const glm::vec3 d = world_of(f0, p1, lateral_offset);

            const glm::vec3 tangent = glm::normalize(b - a);  // sens des u (le long du rail)
            // Handedness glTF : cross(N,T)*w doit suivre les v croissants (a -> d).
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

// Boîte orientée (traverse) : 6 faces, normales explicites, sommets non partagés.
void add_box(RailMeshData& out, const glm::vec3& center, const glm::vec3& right,
             const glm::vec3& forward, const glm::vec3& up, const glm::vec3& half,
             float uv_period) {
    const glm::vec3 x = right * half.x;
    const glm::vec3 y = up * half.y;
    const glm::vec3 z = forward * half.z;

    // Chaque face : sa normale, son axe u et son axe v (avec leurs demi-longueurs).
    const struct {
        glm::vec3 n, uu, vv, offset;
        float ul, vl;
    } faces[6] = {
        {up, right, forward, y, half.x, half.z},          // dessus
        {-up, right, -forward, -y, half.x, half.z},       // dessous
        {right, forward, up, x, half.z, half.y},          // flanc droit
        {-right, -forward, up, -x, half.z, half.y},       // flanc gauche
        {forward, right, up, z, half.x, half.y},          // bout avant
        {-forward, -right, up, -z, half.x, half.y},       // bout arrière
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
        // UV en MÈTRES / période : le grain du béton ne dépend pas de la face.
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

// Profil du rail : polyligne FERMÉE, sens trigonométrique, t = 0 au plan de roulement.
// Silhouette en I : patin large, âme mince, champignon. Les congés réels sont des arcs ;
// on les réduit à un chanfrein droit, invisible à l'échelle où l'on voit un rail.
std::vector<P2> make_rail_profile(const RailProfile& p) {
    const float top = 0.0f;                 // plan de roulement
    const float bottom = -p.rail_height;    // dessous du patin
    const float foot_top = bottom + 0.030f; // épaisseur du patin
    const float head_bottom = top - 0.040f; // hauteur du champignon
    const float fillet = 0.012f;            // chanfrein patin -> âme et âme -> champignon

    return {
        {-p.rail_foot_half, bottom},               // patin, dessous gauche
        {p.rail_foot_half, bottom},                // patin, dessous droit
        {p.rail_foot_half, foot_top - fillet},     // chant du patin
        {p.rail_web_half, foot_top + fillet},      // congé vers l'âme
        {p.rail_web_half, head_bottom - fillet},   // âme, côté droit
        {p.rail_head_half, head_bottom + fillet},  // évasement du champignon
        {p.rail_head_half, top},                   // champignon, dessus droit
        {-p.rail_head_half, top},                  // plan de roulement
        {-p.rail_head_half, head_bottom + fillet},
        {-p.rail_web_half, head_bottom - fillet},  // âme, côté gauche
        {-p.rail_web_half, foot_top + fillet},
        {-p.rail_foot_half, foot_top - fillet},
    };
}

// Profil du rail en LOD lointain : un simple bloc rectangulaire, aux cotes du champignon
// mais sur toute la hauteur. 4 arêtes au lieu de 12. À 2 km, l'âme et le patin d'un rail
// se projettent bien en dessous du pixel : les mailler est du calcul pur perdu.
std::vector<P2> make_rail_profile_distant(const RailProfile& p) {
    return {
        {-p.rail_head_half, -p.rail_height},
        {p.rail_head_half, -p.rail_height},
        {p.rail_head_half, 0.0f},
        {-p.rail_head_half, 0.0f},
    };
}

// Profil du ballast : polyligne OUVERTE (le dessous est enterré, on ne le maille pas).
// Parcourue de DROITE à GAUCHE, ce qui correspond au sens trigonométrique pour une
// surface supérieure — la même formule de normale que le rail s'applique donc.
std::vector<P2> make_ballast_profile(const RailProfile& p) {
    return {
        {p.ballast_base_half, p.ballast_base_y},    // pied du talus droit
        {p.ballast_crown_half, p.ballast_crown_y},  // arête du plateau, droite
        {-p.ballast_crown_half, p.ballast_crown_y}, // plateau
        {-p.ballast_base_half, p.ballast_base_y},   // pied du talus gauche
    };
}

}  // namespace

TrackMeshData generate_track_mesh(const TrackSource& track, double x_start, double x_end,
                                  const WorldPosition& origin, const RailProfile& profile,
                                  TrackLod lod) {
    TrackMeshData out;
    const double span = x_end - x_start;
    if (span <= 0.0) {
        return out;
    }

    const bool full = lod == TrackLod::Full;
    const double raw_step = full ? profile.sample_step : profile.distant_sample_step;
    const double step = (raw_step > 0.01) ? raw_step : 1.0;
    const int count = std::max(2, static_cast<int>(std::ceil(span / step)) + 1);
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const float uv_period = profile.uv_period > 0.01f ? profile.uv_period : 1.0f;

    // Échantillonnage : un repère par section.
    std::vector<Frame> frames;
    frames.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double x = x_start + (static_cast<double>(i) / (count - 1)) * span;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(x, pos_world, tangent);

        Frame f;
        f.center = glm::vec3(pos_world - origin);  // monde double -> local float
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        f.right = glm::normalize(glm::cross(forward, world_up));
        f.up = glm::normalize(glm::cross(f.right, forward));
        // u RELATIF au début de la tuile : garde les UV petits malgré un chainage à 7
        // chiffres. Les tuiles raccordent car chunk_length est un multiple de uv_period.
        f.u = static_cast<float>((x - x_start) / static_cast<double>(uv_period));
        frames.push_back(f);
    }

    // --- Rails : le profil en I, extrudé deux fois -----------------------------
    // L'écartement se mesure entre FACES INTERNES des champignons : l'axe de chaque rail
    // est donc à gauge/2 + demi-champignon de l'axe de voie.
    const std::vector<P2> rail_profile =
        full ? make_rail_profile(profile) : make_rail_profile_distant(profile);
    const float rail_axis = static_cast<float>(profile.gauge * 0.5) + profile.rail_head_half;
    extrude(out.rails, frames, rail_profile, /*closed=*/true, rail_axis, uv_period);
    extrude(out.rails, frames, rail_profile, /*closed=*/true, -rail_axis, uv_period);

    // --- Ballast --------------------------------------------------------------
    // Gardé aux DEUX niveaux : c'est lui qui dessine le tracé de la ligne dans le
    // paysage, et il ne coûte que 3 arêtes.
    extrude(out.ballast, frames, make_ballast_profile(profile), /*closed=*/false, 0.0f, uv_period);

    // --- Accotement / remblai -------------------------------------------------
    // Le seul élément dont le profil VARIE le long de la voie : son point extérieur est
    // calé sur une altitude ABSOLUE (le plan de sol), pas sur la voie. La hauteur du
    // remblai est donc exactement l'altitude locale de la spline au-dessus du sol — c'est
    // ce qui raccorde la voie au terrain quelle que soit la colline, sans plus jamais
    // flotter ni s'enterrer. Gardé aux deux LOD : il ne coûte qu'une arête par côté et
    // sans lui la voie lointaine « décollerait » du sol.
    for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
        for (const float side : {1.0f, -1.0f}) {
            const Frame& f0 = frames[i];
            const Frame& f1 = frames[i + 1];
            // t du point extérieur : profondeur du sol SOUS le repère local. On divise par
            // up.y car `up` est incliné par la pente (l'erreur serait sinon de 1 %).
            auto outer_t = [&](const Frame& f) {
                const double abs_y = origin.y + static_cast<double>(f.center.y);
                return static_cast<float>((profile.ground_level - abs_y) /
                                          std::max(static_cast<double>(f.up.y), 1e-3));
            };
            const P2 inner{side * profile.ballast_base_half, profile.ballast_base_y};
            const P2 outer0{side * profile.shoulder_half, outer_t(f0)};
            const P2 outer1{side * profile.shoulder_half, outer_t(f1)};

            const glm::vec3 a = world_of(f0, inner, 0.0f);
            const glm::vec3 b = world_of(f1, inner, 0.0f);
            const glm::vec3 c = world_of(f1, outer1, 0.0f);
            const glm::vec3 d = world_of(f0, outer0, 0.0f);

            const glm::vec3 tangent = glm::normalize(b - a);
            glm::vec3 normal = glm::normalize(glm::cross(tangent, d - a));
            if (normal.y < 0.0f) {
                normal = -normal;  // un talus se voit du dessus : normale toujours vers le haut
            }
            const float w = glm::dot(glm::cross(normal, tangent), d - a) < 0.0f ? -1.0f : 1.0f;
            const glm::vec4 tangent4(tangent, w);
            const float v_out = (profile.shoulder_half - profile.ballast_base_half) / uv_period;

            const auto base = static_cast<std::uint32_t>(out.shoulder.vertices.size());
            out.shoulder.vertices.push_back(render::MeshVertex{a, normal, {f0.u, 0.0f}, tangent4});
            out.shoulder.vertices.push_back(render::MeshVertex{b, normal, {f1.u, 0.0f}, tangent4});
            out.shoulder.vertices.push_back(render::MeshVertex{c, normal, {f1.u, v_out}, tangent4});
            out.shoulder.vertices.push_back(render::MeshVertex{d, normal, {f0.u, v_out}, tangent4});
            out.shoulder.indices.insert(out.shoulder.indices.end(),
                                        {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }

    // --- Traverses ------------------------------------------------------------
    // ABANDONNÉES au loin : à elles seules, elles pèsent ~80 k sommets par tuile (3333
    // traverses), pour un objet de 30 cm de large qui, à 2 km, ne couvre pas un pixel.
    // C'est l'essentiel de l'économie du LOD.
    if (!full) {
        return out;
    }
    // Échantillonnées à leur PROPRE pas (60 cm), indépendant de celui de la voie : une
    // traverse est un objet discret, pas une extrusion.
    const double spacing = profile.sleeper_spacing > 0.01f ? profile.sleeper_spacing : 0.6;
    // Ancrées sur le chainage ABSOLU : sans ça, deux tuiles voisines auraient chacune
    // leur propre phase et l'espacement sauterait à la jointure.
    const double first = std::ceil(x_start / spacing) * spacing;
    const glm::vec3 sleeper_half(profile.sleeper_half_length, profile.sleeper_thickness * 0.5f,
                                 profile.sleeper_half_width);
    for (double x = first; x < x_end; x += spacing) {
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(x, pos_world, tangent);
        const glm::vec3 center = glm::vec3(pos_world - origin);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));

        // Le dessus de la traverse porte le patin du rail : il affleure donc le dessous
        // du rail, et le centre de la boîte est une demi-épaisseur plus bas.
        const glm::vec3 box_center =
            center - up * (profile.rail_height + profile.sleeper_thickness * 0.5f);
        add_box(out.sleepers, box_center, right, forward, up, sleeper_half, uv_period);
    }
    return out;
}

}  // namespace noire::scene
