#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "noire/render/vertex.hpp"  // render::MeshId, render::TextureId

namespace noire {
class JobSystem;
}
namespace noire::render {
class Renderer;
}

namespace noire::resource {

class AssetPaths;

// État d'une ressource dans le pipeline asynchrone (cf. pump()).
//   Queued -> Loading (worker) -> CpuReady -> Uploading (TransferManager) -> GpuReady
//   (Failed = fichier introuvable / décodage raté => l'app applique son fallback).
enum class ResourceState : std::uint8_t { Queued, Loading, CpuReady, Uploading, GpuReady, Failed };

// Enregistrement d'une texture GPU (référence une render::TextureId).
struct Texture {
    render::TextureId id = 0;
};

// Enregistrement d'un matériau GPU (M8 étape 3) : son identifiant côté renderer + les
// handles de ses textures, qu'il maintient vivantes tant que le matériau existe.
struct Material {
    render::MaterialId id = 0;
    std::vector<std::shared_ptr<Texture>> textures;
};

// Enregistrement d'un modèle : une ou plusieurs primitives (mesh + matériau). `ready`
// passe à true quand TOUTES les ressources GPU du modèle sont téléversées.
struct Model {
    struct Primitive {
        render::MeshId mesh = 0;
        std::shared_ptr<Material> material;  // nul => matériau par défaut côté renderer
    };
    std::vector<Primitive> primitives;
    bool ready = false;
    [[nodiscard]] bool empty() const { return primitives.empty(); }
};

// Environnement HDR (M8 étape 6a) : la cubemap du ciel côté GPU. `ready` passe à true
// quand les 6 faces sont téléversées ET leur chaîne de mips engendrée.
struct Environment {
    render::EnvironmentId id = 0;
    bool ready = false;
};

// Clip audio décodé (CPU) : PCM mono float32 48 kHz (cf. audio::decode_audio_file).
// `ready` passe à true quand le décodage asynchrone est publié (thread principal).
// Aucune ressource GPU : le cycle s'arrête à CpuReady (pas d'étape Uploading).
struct AudioClip {
    std::vector<float> pcm;
    bool ready = false;
    [[nodiscard]] bool empty() const { return pcm.empty(); }
};

using ModelHandle = std::shared_ptr<Model>;
using TextureHandle = std::shared_ptr<Texture>;
using MaterialHandle = std::shared_ptr<Material>;
using AudioHandle = std::shared_ptr<AudioClip>;
using EnvironmentHandle = std::shared_ptr<Environment>;

// Gestionnaire de ressources centralisé (M7 étape 4).
//   * load_*  : NON bloquant ; dédup par chemin via weak_ptr (une seule copie en
//               RAM/VRAM même si plusieurs objets partagent l'asset) ;
//   * les loaders CPU (cgltf/stb) tournent sur le JobSystem — aucun Vulkan ;
//   * pump()  : thread principal, 1x/frame — injecte le CpuReady dans le GPU avec un
//               budget d'upload (anti-pic), et recycle les ressources GPU des handles
//               relâchés (destruction différée côté Renderer).
class ResourceManager {
public:
    ResourceManager(render::Renderer& renderer, JobSystem& jobs, const AssetPaths& paths);
    ~ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    ModelHandle load_model(const std::string& relative_path);
    TextureHandle load_texture(const std::string& relative_path);
    // Charge un fichier audio (.wav/.mp3/...) décodé en PCM sur le JobSystem. NON
    // bloquant, dédup par chemin. Le PCM (handle->pcm) est prêt quand handle->ready.
    AudioHandle load_audio(const std::string& relative_path);
    // Charge un ciel HDR équirectangulaire (.hdr) et le rééchantillonne en cubemap sur le
    // JobSystem (aucun Vulkan côté worker), puis le téléverse au pump. NON bloquant.
    // `face_size` = côté d'une face de la cubemap (1024 => ~64 Mio de VRAM avec les mips).
    EnvironmentHandle load_environment(const std::string& relative_path,
                                       std::uint32_t face_size = 1024);

    void pump();  // thread principal, 1x/frame

    void set_upload_budget(int uploads_per_pump) { upload_budget_ = uploads_per_pump; }
    [[nodiscard]] std::size_t in_flight() const {
        return loading_models_.size() + loading_textures_.size() + loading_audio_.size() +
               loading_environments_.size();
    }

private:
    struct ModelSlot;
    struct TextureSlot;
    struct AudioSlot;
    struct EnvironmentSlot;

    // Cimetière GPU thread-safe : les deleters des handles y déposent les identifiants
    // à détruire ; pump() les draine sur le thread principal (le Renderer diffère
    // ensuite réellement la destruction). Partagé via shared_ptr pour survivre aux
    // handles encore vivants.
    struct Recycler {
        std::mutex mutex;
        std::vector<render::MeshId> meshes;
        std::vector<render::TextureId> textures;
        std::vector<render::MaterialId> materials;
        std::vector<render::EnvironmentId> environments;
    };

    ModelHandle make_model_handle();
    TextureHandle make_texture_handle();
    MaterialHandle make_material_handle();
    EnvironmentHandle make_environment_handle();
    void drain_recycler();
    void pump_textures(int& budget);
    void pump_models(int& budget);
    void pump_audio();  // pas de GPU : simple publication du PCM décodé
    void pump_environments(int& budget);

    render::Renderer& renderer_;
    JobSystem& jobs_;
    const AssetPaths& paths_;

    std::unordered_map<std::string, std::weak_ptr<Model>> model_cache_;
    std::unordered_map<std::string, std::weak_ptr<Texture>> texture_cache_;
    std::unordered_map<std::string, std::weak_ptr<AudioClip>> audio_cache_;
    std::unordered_map<std::string, std::weak_ptr<Environment>> environment_cache_;
    std::vector<std::shared_ptr<ModelSlot>> loading_models_;
    std::vector<std::shared_ptr<TextureSlot>> loading_textures_;
    std::vector<std::shared_ptr<AudioSlot>> loading_audio_;
    std::vector<std::shared_ptr<EnvironmentSlot>> loading_environments_;

    std::shared_ptr<Recycler> recycler_;
    int upload_budget_ = 2;
};

}  // namespace noire::resource
