#include "noire/resource/gltf_loader.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include <cgltf.h>

#include "noire/core/log.hpp"
#include "noire/resource/image_loader.hpp"

namespace noire::resource {

namespace {

// Une image glTF peut servir plusieurs rôles (rare mais légal) ; or le rôle décide de
// l'espace colorimétrique. La clé de cache est donc (image, espace) et non l'image seule.
using ImageCache = std::map<std::pair<const cgltf_image*, int>, int>;
using MaterialCache = std::unordered_map<const cgltf_material*, int>;

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
// avec un cache local (une même image dans un même espace ne décode qu'une fois).
int resolve_image(const cgltf_image* image, const std::string& gltf_dir,
                  render::TextureFormat color_space, ModelData& out, ImageCache& cache) {
    if (image == nullptr) {
        return -1;
    }
    const auto key = std::make_pair(image, static_cast<int>(color_space));
    const auto cached = cache.find(key);
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
        decoded.color_space = color_space;
        index = static_cast<int>(out.images.size());
        out.images.push_back(std::move(decoded));
    }
    cache[key] = index;
    return index;
}

// Extrait un matériau PBR metallic-roughness : les 3 textures + leurs facteurs.
int resolve_material(const cgltf_material* material, const std::string& gltf_dir, ModelData& out,
                     ImageCache& image_cache, MaterialCache& cache) {
    if (material == nullptr) {
        return -1;
    }
    const auto cached = cache.find(material);
    if (cached != cache.end()) {
        return cached->second;
    }

    MaterialData data;
    data.alpha_mask = material->alpha_mode == cgltf_alpha_mode_mask;
    if (material->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
        data.base_color_factor =
            glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                      pbr.base_color_factor[2], pbr.base_color_factor[3]);
        data.metallic_factor = pbr.metallic_factor;
        data.roughness_factor = pbr.roughness_factor;

        if (pbr.base_color_texture.texture != nullptr) {
            data.base_color_image = resolve_image(pbr.base_color_texture.texture->image, gltf_dir,
                                                  render::TextureFormat::SrgbColor, out,
                                                  image_cache);
        }
        // Metallic-roughness : données numériques => JAMAIS de décodage sRGB.
        if (pbr.metallic_roughness_texture.texture != nullptr) {
            data.metallic_roughness_image =
                resolve_image(pbr.metallic_roughness_texture.texture->image, gltf_dir,
                              render::TextureFormat::LinearData, out, image_cache);
        }
    }
    if (material->normal_texture.texture != nullptr) {
        data.normal_image = resolve_image(material->normal_texture.texture->image, gltf_dir,
                                          render::TextureFormat::LinearData, out, image_cache);
        data.normal_scale = material->normal_texture.scale;
    }

    const int index = static_cast<int>(out.materials.size());
    out.materials.push_back(data);
    cache[material] = index;
    return index;
}

// cgltf ne SAIT PAS générer les tangentes (c'est un parseur, pas une lib de géométrie) :
// on les calcule donc quand le fichier n'en fournit pas. Accumulation par triangle à
// partir des deltas d'UV (dP = dU*T + dV*B), puis orthonormalisation de Gram-Schmidt.
// Approximation de MikkTSpace : suffisante tant qu'on ne vise pas la parité stricte
// avec un baker externe.
void generate_tangents(PrimitiveData& prim) {
    const std::size_t vcount = prim.vertices.size();
    std::vector<glm::vec3> tangents(vcount, glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vcount, glm::vec3(0.0f));

    for (std::size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
        const std::uint32_t i0 = prim.indices[i];
        const std::uint32_t i1 = prim.indices[i + 1];
        const std::uint32_t i2 = prim.indices[i + 2];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) {
            continue;
        }
        const glm::vec3 e1 = prim.vertices[i1].position - prim.vertices[i0].position;
        const glm::vec3 e2 = prim.vertices[i2].position - prim.vertices[i0].position;
        const glm::vec2 d1 = prim.vertices[i1].uv - prim.vertices[i0].uv;
        const glm::vec2 d2 = prim.vertices[i2].uv - prim.vertices[i0].uv;

        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) {
            continue;  // UV dégénérés (triangle sans surface dans l'espace texture)
        }
        const float r = 1.0f / det;
        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        for (std::uint32_t index : {i0, i1, i2}) {
            tangents[index] += t;
            bitangents[index] += b;
        }
    }

    for (std::size_t v = 0; v < vcount; ++v) {
        const glm::vec3 n = prim.vertices[v].normal;
        glm::vec3 t = tangents[v] - n * glm::dot(n, tangents[v]);  // Gram-Schmidt
        if (glm::dot(t, t) < 1e-16f) {
            // Sommet sans UV exploitables : on prend une tangente arbitraire mais
            // orthogonale à la normale — le repère reste valide, la normal map sera
            // simplement orientée arbitrairement (mieux qu'un NaN ou un vecteur nul).
            const glm::vec3 axis =
                std::abs(n.x) > 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            t = axis - n * glm::dot(n, axis);
        }
        t = glm::normalize(t);
        const float w = glm::dot(glm::cross(n, t), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
        prim.vertices[v].tangent = glm::vec4(t, w);
    }
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
    ImageCache image_cache;
    MaterialCache material_cache;
    int generated_tangents = 0;

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
            const cgltf_accessor* tan = find_accessor(prim, cgltf_attribute_type_tangent, 0);

            PrimitiveData out_prim;
            const cgltf_size vcount = pos->count;
            out_prim.vertices.resize(vcount);
            for (cgltf_size i = 0; i < vcount; ++i) {
                render::MeshVertex& vertex = out_prim.vertices[i];
                cgltf_float tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
                // TANGENT est un vec4 en glTF : xyz + handedness.
                if (tan != nullptr) {
                    cgltf_accessor_read_float(tan, i, tmp, 4);
                    vertex.tangent = glm::vec4(tmp[0], tmp[1], tmp[2], tmp[3]);
                } else {
                    vertex.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);  // remplacé plus bas
                }
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

            // Tangentes absentes du fichier => on les dérive de la géométrie + des UV.
            if (tan == nullptr) {
                generate_tangents(out_prim);
                ++generated_tangents;
            }

            out_prim.material_index =
                resolve_material(prim.material, gltf_dir, out, image_cache, material_cache);
            out.primitives.push_back(std::move(out_prim));
        }
    }

    cgltf_free(data);

    if (!out.valid()) {
        log::warn("glTF : '{}' sans primitive triangulée exploitable", path);
        return false;
    }
    log::info("glTF : '{}' chargé — {} primitive(s), {} matériau(x), {} image(s), {} tangente(s) générée(s)",
              path, out.primitives.size(), out.materials.size(), out.images.size(),
              generated_tangents);
    return true;
}

}  // namespace noire::resource
