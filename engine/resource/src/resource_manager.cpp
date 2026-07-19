#include "noire/resource/resource_manager.hpp"

#include <atomic>
#include <utility>

#include "noire/audio/audio_loader.hpp"
#include "noire/core/job_system.hpp"
#include "noire/core/log.hpp"
#include "noire/render/renderer.hpp"
#include "noire/resource/asset_paths.hpp"
#include "noire/resource/asset_types.hpp"
#include "noire/resource/gltf_loader.hpp"
#include "noire/resource/hdr_loader.hpp"
#include "noire/resource/image_loader.hpp"

namespace noire::resource {

// Slots internes : portent l'état atomique + la charge utile CPU pendant le vol.
struct ResourceManager::TextureSlot {
    TextureHandle handle;
    std::string full_path;
    render::TextureFormat format = render::TextureFormat::SrgbColor;
    std::atomic<ResourceState> state{ResourceState::Queued};
    ImageData cpu;  // rempli par le worker, consommé au pump
};

struct ResourceManager::ModelSlot {
    ModelHandle handle;
    std::string full_path;
    std::atomic<ResourceState> state{ResourceState::Queued};
    ModelData cpu;
    std::vector<TextureHandle> textures;    // textures GPU du modèle (créées au pump)
    std::vector<MaterialHandle> materials;  // matériaux GPU (créés au pump)
};

struct ResourceManager::AudioSlot {
    AudioHandle handle;
    std::string full_path;
    std::atomic<ResourceState> state{ResourceState::Queued};
    std::vector<float> cpu;  // PCM décodé par le worker
};

struct ResourceManager::EnvironmentSlot {
    EnvironmentHandle handle;
    std::string full_path;
    std::uint32_t face_size = 1024;
    std::atomic<ResourceState> state{ResourceState::Queued};
    CubemapData cpu;  // 6 faces RGBA16F produites par le worker (équirect -> cube)
};

ResourceManager::ResourceManager(render::Renderer& renderer, JobSystem& jobs,
                                 const AssetPaths& paths)
    : renderer_(renderer), jobs_(jobs), paths_(paths), recycler_(std::make_shared<Recycler>()) {}

// Défini ici (et non =default en en-tête) car les slots sont incomplets côté header.
// Les workers sont supposés arrêtés en amont (app::shutdown -> jobs.stop()) ; les
// ressources GPU restantes sont nettoyées par renderer.shutdown().
ResourceManager::~ResourceManager() = default;

TextureHandle ResourceManager::make_texture_handle() {
    std::shared_ptr<Recycler> rec = recycler_;
    return TextureHandle(new Texture(), [rec](Texture* t) {
        if (t->id != 0) {
            std::lock_guard<std::mutex> lock(rec->mutex);
            rec->textures.push_back(t->id);
        }
        delete t;
    });
}

EnvironmentHandle ResourceManager::make_environment_handle() {
    std::shared_ptr<Recycler> rec = recycler_;
    return EnvironmentHandle(new Environment(), [rec](Environment* e) {
        if (e->id != 0) {
            std::lock_guard<std::mutex> lock(rec->mutex);
            rec->environments.push_back(e->id);
        }
        delete e;
    });
}

MaterialHandle ResourceManager::make_material_handle() {
    std::shared_ptr<Recycler> rec = recycler_;
    return MaterialHandle(new Material(), [rec](Material* m) {
        if (m->id != 0) {
            std::lock_guard<std::mutex> lock(rec->mutex);
            rec->materials.push_back(m->id);
        }
        delete m;  // relâche les TextureHandle => elles se recyclent à leur tour
    });
}

ModelHandle ResourceManager::make_model_handle() {
    std::shared_ptr<Recycler> rec = recycler_;
    return ModelHandle(new Model(), [rec](Model* m) {
        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            for (Model::Primitive& prim : m->primitives) {
                if (prim.mesh != 0) {
                    rec->meshes.push_back(prim.mesh);
                }
            }
        }
        delete m;  // détruit les Primitive => les TextureHandle relâchés se recyclent aussi
    });
}

ModelHandle ResourceManager::load_model(const std::string& relative_path) {
    // Dédup : si déjà en cache et vivant, on renvoie la même instance.
    const auto cached = model_cache_.find(relative_path);
    if (cached != model_cache_.end()) {
        if (ModelHandle alive = cached->second.lock()) {
            return alive;
        }
    }

    ModelHandle model = make_model_handle();
    model_cache_[relative_path] = model;

    auto slot = std::make_shared<ModelSlot>();
    slot->handle = model;
    slot->full_path = paths_.resolve(relative_path);
    loading_models_.push_back(slot);

    if (!paths_.exists(relative_path)) {
        // On dit OÙ on a cherché : le fichier posé au mauvais endroit (ou la racine assets/
        // non trouvée depuis le répertoire de lancement) est la cause nº1 d'un « fallback ».
        log::warn("Asset introuvable : '{}' — cherché à '{}'{}", relative_path, slot->full_path,
                  paths_.valid()
                      ? ""
                      : " [racine 'assets/' NON trouvée depuis le répertoire courant — "
                        "définis NOIRE_ASSETS=/chemin/vers/assets]");
        slot->state.store(ResourceState::Failed, std::memory_order_relaxed);
        return model;
    }

    // Chargement CPU sur un worker ; `slot` (shared_ptr) capturé => reste vivant
    // tant que le job tourne, indépendamment de loading_models_.
    jobs_.submit([slot] {
        slot->state.store(ResourceState::Loading, std::memory_order_relaxed);
        ModelData data;
        if (load_gltf(slot->full_path, data)) {
            slot->cpu = std::move(data);
            slot->state.store(ResourceState::CpuReady, std::memory_order_release);
        } else {
            slot->state.store(ResourceState::Failed, std::memory_order_release);
        }
    });
    return model;
}

TextureHandle ResourceManager::load_texture(const std::string& relative_path,
                                           render::TextureFormat format) {
    const auto cached = texture_cache_.find(relative_path);
    if (cached != texture_cache_.end()) {
        if (TextureHandle alive = cached->second.lock()) {
            return alive;
        }
    }

    TextureHandle texture = make_texture_handle();
    texture_cache_[relative_path] = texture;

    auto slot = std::make_shared<TextureSlot>();
    slot->handle = texture;
    slot->full_path = paths_.resolve(relative_path);
    slot->format = format;
    loading_textures_.push_back(slot);

    if (!paths_.exists(relative_path)) {
        log::warn("Asset introuvable : '{}' — texture de secours (fallback)", relative_path);
        slot->state.store(ResourceState::Failed, std::memory_order_relaxed);
        return texture;
    }

    jobs_.submit([slot] {
        slot->state.store(ResourceState::Loading, std::memory_order_relaxed);
        ImageData data;
        if (load_image_file(slot->full_path, data)) {
            slot->cpu = std::move(data);
            slot->state.store(ResourceState::CpuReady, std::memory_order_release);
        } else {
            slot->state.store(ResourceState::Failed, std::memory_order_release);
        }
    });
    return texture;
}

EnvironmentHandle ResourceManager::load_environment(const std::string& relative_path,
                                                    std::uint32_t face_size) {
    const auto cached = environment_cache_.find(relative_path);
    if (cached != environment_cache_.end()) {
        if (EnvironmentHandle alive = cached->second.lock()) {
            return alive;
        }
    }

    EnvironmentHandle env = make_environment_handle();
    environment_cache_[relative_path] = env;

    auto slot = std::make_shared<EnvironmentSlot>();
    slot->handle = env;
    slot->full_path = paths_.resolve(relative_path);
    slot->face_size = face_size;
    loading_environments_.push_back(slot);

    if (!paths_.exists(relative_path)) {
        log::warn("Asset introuvable : '{}' — pas de ciel HDR (fond uni conservé)", relative_path);
        slot->state.store(ResourceState::Failed, std::memory_order_relaxed);
        return env;
    }

    // Décodage .hdr + rééchantillonnage équirect -> 6 faces de cube sur un worker : c'est
    // du pur calcul CPU (~6 M texels en 1024), il n'a rien à faire sur le thread de rendu.
    jobs_.submit([slot] {
        slot->state.store(ResourceState::Loading, std::memory_order_relaxed);
        CubemapData data;
        if (load_environment_file(slot->full_path, slot->face_size, data) && data.valid()) {
            slot->cpu = std::move(data);
            slot->state.store(ResourceState::CpuReady, std::memory_order_release);
        } else {
            slot->state.store(ResourceState::Failed, std::memory_order_release);
        }
    });
    return env;
}

AudioHandle ResourceManager::load_audio(const std::string& relative_path) {
    const auto cached = audio_cache_.find(relative_path);
    if (cached != audio_cache_.end()) {
        if (AudioHandle alive = cached->second.lock()) {
            return alive;
        }
    }

    AudioHandle clip = std::make_shared<AudioClip>();
    audio_cache_[relative_path] = clip;

    auto slot = std::make_shared<AudioSlot>();
    slot->handle = clip;
    slot->full_path = paths_.resolve(relative_path);
    loading_audio_.push_back(slot);

    if (!paths_.exists(relative_path)) {
        log::warn("Asset introuvable : '{}' — audio de synthèse (fallback M6)", relative_path);
        slot->state.store(ResourceState::Failed, std::memory_order_relaxed);
        return clip;
    }

    // Décodage CPU (ma_decoder) sur un worker : aucun périphérique audio requis.
    jobs_.submit([slot] {
        slot->state.store(ResourceState::Loading, std::memory_order_relaxed);
        std::vector<float> pcm;
        if (audio::decode_audio_file(slot->full_path, pcm)) {
            slot->cpu = std::move(pcm);
            slot->state.store(ResourceState::CpuReady, std::memory_order_release);
        } else {
            slot->state.store(ResourceState::Failed, std::memory_order_release);
        }
    });
    return clip;
}

void ResourceManager::drain_recycler() {
    std::vector<render::MeshId> meshes;
    std::vector<render::TextureId> textures;
    std::vector<render::MaterialId> materials;
    std::vector<render::EnvironmentId> environments;
    {
        std::lock_guard<std::mutex> lock(recycler_->mutex);
        meshes.swap(recycler_->meshes);
        textures.swap(recycler_->textures);
        materials.swap(recycler_->materials);
        environments.swap(recycler_->environments);
    }
    for (render::EnvironmentId id : environments) {
        renderer_.destroy_environment(id);
    }
    for (render::MeshId id : meshes) {
        renderer_.destroy_mesh(id);
    }
    // Les matériaux AVANT leurs textures : un matériau détruit ne référence plus rien.
    for (render::MaterialId id : materials) {
        renderer_.destroy_material(id);
    }
    for (render::TextureId id : textures) {
        renderer_.destroy_texture(id);
    }
}

void ResourceManager::pump_environments(int& budget) {
    for (auto it = loading_environments_.begin(); it != loading_environments_.end();) {
        EnvironmentSlot& slot = **it;
        const ResourceState state = slot.state.load(std::memory_order_acquire);

        if (state == ResourceState::Failed) {
            it = loading_environments_.erase(it);  // handle->id reste 0 => pas de skybox
            continue;
        }
        if (state == ResourceState::CpuReady && budget > 0) {
            slot.handle->id = renderer_.create_environment(slot.cpu.size, slot.cpu.texels.data());
            // Le soleil et les SH sont publiés AVANT de libérer le CPU : ils sont
            // minuscules et l'app en a besoin dès que le ciel est lié.
            slot.handle->sun_direction = slot.cpu.sun_direction;
            slot.handle->sun_color = slot.cpu.sun_color;
            slot.handle->sh = slot.cpu.sh;
            slot.cpu = CubemapData{};  // ~48 Mio rendus au système dès le staging fait
            slot.state.store(ResourceState::Uploading, std::memory_order_relaxed);
            --budget;
        }
        if (slot.state.load(std::memory_order_relaxed) == ResourceState::Uploading &&
            (slot.handle->id == 0 || renderer_.is_environment_ready(slot.handle->id))) {
            slot.handle->ready = slot.handle->id != 0;
            slot.state.store(ResourceState::GpuReady, std::memory_order_relaxed);
            it = loading_environments_.erase(it);
            continue;
        }
        ++it;
    }
}

void ResourceManager::pump_textures(int& budget) {
    for (auto it = loading_textures_.begin(); it != loading_textures_.end();) {
        TextureSlot& slot = **it;
        const ResourceState state = slot.state.load(std::memory_order_acquire);

        if (state == ResourceState::Failed) {
            it = loading_textures_.erase(it);  // handle->id reste 0 => fallback renderer
            continue;
        }
        if (state == ResourceState::CpuReady && budget > 0) {
            slot.handle->id = renderer_.create_texture(static_cast<std::uint32_t>(slot.cpu.width),
                                                       static_cast<std::uint32_t>(slot.cpu.height),
                                                       slot.cpu.pixels.data(), slot.format);
            slot.cpu = ImageData{};  // libère la RAM CPU une fois lancé vers le GPU
            slot.state.store(ResourceState::Uploading, std::memory_order_relaxed);
            --budget;
        }
        if (slot.state.load(std::memory_order_relaxed) == ResourceState::Uploading &&
            (slot.handle->id == 0 || renderer_.is_texture_ready(slot.handle->id))) {
            slot.state.store(ResourceState::GpuReady, std::memory_order_relaxed);
            it = loading_textures_.erase(it);
            continue;
        }
        ++it;
    }
}

void ResourceManager::pump_models(int& budget) {
    for (auto it = loading_models_.begin(); it != loading_models_.end();) {
        ModelSlot& slot = **it;
        const ResourceState state = slot.state.load(std::memory_order_acquire);

        if (state == ResourceState::Failed) {
            // L'échec est DÉJÀ journalisé à la source (load_model / load_gltf) ; on le
            // propage au handle pour que l'app puisse, elle aussi, arrêter d'attendre.
            slot.handle->failed = true;
            it = loading_models_.erase(it);  // model->primitives vide => fallback app
            continue;
        }
        if (state == ResourceState::CpuReady && budget > 0) {
            // 1) Une texture GPU par image décodée — l'espace colorimétrique vient du
            //    RÔLE que le loader lui a attribué (base color = sRGB, PBR = linéaire).
            slot.textures.clear();
            slot.textures.reserve(slot.cpu.images.size());
            for (ImageData& img : slot.cpu.images) {
                TextureHandle tex = make_texture_handle();
                tex->id = renderer_.create_texture(static_cast<std::uint32_t>(img.width),
                                                   static_cast<std::uint32_t>(img.height),
                                                   img.pixels.data(), img.color_space);
                slot.textures.push_back(std::move(tex));
            }

            // 2) Un matériau GPU (= un descriptor set 1) par matériau décodé. Il retient
            //    les handles de ses textures pour les garder vivantes.
            const auto image_texture = [&slot](int image_index) -> TextureHandle {
                if (image_index >= 0 &&
                    static_cast<std::size_t>(image_index) < slot.textures.size()) {
                    return slot.textures[static_cast<std::size_t>(image_index)];
                }
                return nullptr;
            };
            slot.materials.clear();
            slot.materials.reserve(slot.cpu.materials.size());
            for (const MaterialData& mat : slot.cpu.materials) {
                MaterialHandle handle = make_material_handle();
                render::MaterialDesc desc;
                desc.base_color_factor = mat.base_color_factor;
                desc.metallic_factor = mat.metallic_factor;
                desc.roughness_factor = mat.roughness_factor;
                desc.normal_scale = mat.normal_scale;
                // alphaMode MASK => feuillage : discard ET éclairage traversant.
                desc.foliage = mat.alpha_mask;
                for (auto [image_index, out_id] :
                     {std::pair{mat.base_color_image, &desc.base_color},
                      std::pair{mat.metallic_roughness_image, &desc.metallic_roughness},
                      std::pair{mat.normal_image, &desc.normal}}) {
                    if (TextureHandle tex = image_texture(image_index)) {
                        *out_id = tex->id;
                        handle->textures.push_back(std::move(tex));
                    }
                }
                handle->id = renderer_.create_material(desc);
                slot.materials.push_back(std::move(handle));
            }

            // 3) Un mesh device-local par primitive, puis on assemble le Model.
            Model& model = *slot.handle;
            model.primitives.clear();
            model.primitives.reserve(slot.cpu.primitives.size());
            for (const PrimitiveData& prim : slot.cpu.primitives) {
                Model::Primitive out_prim;
                out_prim.mesh = renderer_.create_mesh_indexed(prim.vertices, prim.indices);
                if (prim.material_index >= 0 &&
                    static_cast<std::size_t>(prim.material_index) < slot.materials.size()) {
                    out_prim.material = slot.materials[static_cast<std::size_t>(prim.material_index)];
                }
                model.primitives.push_back(std::move(out_prim));
            }
            slot.cpu = ModelData{};  // libère la RAM CPU
            slot.state.store(ResourceState::Uploading, std::memory_order_relaxed);
            --budget;
        }
        if (slot.state.load(std::memory_order_relaxed) == ResourceState::Uploading) {
            bool all_ready = true;
            for (const Model::Primitive& prim : slot.handle->primitives) {
                if (prim.mesh != 0 && !renderer_.is_mesh_ready(prim.mesh)) {
                    all_ready = false;
                    break;
                }
                // Un matériau n'est « prêt » que quand son set 1 a été écrit, donc quand
                // ses 3 textures sont téléversées.
                if (prim.material && prim.material->id != 0 &&
                    !renderer_.is_material_ready(prim.material->id)) {
                    all_ready = false;
                    break;
                }
            }
            if (all_ready) {
                slot.handle->ready = true;
                slot.state.store(ResourceState::GpuReady, std::memory_order_relaxed);
                it = loading_models_.erase(it);  // le Model reste vivant via le handle appelant
                continue;
            }
        }
        ++it;
    }
}

void ResourceManager::pump_audio() {
    for (auto it = loading_audio_.begin(); it != loading_audio_.end();) {
        AudioSlot& slot = **it;
        const ResourceState state = slot.state.load(std::memory_order_acquire);
        if (state == ResourceState::Failed) {
            it = loading_audio_.erase(it);  // clip vide => fallback synthé côté app
            continue;
        }
        if (state == ResourceState::CpuReady) {
            // Publication sur le thread principal (move O(1)). Aucun GPU => pas de budget.
            slot.handle->pcm = std::move(slot.cpu);
            slot.handle->ready = true;
            it = loading_audio_.erase(it);
            continue;
        }
        ++it;
    }
}

void ResourceManager::pump() {
    drain_recycler();
    int budget = upload_budget_ > 0 ? upload_budget_ : 1;
    // L'environnement en premier : c'est le plus gros staging du moteur (~48 Mio), autant
    // qu'il parte tôt dans la frame plutôt que derrière une file de petites textures.
    pump_environments(budget);
    pump_textures(budget);
    pump_models(budget);
    pump_audio();
}

}  // namespace noire::resource
