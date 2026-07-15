#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/render/vertex.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire {
class JobSystem;
}
namespace noire::render {
class Renderer;
}

namespace noire::scene {

struct StreamerConfig {
    double chunk_length = 2000.0;      // longueur d'une tuile de voie (m)
    long chunks_ahead = 2;             // tuiles chargées devant le train
    long chunks_behind = 1;            // tuiles conservées derrière
    int max_uploads_per_update = 1;    // budget d'upload GPU par frame (anti-pic)
    RailProfile rail_profile{};
};

// Info de rendu d'un chunk : ses maillages + son origine (pour l'origine flottante).
// TROIS maillages depuis le M9 : un maillage ne porte qu'un descriptor set, et acier /
// béton / gravier sont trois matériaux distincts. Un identifiant peut valoir 0 (partie
// vide, ex. pas de traverse dans une tuile plus courte que l'entraxe).
struct ChunkRenderInfo {
    render::MeshId rails = 0;
    render::MeshId sleepers = 0;
    render::MeshId ballast = 0;
    WorldPosition origin{};
};

// Streaming de la voie infinie par tuiles.
//
// Concurrence :
//   * la GÉNÉRATION (splines + sommets) tourne sur des workers (JobSystem) ;
//   * le hand-off worker -> thread principal se fait via `std::atomic<State>`
//     (release/acquire) : aucune donnée de mesh n'est partagée sous verrou ;
//   * TOUS les appels GPU (upload/destruction) restent sur le thread principal ;
//   * un chunk en cours de génération n'est JAMAIS supprimé (le worker écrit dedans).
class WorldStreamer {
public:
    WorldStreamer(const TrackSource& track, JobSystem& jobs, StreamerConfig config = {});
    ~WorldStreamer();
    WorldStreamer(const WorldStreamer&) = delete;
    WorldStreamer& operator=(const WorldStreamer&) = delete;

    // Thread principal, 1x/frame : (dé)charge selon la position du wagon (chainage).
    void update(double wagon_chainage, render::Renderer& renderer);

    [[nodiscard]] const std::vector<ChunkRenderInfo>& renderables() const { return renderables_; }
    [[nodiscard]] int active_chunk_count() const;

private:
    enum class State : std::uint8_t { Generating, CpuReady, Active };

    struct Chunk {
        long index = 0;
        double x_start = 0.0;
        double x_end = 0.0;
        WorldPosition origin{};
        std::atomic<State> state{State::Generating};
        TrackMeshData cpu_mesh;  // rempli par le worker (rails + traverses + ballast)
        render::MeshId rails = 0;
        render::MeshId sleepers = 0;
        render::MeshId ballast = 0;
        bool has_mesh = false;
    };

    void request_chunk(long index);

    const TrackSource& track_;
    JobSystem& jobs_;
    StreamerConfig config_;
    std::unordered_map<long, std::unique_ptr<Chunk>> chunks_;
    std::vector<ChunkRenderInfo> renderables_;
};

}  // namespace noire::scene
