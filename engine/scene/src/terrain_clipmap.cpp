#include "noire/scene/terrain_clipmap.hpp"

#include <cmath>

#include "noire/core/job_system.hpp"
#include "noire/render/renderer.hpp"

namespace noire::scene {

namespace {

double snap_to(double v, double step) { return std::floor(v / step) * step; }

// Construit UN maillage contenant tous les niveaux. Fonction PURE (rien que le Terrain,
// qui est lui-même sans état) => appelable depuis un worker.
void build(const Terrain& terrain, const ClipmapConfig& cfg, const WorldPosition& center,
           std::vector<render::MeshVertex>& vertices, std::vector<std::uint32_t>& indices) {
    const int n = cfg.cells_per_side;
    const float period = cfg.uv_period > 0.01f ? cfg.uv_period : 1.0f;

    // Emprise du niveau précédent : c'est le trou à percer dans le niveau courant.
    // Initialisée « vide » pour que le niveau 0 soit plein.
    double inner_x0 = 0.0, inner_x1 = -1.0, inner_z0 = 0.0, inner_z1 = -1.0;

    for (int level = 0; level < cfg.levels; ++level) {
        const double cell = cfg.base_cell * static_cast<double>(1 << level);
        // Chaque niveau se cale sur DEUX mailles : c'est la condition pour que ses bords
        // tombent sur la grille du niveau supérieur, et donc que le trou coïncide au
        // texel près (cf. l'en-tête).
        const double cx = snap_to(center.x, cell * 2.0);
        const double cz = snap_to(center.z, cell * 2.0);
        const double half = static_cast<double>(n) * 0.5 * cell;
        const double x0 = cx - half;
        const double z0 = cz - half;

        const auto base = static_cast<std::uint32_t>(vertices.size());
        for (int j = 0; j <= n; ++j) {
            const double wz = z0 + static_cast<double>(j) * cell;
            for (int i = 0; i <= n; ++i) {
                const double wx = x0 + static_cast<double>(i) * cell;
                const double h = terrain.height(wx, wz);
                // La normale est échantillonnée à l'échelle de la MAILLE : sur un niveau
                // grossier, un pas de 1 m capterait un détail que la géométrie ne porte
                // pas, et l'éclairage jurerait avec la silhouette.
                const glm::dvec3 nd = terrain.normal(wx, wz, cell * 0.5);

                const auto normal = glm::vec3(nd);
                // Tangente : +x projeté sur le plan tangent. w = -1 pour que la
                // bitangente cross(N,T)*w suive +z (même convention que l'ancien sol).
                glm::vec3 tangent(1.0f, 0.0f, 0.0f);
                tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));

                render::MeshVertex v;
                v.position = glm::vec3(glm::dvec3(wx, h, wz) - center);  // origine flottante
                v.normal = normal;
                v.uv = glm::vec2(static_cast<float>((wx - cfg.uv_origin.x) / period),
                                 static_cast<float>((wz - cfg.uv_origin.y) / period));
                v.tangent = glm::vec4(tangent, -1.0f);
                vertices.push_back(v);
            }
        }

        for (int j = 0; j < n; ++j) {
            const double qz0 = z0 + static_cast<double>(j) * cell;
            const double qz1 = qz0 + cell;
            for (int i = 0; i < n; ++i) {
                const double qx0 = x0 + static_cast<double>(i) * cell;
                const double qx1 = qx0 + cell;
                // Trou : on saute le quad s'il est ENTIÈREMENT couvert par le niveau
                // précédent. Le test est strict des deux côtés, donc aucun recouvrement
                // (qui z-fighterait) ni aucun jour entre les deux niveaux.
                if (qx0 >= inner_x0 && qx1 <= inner_x1 && qz0 >= inner_z0 && qz1 <= inner_z1) {
                    continue;
                }
                const auto a = base + static_cast<std::uint32_t>(j * (n + 1) + i);
                const auto b = a + 1;
                const auto c = a + static_cast<std::uint32_t>(n + 1);
                const auto d = c + 1;
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }

        inner_x0 = x0;
        inner_x1 = x0 + static_cast<double>(n) * cell;
        inner_z0 = z0;
        inner_z1 = z0 + static_cast<double>(n) * cell;
    }
}

}  // namespace

TerrainClipmap::TerrainClipmap(const Terrain& terrain, JobSystem& jobs, ClipmapConfig config)
    : terrain_(terrain), jobs_(jobs), config_(config) {}

TerrainClipmap::~TerrainClipmap() = default;

void TerrainClipmap::request(const glm::i64vec2& snap, const WorldPosition& center) {
    auto payload = std::make_shared<Payload>();
    payload->origin = center;
    payload->snap = snap;
    pending_ = payload;
    state_.store(State::Generating, std::memory_order_relaxed);

    const Terrain* terrain = &terrain_;
    const ClipmapConfig cfg = config_;
    std::atomic<State>* state = &state_;
    jobs_.submit([payload, terrain, cfg, state] {
        build(*terrain, cfg, payload->origin, payload->vertices, payload->indices);
        state->store(State::CpuReady, std::memory_order_release);
    });
}

void TerrainClipmap::update(const WorldPosition& viewer, render::Renderer& renderer) {
    // Le pas de recalage est celui du niveau 0 : c'est lui qui bouge le plus souvent, et
    // les niveaux grossiers, recalculés pour rien, sont IDENTIQUES d'une fois sur l'autre
    // (ils se calent sur leur propre maille) — donc aucun scintillement au loin.
    const double step = config_.base_cell * 2.0;
    const glm::i64vec2 snap{static_cast<std::int64_t>(std::floor(viewer.x / step)),
                            static_cast<std::int64_t>(std::floor(viewer.z / step))};

    const State state = state_.load(std::memory_order_acquire);
    if (state == State::CpuReady && pending_) {
        const render::MeshId fresh =
            renderer.create_mesh_indexed(pending_->vertices, pending_->indices);
        if (fresh != 0) {
            // L'ancien maillage a été affiché jusqu'à cette frame incluse : sa
            // destruction est DIFFÉRÉE côté Renderer, la substitution est donc sûre même
            // s'il est encore référencé par une frame en vol.
            if (mesh_ != 0) {
                renderer.destroy_mesh(mesh_);
            }
            mesh_ = fresh;
            origin_ = pending_->origin;
            current_snap_ = pending_->snap;
            has_snap_ = true;
        }
        pending_.reset();
        state_.store(State::Idle, std::memory_order_relaxed);
    }

    if (state_.load(std::memory_order_relaxed) == State::Idle &&
        (!has_snap_ || snap != current_snap_)) {
        request(snap, WorldPosition{static_cast<double>(snap.x) * step, 0.0,
                                    static_cast<double>(snap.y) * step});
    }
}

}  // namespace noire::scene
