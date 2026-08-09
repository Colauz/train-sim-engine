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
    // Tuile d'un mètre pour la bande podotactile : ses plots font 20 cm, ils doivent
    // sortir à leur taille réelle. Le reste de la gare est sur la tuile de 4 m du
    // viaduc (uv_period ci-dessous) — deux échelles, deux périodes.
    constexpr float kTactileUvPeriod = 1.0f;
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

    // Les mêmes frames, mais graduées pour la tuile d'un mètre de la bande
    // podotactile (cf. l'usage plus bas). Une copie d'une quinzaine de repères :
    // moins cher que de rendre `extrude` paramétrable, et surtout impossible à
    // confondre avec les frames du quai.
    std::vector<Frame> tactile_frames = frames;
    for (Frame& f : tactile_frames) {
        f.u *= uv_period / kTactileUvPeriod;
    }

    // --- Quais (M52) : nez de quai au gabarit de la rame, plancher de plain-pied ---
    // Profil DÉCROCHÉ, en 6 points : le nez porte en encorbellement au-dessus de
    // l'épaulement du ballast (on ne peut pas descendre un quai plein jusqu'au
    // tablier à 1,55 m de l'axe — le plateau de ballast va jusqu'à 1,90 m), puis la
    // dalle redescend sur le tablier une fois le pied du talus passé.
    //
    //        nez (1,55)                             rive (8,00)
    //   +1,20  o------------------------------------o
    //          |                                    |
    //   -0,30  o--------o (2,70)                    |   <- au-dessus du ballast
    //                   |                           |
    //   -0,80          o---------------------------o     <- sur le tablier
    const float in = profile.platform_inner;
    const float out_w = profile.platform_outer;
    const float top = profile.platform_top;
    const float bottom = -0.80f;  // affleure le dessus du tablier
    const float edge_bot = profile.edge_bottom;
    const float edge_out = profile.edge_outer;
    {
        // Le décroché DOIT rester accordé au ballast : ces deux invariants sont la
        // raison d'être du profil, et un changement de RailProfile doit casser ici,
        // à la compilation, plutôt que sous forme de ballast à travers le quai.
        constexpr RailProfile rail{};
        static_assert(rail.ballast_crown_half > 1.55f,
                      "Si le ballast se rétrécit sous 1,55 m, le décroché du nez de quai "
                      "n'a plus lieu d'être : repasser le quai en profil plein.");
        static_assert(rail.ballast_base_half < 2.70f,
                      "Le pied du talus de ballast doit rester en deçà de edge_outer, "
                      "sinon le quai le traverse.");
    }
    for (const float sign : {-1.0f, 1.0f}) {
        const float a = sign * in, b = sign * out_w, m = sign * edge_out;
        std::vector<P2> platform = {
            {a, edge_bot}, {m, edge_bot}, {m, bottom}, {b, bottom}, {b, top}, {a, top},
        };
        // Le miroir INVERSE le sens de parcours, donc les normales sortantes. Sans ce
        // retournement, le quai de gauche est engendré à l'envers : il ne s'éclaire
        // pas comme celui de droite et disparaît dès que le rendu élimine les faces
        // arrière. Le profil reste rigoureusement le même — seul l'ordre change.
        if (sign < 0.0f) {
            std::reverse(platform.begin(), platform.end());
        }
        // M53 : la dalle part dans SON PROPRE maillage (carrelage de quai), plus
        // dans le béton de structure. Même géométrie, matériau différent.
        extrude(meshes.platform, frames, platform, uv_period);

        // Bande podotactile : un plat surélevé de 12 mm posé sur la dalle, derrière
        // la façade vitrée. Extrudé sur les MÊMES frames que le quai, donc il en
        // épouse exactement la courbure et la pente — un ruban posé « à plat sur le
        // monde » se décollerait du quai dès la première rampe.
        //
        // ...mais sur des frames dont le U a été RECALÉ : `extrude` prend le U tel
        // qu'il est déjà porté par les frames (calculé pour la tuile de 4 m du
        // viaduc) et ne dérive du paramètre `uv_period` que le V. Réutiliser les
        // frames telles quelles étirerait donc les plots d'un facteur 4 dans le sens
        // de la marche : des ovales, pas des plots.
        const float t_in = sign * profile.tactile_inner;
        const float t_out = sign * (profile.tactile_inner + profile.tactile_width);
        const float t_top = top + profile.tactile_rise;
        // Enfoncée de 5 mm DANS la dalle : sa face inférieure n'est alors coplanaire
        // avec rien (la discipline anti-z-fighting du M30.5, appliquée ici aussi).
        const float t_bot = top - 0.005f;
        std::vector<P2> band = {
            {t_in, t_bot}, {t_out, t_bot}, {t_out, t_top}, {t_in, t_top},
        };
        if (sign < 0.0f) {
            std::reverse(band.begin(), band.end());
        }
        extrude(meshes.tactile, tactile_frames, band, kTactileUvPeriod);
    }

    // Verrière (M50) : caisson plan STRICTEMENT borné, dans son propre maillage.
    // Boîte englobante garantie par construction — `frames` ne couvre que
    // [s_center - 75, s_center + 75] et le profil ne dépasse pas ±roof_half_width :
    //   Z : 150 m (les quais, pas un mètre de plus)  X : 20 m  Y : +7,20 à +7,55 m.
    // C'est la fin du « plafond infini » : la dalle ne peut plus s'étendre au-delà
    // de la gare, ni couper la caténaire, qui passe entièrement dessous.
    const float roof = profile.roof_y;
    const float rw = profile.roof_half_width;
    const std::vector<P2> canopy = {
        {-rw, roof}, {rw, roof}, {rw, roof + profile.roof_thickness},
        {-rw, roof + profile.roof_thickness},
    };
    extrude(meshes.canopy, frames, canopy, uv_period);

    // Porteurs de la verrière (M51) : la gare est un ÎLOT AUTOPORTANT. Elle ne
    // s'appuie sur rien d'autre — ni sur les piles du viaduc, ni sur quoi que ce soit
    // de l'environnement urbain : ses appuis descendent en propre jusqu'au SOL
    // UNIFIÉ (Y = 0 monde), et rien d'autre ne peut être bâti dans leur emprise
    // (cf. le corridor sanitaire de 45 m côté app).
    //
    // Deux tronçons, à la même verticale — c'est un seul porteur, dimensionné selon
    // ce qu'il porte :
    //   * en dessous du quai, un PILIER trapu (0,70 m) qui reprend ~20 m de hauteur
    //     jusqu'au sol ; il court en rive EXTÉRIEURE des quais, donc à 7,8 m de
    //     l'axe, très au large du tablier (4,6 m) qu'il ne touche jamais ;
    //   * au-dessus du quai, la COLONNE fine (0,36 m) qui tient la verrière.
    const long k0 = static_cast<long>(std::ceil(s0 / profile.column_spacing));
    const long k1 = static_cast<long>(std::floor(s1 / profile.column_spacing));
    for (long k = k0; k <= k1; ++k) {
        const double s = static_cast<double>(k) * profile.column_spacing;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(s, pos_world, tangent);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));

        // Sol unifié, EXPRIMÉ dans le repère local de la gare (0 = plan de roulement,
        // qui vole à pos_world.y au-dessus du sol). L'embase s'enfonce d'un demi-mètre
        // sous le plan : jamais de jour entre le pilier et le sol, jamais de face
        // coplanaire avec lui non plus.
        const auto ground_local = static_cast<float>(-pos_world.y - 0.5);

        for (const float sign : {-1.0f, 1.0f}) {
            const glm::vec3 base = glm::vec3(pos_world - origin) +
                                   right * (sign * (out_w - profile.column_half));
            // Colonne : du dessus du quai à l'intrados de la verrière.
            const float h_col = roof - top;
            add_box(out, base + glm::vec3(0.0f, top + h_col * 0.5f, 0.0f), right, forward,
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    glm::vec3(profile.column_half, h_col * 0.5f, profile.column_half),
                    uv_period);
            // Pilier : du sol unifié au dessous du quai (-0,80 m, le dessus du tablier).
            const float h_pil = bottom - ground_local;
            if (h_pil > 0.5f) {
                add_box(out, base + glm::vec3(0.0f, ground_local + h_pil * 0.5f, 0.0f), right,
                        forward, glm::vec3(0.0f, 1.0f, 0.0f),
                        glm::vec3(profile.pillar_half, h_pil * 0.5f, profile.pillar_half),
                        uv_period);
            }
        }
    }

    // --- M52 : FAÇADE DE QUAI CALQUÉE SUR LE PLAN DE PORTES DE LA RAME --------
    // Fini la clôture générique sur trame de 2,5 m, qui ne tombait en face d'une
    // porte de rame que par hasard. On parcourt le quai d'un bout à l'autre en
    // s'arrêtant à chaque porte : baie ouvrante EXACTEMENT à son chainage et
    // EXACTEMENT à sa largeur (2 x door_half = 1,30 m, la cote du gabarit), verre
    // fixe sur tout le reste. Les montants opaques encadrent la baie PAR L'EXTÉRIEUR
    // pour ne pas rogner le passage.
    //
    // Latéralement, tout est indexé sur le nez de quai `in` (1,55 m) :
    //   * le vitrage fixe a sa FACE côté voie exactement au nez de quai ;
    //   * les vantaux mobiles sont 7 cm en retrait, donc ils coulissent DERRIÈRE le
    //     vitrage fixe sans jamais le toucher (aucun Z-fighting possible).
    const float psd_face = in + profile.psd_offset;
    const float psd_lat = psd_face + profile.psd_thickness;          // axe du vitrage fixe
    const float leaf_lat = psd_lat + 2.0f * profile.psd_thickness + 0.01f;  // axe des vantaux
    const float psd_mid_y = top + profile.psd_height * 0.5f;
    const float psd_hh = profile.psd_height * 0.5f;
    const float jamb = profile.jamb_half;

    // Un élément de façade : une boîte centrée au chainage `sc`, longue de 2*half_len,
    // posée des DEUX côtés de la voie (symétrie M51).
    const auto add_panel = [&](RailMeshData& dst, double sc, float half_len, float lateral) {
        if (half_len <= 0.0f) {
            return;
        }
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(sc, pos_world, tangent);
        const glm::vec3 fwd = glm::vec3(glm::normalize(tangent));
        const glm::vec3 rgt = glm::normalize(glm::cross(fwd, world_up));
        // On monte selon la NORMALE DE LA VOIE, pas selon la verticale du monde.
        // Le quai est extrudé dans ce repère : s'élever verticalement depuis un rail
        // en pente fait glisser le panneau LE LONG du quai (hauteur x pente, soit
        // 18 mm à 2,10 m sur une rampe de 0,9 %) — assez pour décaler une baie par
        // rapport à la porte de rame qu'elle doit encadrer au millimètre.
        const glm::vec3 up = glm::normalize(glm::cross(rgt, fwd));
        for (const float sign : {-1.0f, 1.0f}) {
            const glm::vec3 mid = glm::vec3(pos_world - origin) + rgt * (sign * lateral) +
                                  up * psd_mid_y;
            // Le panneau est dressé selon la NORMALE DE LA VOIE et non d'aplomb sur
            // le monde. C'est la porte de la RAME qu'il doit épouser : elle est
            // solidaire de la caisse, donc inclinée de la pente. Un panneau d'aplomb
            // voit ses arêtes haute et basse se décaler de 8 mm le long du quai sur
            // une rampe de 0,9 % — la baie ne serait plus « au millimètre ».
            add_box(dst, mid, rgt, fwd, up,
                    glm::vec3(profile.psd_thickness, psd_hh, half_len), uv_period);
        }
    };
    // Verre FIXE entre deux baies : découpé en panneaux de `panel_max` au plus, pour
    // que la façade suive la courbure de la voie au lieu de la couper à la corde.
    const auto add_fixed_run = [&](double from, double to) {
        const double len = to - from;
        if (len <= 0.01) {
            return;
        }
        const int n = std::max(1, static_cast<int>(std::ceil(len / profile.panel_max)));
        const double seg = len / n;
        for (int i = 0; i < n; ++i) {
            add_panel(meshes.glass, from + (static_cast<double>(i) + 0.5) * seg,
                      static_cast<float>(seg * 0.5), psd_lat);
        }
    };

    // Les portes, converties en chainage et triées. `door_centers` donne des
    // distances d'ARC depuis le centre de la gare — c'est la seule unité qui a un
    // sens ici, puisque c'est en distance d'arc que la physique place les voitures.
    // Les additionner au chainage comme si c'était la même chose décalait les baies
    // d'autant plus que la voiture était loin de la motrice (cf. chainage_at_arc).
    std::vector<double> doors;
    doors.reserve(profile.door_centers.size());
    for (const double rel : profile.door_centers) {
        const double sc = chainage_at_arc(track, s_center, rel);
        const double half_bay = static_cast<double>(profile.door_half + 2.0f * jamb);
        if (sc - half_bay > s0 && sc + half_bay < s1) {
            doors.push_back(sc);
        }
    }
    std::sort(doors.begin(), doors.end());

    double cursor = s0;
    for (const double sc : doors) {
        const double a = sc - static_cast<double>(profile.door_half);  // rive de la baie
        const double b = sc + static_cast<double>(profile.door_half);
        // Verre fixe jusqu'au montant, puis les deux montants qui bordent la baie.
        add_fixed_run(cursor, a - 2.0 * static_cast<double>(jamb));
        add_panel(out, a - static_cast<double>(jamb), jamb, psd_lat);
        add_panel(out, b + static_cast<double>(jamb), jamb, psd_lat);
        // Deux demi-vantaux bi-parting : `a` s'efface vers les chainages décroissants,
        // `b` vers les croissants. Chacun fait la moitié de la baie, donc s'ouvrir de
        // sa propre longueur suffit à dégager tout le passage.
        const float leaf = profile.door_half * 0.5f;
        add_panel(meshes.psd_doors, sc - static_cast<double>(leaf), leaf, leaf_lat);
        add_panel(meshes.psd_doors_b, sc + static_cast<double>(leaf), leaf, leaf_lat);
        cursor = b + 2.0 * static_cast<double>(jamb);
    }
    add_fixed_run(cursor, s1);

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

    // --- REPÈRE D'ARRÊT (停止位置目標) ----------------------------------------
    // M52 avait corrigé sa POSITION : le losange est au chainage EXACT de la cabine
    // quand le centre de la rame coïncide avec celui de la gare, c'est-à-dire au
    // point d'arrêt dont dérive aussi `door_centers`. Repère et baies vitrées
    // sortent du MÊME calcul : s'aligner sur le losange, c'est nécessairement avoir
    // chaque porte de la rame en face de sa baie.
    //
    // M53 corrige ce qu'il en restait d'INUTILISABLE. Il y avait bien un repère,
    // mais un seul, du côté droit, planté au MILIEU du quai à +2,00 m — soit
    // derrière la façade vitrée et hors de l'axe du regard. Viser « à peu près le
    // petit losange » ne permet pas de tenir 50 cm. Trois choses changent :
    //   1. il est posé des DEUX CÔTÉS (la ligne est banale dans les deux sens) ;
    //   2. il monte à +2,95 m, au-dessus de la façade, à hauteur d'œil de cabine ;
    //   3. une LIGNE D'ARRÊT est peinte en travers des deux quais à son chainage.
    // Le losange dit « où », la ligne dit « exactement où » : sur les derniers
    // mètres, c'est elle qu'on regarde, parce qu'une ligne se juge au centimètre
    // alors qu'un panneau vu de biais, non.
    {
        const double s_mark = chainage_at_arc(track, s_center, profile.stop_marker_offset);
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(s_mark, pos_world, tangent);
        const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
        const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
        // Même règle que les façades : on s'élève selon la NORMALE DE LA VOIE, sinon
        // le repère glisse le long du quai sur les sections en pente — et c'est
        // précisément lui qui ne doit PAS bouger d'un millimètre.
        const glm::vec3 up_frame = glm::normalize(glm::cross(right, forward));
        const glm::vec3 anchor = glm::vec3(pos_world - origin);
        const float k = 0.70710678f;  // cos/sin 45°

        for (const float sign : {-1.0f, 1.0f}) {
            const glm::vec3 lat = right * (sign * profile.marker_lateral);
            // Losange : carré tourné de 45° dans le plan (travers voie, vertical),
            // épaisseur portée par `forward` => la plaque FAIT FACE au train.
            const glm::vec3 diag_r = (right * sign + up_frame) * k;
            const glm::vec3 diag_u = (up_frame - right * sign) * k;
            const glm::vec3 mid = anchor + lat + up_frame * profile.marker_y;
            add_box(meshes.signs, mid, diag_r, forward, diag_u,
                    glm::vec3(profile.marker_half, profile.marker_half, 0.05f), uv_period);
            // Mât fin (4 cm) du dessus du quai au losange. Sa hauteur se RECALCULE
            // depuis le quai : codée en dur, elle aurait laissé le mât flotter au
            // premier changement de cote de dalle (c'est arrivé au M52).
            const float mast_h = profile.marker_y - top;
            add_box(out, anchor + lat + up_frame * (top + mast_h * 0.5f), right, forward,
                    up_frame, glm::vec3(0.02f, mast_h * 0.5f, 0.02f), uv_period);
        }

        // Ligne d'arrêt peinte, en travers des deux quais. Elle démarre au-delà de
        // la bande podotactile (deux surfaces ne se disputent pas le même plan) et
        // court jusqu'à la rive extérieure. Elle mord de 4 mm DANS la dalle et en
        // ressort de 6 mm : de la peinture épaisse, dont aucune face n'est
        // coplanaire avec le quai.
        const float line_in = profile.tactile_inner + profile.tactile_width + 0.02f;
        const float line_len = out_w - line_in;
        if (line_len > 0.1f) {
            for (const float sign : {-1.0f, 1.0f}) {
                const glm::vec3 mid = anchor +
                                      right * (sign * (line_in + line_len * 0.5f)) +
                                      up_frame * (top + 0.001f);
                add_box(meshes.markings, mid, right, forward, up_frame,
                        glm::vec3(line_len * 0.5f, 0.005f, profile.stop_line_half),
                        kTactileUvPeriod);
            }
        }
    }
    return meshes;
}

}  // namespace noire::scene
