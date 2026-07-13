#include "noire/resource/gltf_loader.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

#include <cgltf.h>

#include "noire/core/log.hpp"
#include "noire/resource/image_loader.hpp"

namespace noire::resource {

namespace {

const cgltf_accessor* find_accessor(const cgltf_primitive& prim, cgltf_attribute_type type,
                                    cgltf_int set) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute& attr = prim.attributes[i];
        if (attr.type == type && attr.index == set) {
            return attr.data;
        }
    }
    return nullptr;
}

// Décode une image cgltf (embarquée via bufferView, ou externe via URI) en RGBA8,
// avec un cache local (une même cgltf_image ne produit qu'un seul ImageData).
int resolve_image(const cgltf_image* image, const std::string& gltf_dir, ModelData& out,
                  std::unordered_map<const cgltf_image*, int>& cache) {
    if (image == nullptr) {
        return -1;
    }
    const auto cached = cache.find(image);
    if (cached != cache.end()) {
        return cached->second;
    }

    ImageData decoded;
    bool ok = false;
    if (image->buffer_view != nullptr) {
        const cgltf_buffer_view* bv = image->buffer_view;
        const std::uint8_t* base =
            bv->data != nullptr ? static_cast<const std::uint8_t*>(bv->data)
                                : static_cast<const std::uint8_t*>(bv->buffer->data) + bv->offset;
        ok = decode_image_memory(base, bv->size, decoded);
    } else if (image->uri != nullptr && std::strncmp(image->uri, "data:", 5) != 0) {
        std::string uri = image->uri;
        cgltf_decode_uri(uri.data());  // décode les %xx en place
        uri.resize(std::strlen(uri.c_str()));
        const std::string full = (std::filesystem::path(gltf_dir) / uri).string();
        ok = load_image_file(full, decoded);
    } else {
        log::warn("glTF : image en data:URI non gérée — ignorée");
    }

    int index = -1;
    if (ok) {
        index = static_cast<int>(out.images.size());
        out.images.push_back(std::move(decoded));
    }
    cache[image] = index;
    return index;
}

}  // namespace

bool load_gltf(const std::string& path, ModelData& out) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        log::warn("glTF : parsing de '{}' échoué", path);
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        log::warn("glTF : chargement des buffers de '{}' échoué", path);
        cgltf_free(data);
        return false;
    }

    const std::string gltf_dir = std::filesystem::path(path).parent_path().string();
    std::unordered_map<const cgltf_image*, int> image_cache;

    for (cgltf_size m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& mesh = data->meshes[m];
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& prim = mesh.primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) {
                continue;  // on ne gère que les triangles au M7
            }
            const cgltf_accessor* pos = find_accessor(prim, cgltf_attribute_type_position, 0);
            if (pos == nullptr) {
                continue;
            }
            const cgltf_accessor* nrm = find_accessor(prim, cgltf_attribute_type_normal, 0);
            const cgltf_accessor* uv = find_accessor(prim, cgltf_attribute_type_texcoord, 0);

            PrimitiveData out_prim;
            const cgltf_size vcount = pos->count;
            out_prim.vertices.resize(vcount);
            for (cgltf_size i = 0; i < vcount; ++i) {
                render::MeshVertex& vertex = out_prim.vertices[i];
                cgltf_float tmp[3] = {0.0f, 0.0f, 0.0f};
                cgltf_accessor_read_float(pos, i, tmp, 3);
                vertex.position = glm::vec3(tmp[0], tmp[1], tmp[2]);
                if (nrm != nullptr) {
                    cgltf_accessor_read_float(nrm, i, tmp, 3);
                    vertex.normal = glm::vec3(tmp[0], tmp[1], tmp[2]);
                } else {
                    vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                cgltf_float uvt[2] = {0.0f, 0.0f};
                if (uv != nullptr) {
                    cgltf_accessor_read_float(uv, i, uvt, 2);
                }
                vertex.uv = glm::vec2(uvt[0], uvt[1]);
            }

            if (prim.indices != nullptr) {
                const cgltf_size icount = prim.indices->count;
                out_prim.indices.resize(icount);
                for (cgltf_size i = 0; i < icount; ++i) {
                    out_prim.indices[i] =
                        static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i));
                }
            } else {
                out_prim.indices.resize(vcount);
                for (cgltf_size i = 0; i < vcount; ++i) {
                    out_prim.indices[i] = static_cast<std::uint32_t>(i);
                }
            }

            if (prim.material != nullptr && prim.material->has_pbr_metallic_roughness) {
                const cgltf_texture* tex =
                    prim.material->pbr_metallic_roughness.base_color_texture.texture;
                if (tex != nullptr) {
                    out_prim.image_index = resolve_image(tex->image, gltf_dir, out, image_cache);
                }
            }

            out.primitives.push_back(std::move(out_prim));
        }
    }

    cgltf_free(data);

    if (!out.valid()) {
        log::warn("glTF : '{}' sans primitive triangulée exploitable", path);
        return false;
    }
    log::info("glTF : '{}' chargé — {} primitive(s), {} image(s)", path, out.primitives.size(),
              out.images.size());
    return true;
}

}  // namespace noire::resource
