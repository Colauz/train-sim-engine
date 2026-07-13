#include "noire/resource/resource_manager.hpp"

#include <atomic>
#include <utility>

#include "noire/core/job_system.hpp"
#include "noire/core/log.hpp"
#include "noire/render/renderer.hpp"
#include "noire/resource/asset_paths.hpp"
#include "noire/resource/asset_types.hpp"
#include "noire/resource/gltf_loader.hpp"
#include "noire/resource/image_loader.hpp"

namespace noire::resource {

// Slots internes : portent l'état atomique + la charge utile CPU pendant le vol.
struct ResourceManager::TextureSlot {
    TextureHandle handle;
    std::string full_path;
    std::atomic<ResourceState> state{ResourceState::Queued};
    ImageData cpu;  // rempli par le worker, consommé au pump
};

struct ResourceManager::ModelSlot {
    ModelHandle handle;
    std::string full_path;
    std::atomic<ResourceState> state{ResourceState::Queued};
    ModelData cpu;
    std::vector<TextureHandle> textures;  // textures GPU du modèle (créées au pump)
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
        log::warn("Asset introuvable : '{}' — modèle vide (l'app appliquera son fallback)",
                  relative_path);
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

TextureHandle ResourceManager::load_texture(const std::string& relative_path) {
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

void ResourceManager::drain_recycler() {
    std::vector<render::MeshId> meshes;
    std::vector<render::TextureId> textures;
    {
        std::lock_guard<std::mutex> lock(recycler_->mutex);
        meshes.swap(recycler_->meshes);
        textures.swap(recycler_->textures);
    }
    for (render::MeshId id : meshes) {
        renderer_.destroy_mesh(id);
    }
    for (render::TextureId id : textures) {
        renderer_.destroy_texture(id);
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
                                                       slot.cpu.pixels.data());
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
            it = loading_models_.erase(it);  // model->primitives vide => fallback app
            continue;
        }
        if (state == ResourceState::CpuReady && budget > 0) {
            // 1) Une texture GPU par image décodée du modèle.
            slot.textures.clear();
            slot.textures.reserve(slot.cpu.images.size());
            for (ImageData& img : slot.cpu.images) {
                TextureHandle tex = make_texture_handle();
                tex->id = renderer_.create_texture(static_cast<std::uint32_t>(img.width),
                                                   static_cast<std::uint32_t>(img.height),
                                                   img.pixels.data());
                slot.textures.push_back(std::move(tex));
            }
            // 2) Un mesh device-local par primitive, puis on assemble le Model.
            Model& model = *slot.handle;
            model.primitives.clear();
            model.primitives.reserve(slot.cpu.primitives.size());
            for (const PrimitiveData& prim : slot.cpu.primitives) {
                Model::Primitive out_prim;
                out_prim.mesh = renderer_.create_mesh_indexed(prim.vertices, prim.indices);
                if (prim.image_index >= 0 &&
                    static_cast<std::size_t>(prim.image_index) < slot.textures.size()) {
                    out_prim.texture = slot.textures[static_cast<std::size_t>(prim.image_index)];
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
                if (prim.texture && prim.texture->id != 0 &&
                    !renderer_.is_texture_ready(prim.texture->id)) {
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

void ResourceManager::pump() {
    drain_recycler();
    int budget = upload_budget_ > 0 ? upload_budget_ : 1;
    pump_textures(budget);
    pump_models(budget);
}

}  // namespace noire::resource
