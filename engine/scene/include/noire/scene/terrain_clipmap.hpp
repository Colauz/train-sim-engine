#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/terrain.hpp"
#include "noire/render/vertex.hpp"

namespace noire {
class JobSystem;
}
namespace noire::render {
class Renderer;
}

namespace noire::scene {

struct ClipmapConfig {
    double base_cell = 8.0;       // maille du niveau 0 (m)
    int levels = 7;               // 7 niveaux => maille 8..512 m, portée ±16 km
    int cells_per_side = 64;      // par niveau ; le niveau N couvre exactement le trou du N+1
    float uv_period = 8.0f;       // 1 unité UV = 8 m : le grain des textures de sol
    // Référence FIXE des UV. Indispensable pour DEUX raisons :
    //  * précision — nos coordonnées monde valent ~1e6 ; un UV = wx/8 vaudrait 125 000, où
    //    l'ulp d'un float32 fait 0.008, soit 6 cm de quantification visible (mesuré) ;
    //  * stabilité — les caler sur le centre du clipmap, qui bouge tous les 16 m, ferait
    //    GLISSER la texture et le bruit de splatting sous les pieds du joueur.
    // Relatifs à une origine fixe, les UV restent dans ±2000 (ulp = 1 mm) ET absolus.
    glm::dvec2 uv_origin{0.0, 0.0};
};

// Terrain en GEO-CLIPMAP : des anneaux concentriques centrés sur le train, chacun deux
// fois plus grossier que le précédent. C'est ce qui permet 16 km de relief pour ~30 k
// sommets — une grille uniforme à 8 m sur la même étendue en demanderait 16 millions.
//
// Détails qui font que ça marche :
//   * chaque niveau se cale (« snap ») sur SA PROPRE maille : sans ça, ses sommets
//     glisseraient d'une frame à l'autre et le relief lointain ondulerait (« swimming ») ;
//   * le trou du niveau i est EXACTEMENT l'emprise du niveau i-1 — c'est automatique,
//     puisque i-1 se cale sur 2*maille(i-1) = maille(i), donc ses bords tombent
//     forcément sur la grille de i ;
//   * TOUS les niveaux vivent dans UN SEUL maillage : un seul upload par régénération
//     au lieu de 7, ce qui évite de saturer les 4 slots du TransferManager.
class TerrainClipmap {
public:
    TerrainClipmap(const Terrain& terrain, JobSystem& jobs, ClipmapConfig config = {});
    ~TerrainClipmap();
    TerrainClipmap(const TerrainClipmap&) = delete;
    TerrainClipmap& operator=(const TerrainClipmap&) = delete;

    // Thread principal, 1x/frame. Régénère (async) quand le train a franchi une maille
    // du niveau 0, et publie le nouveau maillage dès qu'il est téléversé.
    void update(const WorldPosition& viewer, render::Renderer& renderer);

    [[nodiscard]] render::MeshId mesh() const { return mesh_; }
    [[nodiscard]] const WorldPosition& origin() const { return origin_; }
    [[nodiscard]] bool ready() const { return mesh_ != 0; }

private:
    enum class State : std::uint8_t { Idle, Generating, CpuReady };

    struct Payload {
        std::vector<render::MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
        WorldPosition origin{};
        glm::i64vec2 snap{0, 0};
    };

    void request(const glm::i64vec2& snap, const WorldPosition& center);

    const Terrain& terrain_;
    JobSystem& jobs_;
    ClipmapConfig config_;

    std::atomic<State> state_{State::Idle};
    std::shared_ptr<Payload> pending_;  // écrit par le worker, lu au pump
    glm::i64vec2 current_snap_{0, 0};
    bool has_snap_ = false;

    render::MeshId mesh_ = 0;
    WorldPosition origin_{};
};

}  // namespace noire::scene
