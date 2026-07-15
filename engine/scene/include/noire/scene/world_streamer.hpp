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
    // Rayon (en tuiles) de la zone en LOD complet autour du train. 1 => la tuile courante
    // et ses deux voisines immédiates, soit ~2 à 4 km de voie détaillée : bien au-delà de
    // ce que l'oeil distingue, et déjà 3 tuiles pleines à porter.
    long full_lod_radius = 1;
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
        // Maillages créés mais dont le TRANSFERT GPU est encore en vol. Ils ne remplacent
        // les précédents qu'une fois tous les trois `is_mesh_ready()`. Même sas que
        // TerrainClipmap, et pour la même raison : `create_mesh_indexed` est asynchrone et
        // le Renderer saute tout maillage non prêt — substituer tout de suite faisait
        // disparaître la tuile le temps du téléversement.
        render::MeshId up_rails = 0;
        render::MeshId up_sleepers = 0;
        render::MeshId up_ballast = 0;
        bool uploading = false;
        TrackLod uploading_lod = TrackLod::Distant;
        TrackLod lod = TrackLod::Distant;      // LOD des maillages ACTUELLEMENT affichés
        TrackLod building_lod = TrackLod::Distant;  // LOD que le worker est en train de bâtir
    };

    // `lod` est fixé à la demande : une tuile régénérée à un autre LOD passe par ici.
    void request_chunk(long index, TrackLod lod);
    void generate_async(Chunk& chunk, TrackLod lod);
    [[nodiscard]] TrackLod desired_lod(long index, long current) const;

    const TrackSource& track_;
    JobSystem& jobs_;
    StreamerConfig config_;
    std::unordered_map<long, std::unique_ptr<Chunk>> chunks_;
    std::vector<ChunkRenderInfo> renderables_;
};

}  // namespace noire::scene
