#include "noire/scene/catenary.hpp"

#include <cmath>

namespace noire::scene {

namespace {

// Repère local de la voie au chainage x. `lateral` = cross(tangent, up) : c'est EXACTEMENT
// l'image de +Z local par la rotation de lacet des instances (cf. pole_yaw), ce qui garantit
// qu'un poteau instancié tombe sur le même côté que les fils engendrés ici.
struct Frame {
    glm::dvec3 pos;
    glm::dvec3 tangent;
    glm::dvec3 lateral;
    glm::dvec3 up;
};

Frame frame_at(const TrackSource& track, double x) {
    Frame f;
    track.sample(x, f.pos, f.tangent);
    f.tangent = glm::normalize(f.tangent);
    f.lateral = glm::normalize(glm::cross(f.tangent, glm::dvec3(0.0, 1.0, 0.0)));
    f.up = glm::normalize(glm::cross(f.lateral, f.tangent));
    return f;
}

// Lacet tel que la rotation des instances envoie +X local sur la tangente ET +Z local sur
// `lateral`. La rotation du vertex shader est mat3(c,0,-s, 0,1,0, s,0,c) : elle envoie
// +X sur (c,0,-s), donc c = tangent.x et s = -tangent.z.
float pole_yaw(const glm::dvec3& tangent) {
    return static_cast<float>(std::atan2(-tangent.z, tangent.x));
}

// Désaxement au poteau d'indice `k` : il ALTERNE, c'est tout son intérêt.
double stagger_at_pole(long k, double stagger) { return (k % 2 == 0) ? stagger : -stagger; }

// Un ruban de câble. Les DEUX sommets d'un point partagent la même position : le ruban
// n'existe pas encore ici, il sera déplié par le vertex shader face à la caméra. On ne
// pousse que ce qu'il lui faut pour ça — la tangente et le rayon vrai.
//   uv.x = côté (-1 / +1), uv.y = rayon vrai (m), tangent.xyz = direction du fil.
// Réutilise MeshVertex tel quel : aucune entrée de sommets supplémentaire à décrire côté
// pipeline, et le chemin create_mesh_indexed reste inchangé.
void emit_wire(RailMeshData& out, const std::vector<glm::dvec3>& pts, const WorldPosition& origin,
               float radius) {
    if (pts.size() < 2) {
        return;
    }
    const auto base = static_cast<std::uint32_t>(out.vertices.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        // Différence centrée : sur une courbe tessellée, prendre le segment suivant seul
        // ferait tourner le ruban d'un cran à chaque sommet et le ferait vriller.
        const glm::dvec3 prev = pts[i > 0 ? i - 1 : 0];
        const glm::dvec3 next = pts[i + 1 < pts.size() ? i + 1 : pts.size() - 1];
        glm::dvec3 t = next - prev;
        const double len = glm::length(t);
        t = len > 1e-9 ? t / len : glm::dvec3(1.0, 0.0, 0.0);

        for (int side = 0; side < 2; ++side) {
            render::MeshVertex v;
            v.position = glm::vec3(pts[i] - origin);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);  // inutilisée : la normale est reconstruite
            v.uv = glm::vec2(side == 0 ? -1.0f : 1.0f, radius);
            v.tangent = glm::vec4(glm::vec3(t), 0.0f);
            out.vertices.push_back(v);
        }
    }
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const auto a = base + static_cast<std::uint32_t>(i * 2);
        out.indices.insert(out.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
}

// --- Poteau : petites boîtes, en repère local ---------------------------------
void emit_box(RailMeshData& out, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {2, 3, 7, 6}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    const glm::vec3 normals[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0},
                                  {0, 1, 0},  {-1, 0, 0}, {1, 0, 0}};
    for (int f = 0; f < 6; ++f) {
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (int k = 0; k < 4; ++k) {
            render::MeshVertex v;
            v.position = c[faces[f][k]];
            v.normal = normals[f];
            // UV planaires grossières : l'acier du poteau est uni, elles ne servent qu'à
            // ne pas laisser la texture de secours lire des UV indéfinies.
            v.uv = glm::vec2(v.position.x + v.position.z, v.position.y) * 0.5f;
            // Tangente arbitraire mais ORTHOGONALE à la normale : le TBN du shader la
            // suppose telle, et une tangente colinéaire à N produirait une normale NaN.
            const glm::vec3 t = std::abs(normals[f].y) > 0.9f ? glm::vec3(1, 0, 0)
                                                              : glm::vec3(0, 1, 0);
            v.tangent = glm::vec4(glm::normalize(glm::cross(normals[f], t)), 1.0f);
            out.vertices.push_back(v);
        }
        out.indices.insert(out.indices.end(),
                           {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

}  // namespace

RailMeshData generate_pole_mesh(const CatenaryProfile& p) {
    RailMeshData m;
    const float fh = p.pole_flange_half;
    const float wh = p.pole_web_half;
    const float y0 = p.pole_base_y;
    const float y1 = p.pole_top_y;

    // Profilé en H, vu de dessus : deux SEMELLES parallèles à la voie, reliées par une
    // ÂME. Les semelles sont perpendiculaires à l'effort dominant — la traction des fils,
    // qui tire le poteau vers la voie (+Z). C'est bien le sens d'un vrai poteau caténaire.
    emit_box(m, {-fh, y0, -wh}, {fh, y1, -wh + 0.03f});  // semelle arrière
    emit_box(m, {-fh, y0, wh - 0.03f}, {fh, y1, wh});    // semelle avant
    emit_box(m, {-0.02f, y0, -wh}, {0.02f, y1, wh});     // âme

    // Console (potence) : elle part du mât et franchit l'axe pour porter les fils.
    // `pole_offset` sépare le poteau de l'axe ; on va un peu au-delà pour que le bras de
    // rappel du fil de contact ait de quoi tenir le désaxement.
    const auto reach = static_cast<float>(-(p.pole_offset + p.stagger + 0.35));
    emit_box(m, {-0.05f, p.console_y, reach}, {0.05f, p.console_y + 0.12f, 0.0f});

    // Haubanage : la diagonale qui empêche la console de ployer. Approximée par une boîte
    // fine et inclinée « en escalier » — à la distance où l'on voit un poteau, sa section
    // exacte ne se lit pas, seule sa SILHOUETTE compte.
    const int steps = 6;
    for (int i = 0; i < steps; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(steps);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(steps);
        const float z0 = reach * t0;
        const float z1 = reach * t1;
        const float ya = glm::mix(p.pole_top_y, p.console_y + 0.12f, t0);
        const float yb = glm::mix(p.pole_top_y, p.console_y + 0.12f, t1);
        emit_box(m, {-0.03f, std::min(ya, yb) - 0.03f, std::min(z0, z1)},
                 {0.03f, std::max(ya, yb) + 0.03f, std::max(z0, z1)});
    }
    return m;
}

CatenaryData generate_catenary(const TrackSource& track, double x_start, double x_end,
                               const WorldPosition& origin, const CatenaryProfile& p) {
    CatenaryData out;
    if (p.span <= 0.1 || x_end <= x_start) {
        return out;
    }

    // Grille ABSOLUE de poteaux. Indexer depuis x_start ferait glisser tous les poteaux
    // quand la fenêtre bouge : ils sont ancrés au monde, pas à la caméra.
    const long k0 = static_cast<long>(std::floor(x_start / p.span));
    const long k1 = static_cast<long>(std::ceil(x_end / p.span));

    for (long k = k0; k < k1; ++k) {
        const double xa = static_cast<double>(k) * p.span;      // poteau amont
        const double xb = xa + p.span;                          // poteau aval
        const Frame fa = frame_at(track, xa);
        const Frame fb = frame_at(track, xb);

        // --- Le poteau, en instance ---
        render::InstanceData inst;
        const glm::dvec3 pole_pos = fa.pos + fa.lateral * p.pole_offset;
        inst.position_scale = glm::vec4(glm::vec3(pole_pos - origin), 1.0f);
        // z = 0 : AUCUN vent. Le pipeline instancié est partagé avec la végétation, dont le
        // vertex shader fait ployer les sommets ; un poteau d'acier qui se balance serait
        // grotesque. C'est l'instance qui décide, pas le pipeline.
        inst.rotation_phase = glm::vec4(pole_yaw(fa.tangent), 0.0f, 0.0f, 0.0f);
        out.poles.push_back(inst);

        // --- Désaxement aux deux extrémités de la portée ---
        const double sa = stagger_at_pole(k, p.stagger);
        const double sb = stagger_at_pole(k + 1, p.stagger);

        // --- Fil de contact : RIGOUREUSEMENT horizontal, mais désaxé ---
        // Deux points suffiraient pour la géométrie (il est droit entre deux poteaux),
        // mais on le tessellle au pas des pendules pour que la voie courbe ne le fasse pas
        // couper au travers, et pour que le ruban suive la courbure.
        const int seg = std::max(2, static_cast<int>(std::lround(p.span / p.dropper_spacing)));
        std::vector<glm::dvec3> contact;
        std::vector<glm::dvec3> messenger;
        contact.reserve(static_cast<std::size_t>(seg) + 1);
        const int msg_seg = std::max(seg, static_cast<int>(std::lround(p.span / p.wire_step)));
        messenger.reserve(static_cast<std::size_t>(msg_seg) + 1);

        const auto contact_point = [&](double t) {
            const Frame f = frame_at(track, xa + t * p.span);
            const double s = glm::mix(sa, sb, t);
            return f.pos + f.up * p.contact_height + f.lateral * s;
        };
        // Le porteur reste sur l'AXE (pas de désaxement) : c'est le fil de contact seul
        // qui louvoie, tiré par les bras de rappel. Les pendules sont donc légèrement
        // obliques, ce qui est exactement ce qu'on voit sur une vraie caténaire.
        const auto messenger_point = [&](double t) {
            const Frame f = frame_at(track, xa + t * p.span);
            // Parabole : la chaînette vraie est y = a·cosh(x/a), mais sous faible flèche
            // (0,7 m pour 50 m de portée, soit 1,4 %) elle et sa parabole osculatrice
            // diffèrent de moins d'un MILLIMÈTRE — trois ordres de grandeur sous le pixel.
            // On garde la parabole : deux multiplications au lieu d'un cosh par sommet.
            const double sag = p.messenger_sag * 4.0 * t * (1.0 - t);
            return f.pos + f.up * (p.contact_height + p.system_height - sag);
        };

        for (int i = 0; i <= seg; ++i) {
            contact.push_back(contact_point(static_cast<double>(i) / seg));
        }
        for (int i = 0; i <= msg_seg; ++i) {
            messenger.push_back(messenger_point(static_cast<double>(i) / msg_seg));
        }
        emit_wire(out.wires, contact, origin, p.contact_radius);
        emit_wire(out.wires, messenger, origin, p.messenger_radius);

        // --- Pendules : ils tiennent le fil de contact À L'HORIZONTALE ---
        // Leur longueur VARIE : maximale au poteau (le porteur y est haut), minimale à
        // mi-portée (le porteur y a fléchi). C'est le mécanisme même de la caténaire.
        // On saute t=0 et t=1 : au poteau, c'est la console qui tient le fil.
        for (int i = 1; i < seg; ++i) {
            const double t = static_cast<double>(i) / seg;
            const std::vector<glm::dvec3> dropper{messenger_point(t), contact_point(t)};
            emit_wire(out.wires, dropper, origin, p.dropper_radius);
        }
    }
    return out;
}

}  // namespace noire::scene
