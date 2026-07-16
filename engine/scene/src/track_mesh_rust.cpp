// PoC M13.5 — pont C++ ⇄ crate Rust `noire_procgen` pour la génération de voie.
//
// Rôle : le C++ garde tout ce qui est en DOUBLE (échantillonnage de TrackSource, origine
// flottante) et pré-projette la voie en tableaux POD `f32` ; Rust fait tout le maillage.
// C'est la frontière « batchée » : UN appel par tuile, pas un par sommet.
//
// La logique d'échantillonnage ci-dessous est le CALQUE EXACT de generate_track_mesh
// (track_mesh.cpp) — c'est ce qui garantit que Rust reçoit les mêmes entrées et produit,
// à l'epsilon flottant près, la même géométrie (cf. le banc A/B).

#include "noire/scene/track_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "noire_procgen.h"  // généré par cbindgen (build.rs)

namespace noire::scene {

namespace {

// Le memcpy en bloc n'est correct que si les deux sommets ont EXACTEMENT le même layout.
// On le vérifie à la compilation plutôt que de le supposer.
static_assert(sizeof(render::MeshVertex) == sizeof(ProcgenVertex),
              "MeshVertex (C++) et ProcgenVertex (Rust) doivent avoir la même taille");
static_assert(sizeof(render::MeshVertex) == 48, "MeshVertex attendu à 48 octets (12 float)");
static_assert(offsetof(render::MeshVertex, position) == 0);
static_assert(offsetof(render::MeshVertex, normal) == 12);
static_assert(offsetof(render::MeshVertex, uv) == 24);
static_assert(offsetof(render::MeshVertex, tangent) == 32);

ProcgenSample make_sample(const glm::dvec3& pos_world, const glm::dvec3& tangent,
                          const WorldPosition& origin, float u) {
    // Origine flottante + normalisation de la tangente : EN DOUBLE, puis conversion float.
    // Exactement comme le C++ d'origine, pour que right/up (recalculés en float côté Rust)
    // partent des mêmes bits que côté C++.
    const glm::vec3 center = glm::vec3(pos_world - origin);
    const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
    ProcgenSample s{};
    s.center[0] = center.x;
    s.center[1] = center.y;
    s.center[2] = center.z;
    s.forward[0] = forward.x;
    s.forward[1] = forward.y;
    s.forward[2] = forward.z;
    s.u = u;
    return s;
}

void copy_submesh(RailMeshData& dst, const ProcgenSubMesh& src) {
    if (src.vertex_count == 0 || src.index_count == 0) {
        return;  // sous-maillage vide (ex. traverses en LOD Distant)
    }
    dst.vertices.resize(src.vertex_count);
    std::memcpy(dst.vertices.data(), src.vertices, src.vertex_count * sizeof(render::MeshVertex));
    dst.indices.resize(src.index_count);
    std::memcpy(dst.indices.data(), src.indices, src.index_count * sizeof(std::uint32_t));
}

}  // namespace

TrackMeshData generate_track_mesh_rust(const TrackSource& track, double x_start, double x_end,
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
    const float uv_period = profile.uv_period > 0.01f ? profile.uv_period : 1.0f;

    // --- Sections (repères d'extrusion) : calque de la boucle `frames` du C++ ---
    std::vector<ProcgenSample> sections;
    sections.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double x = x_start + (static_cast<double>(i) / (count - 1)) * span;
        glm::dvec3 pos_world;
        glm::dvec3 tangent;
        track.sample(x, pos_world, tangent);
        const float u = static_cast<float>((x - x_start) / static_cast<double>(uv_period));
        sections.push_back(make_sample(pos_world, tangent, origin, u));
    }

    // --- Traverses : ancrées sur le chainage ABSOLU, uniquement en LOD Full ---
    std::vector<ProcgenSample> sleepers;
    if (full) {
        const double spacing = profile.sleeper_spacing > 0.01f ? profile.sleeper_spacing : 0.6;
        const double first = std::ceil(x_start / spacing) * spacing;
        for (double x = first; x < x_end; x += spacing) {
            glm::dvec3 pos_world;
            glm::dvec3 tangent;
            track.sample(x, pos_world, tangent);
            sleepers.push_back(make_sample(pos_world, tangent, origin, 0.0f));
        }
    }

    // --- Cotes du profil (miroir de RailProfile, champs utiles au maillage) ---
    ProcgenParams p{};
    p.uv_period = uv_period;
    p.gauge = static_cast<float>(profile.gauge);
    p.rail_height = profile.rail_height;
    p.rail_head_half = profile.rail_head_half;
    p.rail_web_half = profile.rail_web_half;
    p.rail_foot_half = profile.rail_foot_half;
    p.sleeper_half_length = profile.sleeper_half_length;
    p.sleeper_half_width = profile.sleeper_half_width;
    p.sleeper_thickness = profile.sleeper_thickness;
    p.ballast_crown_y = profile.ballast_crown_y;
    p.ballast_crown_half = profile.ballast_crown_half;
    p.ballast_base_y = profile.ballast_base_y;
    p.ballast_base_half = profile.ballast_base_half;
    p.lod_full = full ? 1u : 0u;

    // --- L'unique traversée de la frontière : tout le maillage se fait côté Rust ---
    const ProcgenMesh mesh = noire_procgen_generate_track(
        sections.data(), sections.size(),
        sleepers.empty() ? nullptr : sleepers.data(), sleepers.size(), &p);

    // Contrat d'ownership : Rust a alloué, le C++ COPIE des vues const, puis Rust LIBÈRE.
    copy_submesh(out.rails, mesh.rails);
    copy_submesh(out.sleepers, mesh.sleepers);
    copy_submesh(out.ballast, mesh.ballast);
    noire_procgen_free(mesh.handle);

    return out;
}

}  // namespace noire::scene
