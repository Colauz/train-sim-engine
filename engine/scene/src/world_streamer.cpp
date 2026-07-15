#include "noire/scene/world_streamer.hpp"

#include <cmath>

#include "noire/core/job_system.hpp"
#include "noire/render/renderer.hpp"

namespace noire::scene {

WorldStreamer::WorldStreamer(const TrackSource& track, JobSystem& jobs, StreamerConfig config)
    : track_(track), jobs_(jobs), config_(config) {}

WorldStreamer::~WorldStreamer() = default;

TrackLod WorldStreamer::desired_lod(long index, long current) const {
    return std::abs(index - current) <= config_.full_lod_radius ? TrackLod::Full : TrackLod::Distant;
}

void WorldStreamer::generate_async(Chunk& chunk, TrackLod lod) {
    chunk.building_lod = lod;
    chunk.state.store(State::Generating, std::memory_order_relaxed);

    // Génération asynchrone : sommets CPU purs, AUCUNE API Vulkan ici.
    Chunk* raw = &chunk;
    const double x0 = chunk.x_start;
    const double x1 = chunk.x_end;
    const WorldPosition origin = chunk.origin;
    const RailProfile profile = config_.rail_profile;
    jobs_.submit([this, raw, x0, x1, origin, profile, lod] {
        raw->cpu_mesh = generate_track_mesh(track_, x0, x1, origin, profile, lod);
        // Barrière release : rend visibles les écritures ci-dessus au thread principal.
        raw->state.store(State::CpuReady, std::memory_order_release);
    });
}

void WorldStreamer::request_chunk(long index, TrackLod lod) {
    auto chunk = std::make_unique<Chunk>();
    chunk->index = index;
    chunk->x_start = static_cast<double>(index) * config_.chunk_length;
    chunk->x_end = chunk->x_start + config_.chunk_length;

    // Origine du chunk = point de voie à son début (origine flottante par tuile).
    glm::dvec3 pos;
    glm::dvec3 tangent;
    track_.sample(chunk->x_start, pos, tangent);
    chunk->origin = pos;

    Chunk* raw = chunk.get();
    chunks_.emplace(index, std::move(chunk));
    generate_async(*raw, lod);
}

void WorldStreamer::update(double wagon_chainage, render::Renderer& renderer) {
    const long current = static_cast<long>(std::floor(wagon_chainage / config_.chunk_length));
    const long lo = current - config_.chunks_behind;
    const long hi = current + config_.chunks_ahead;

    // 1) Demander les tuiles manquantes dans la fenêtre (génération asynchrone).
    for (long i = lo; i <= hi; ++i) {
        if (chunks_.find(i) == chunks_.end()) {
            request_chunk(i, desired_lod(i, current));
        }
    }

    // 1bis) Re-générer les tuiles dont le LOD ne correspond plus (le train a avancé).
    // On NE touche PAS aux maillages affichés : la tuile continue d'être dessinée à son
    // ancien LOD pendant que le worker bâtit le nouveau, donc aucun trou. À 90 km/h et
    // 2 km par tuile, ça n'arrive qu'une fois toutes les ~80 s : le coût est nul.
    for (auto& [index, chunk] : chunks_) {
        const TrackLod want = desired_lod(index, current);
        if (chunk->state.load(std::memory_order_acquire) == State::Active && chunk->lod != want) {
            generate_async(*chunk, want);
        }
    }

    // 2) Uploader vers le GPU les tuiles prêtes, avec budget anti-pic de latence.
    int uploads = 0;
    for (auto& [index, chunk] : chunks_) {
        if (uploads >= config_.max_uploads_per_update) {
            break;
        }
        if (chunk->state.load(std::memory_order_acquire) == State::CpuReady) {
            // Chemin indexé device-local : la voie est au format PBR (MeshVertex) => elle
            // passe par le pipeline texturé, donc s'éclaire et REÇOIT les ombres, en plus
            // d'en projeter. Les 3 sous-maillages partent ensemble : ils forment un tout
            // visuel, en séparer l'upload ferait apparaître des rails sans ballast.
            auto upload = [&renderer](const RailMeshData& m) {
                return m.empty() ? 0u : renderer.create_mesh_indexed(m.vertices, m.indices);
            };
            const render::MeshId new_rails = upload(chunk->cpu_mesh.rails);
            const render::MeshId new_sleepers = upload(chunk->cpu_mesh.sleepers);
            const render::MeshId new_ballast = upload(chunk->cpu_mesh.ballast);

            // Changement de LOD : les anciens maillages ont été affichés jusqu'à cette
            // frame incluse. Leur destruction est DIFFÉRÉE côté Renderer, donc la
            // substitution est sûre même s'ils sont encore référencés par une frame en vol.
            if (chunk->has_mesh) {
                for (const render::MeshId id : {chunk->rails, chunk->sleepers, chunk->ballast}) {
                    if (id != 0) {
                        renderer.destroy_mesh(id);
                    }
                }
            }
            chunk->rails = new_rails;
            chunk->sleepers = new_sleepers;
            chunk->ballast = new_ballast;
            chunk->lod = chunk->building_lod;
            chunk->has_mesh = true;
            chunk->state.store(State::Active, std::memory_order_relaxed);
            chunk->cpu_mesh = TrackMeshData{};  // libère la RAM CPU une fois sur le GPU
            ++uploads;
        }
    }

    // 3) Garbage collection : décharger les tuiles hors fenêtre (RAM + GPU).
    //    On NE supprime JAMAIS une tuile encore en génération (le worker y écrit).
    const long unload_lo = lo - 1;  // légère hystérésis pour éviter le yo-yo
    const long unload_hi = hi + 1;
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        Chunk* c = it->second.get();
        const bool out_of_range = (c->index < unload_lo || c->index > unload_hi);
        const State state = c->state.load(std::memory_order_acquire);
        if (out_of_range && state != State::Generating) {
            if (c->has_mesh) {
                // Destruction GPU DIFFÉRÉE (voir Renderer) : 0 est ignoré côté renderer.
                for (const render::MeshId id : {c->rails, c->sleepers, c->ballast}) {
                    if (id != 0) {
                        renderer.destroy_mesh(id);
                    }
                }
            }
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }

    // 4) Liste des tuiles dessinables (thread principal).
    renderables_.clear();
    for (auto& [index, chunk] : chunks_) {
        if (chunk->has_mesh) {
            renderables_.push_back(
                ChunkRenderInfo{chunk->rails, chunk->sleepers, chunk->ballast, chunk->origin});
        }
    }
}

int WorldStreamer::active_chunk_count() const {
    int count = 0;
    for (const auto& [index, chunk] : chunks_) {
        if (chunk->has_mesh) {
            ++count;
        }
    }
    return count;
}

}  // namespace noire::scene
