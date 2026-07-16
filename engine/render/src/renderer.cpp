#include "noire/render/renderer.hpp"

#include <glm/gtc/packing.hpp>  // packHalf1x16 (cubemap de secours en R16G16B16A16_SFLOAT)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "noire/core/log.hpp"
#include "hud_font.hpp"

// En-têtes SPIR-V embarqués (générés au build : voir cmake/Shaders.cmake).
#include "shaders/mesh.frag.spv.h"
#include "shaders/mesh.vert.spv.h"
#include "shaders/mesh_textured.frag.spv.h"
#include "shaders/foliage.frag.spv.h"
#include "shaders/hud.frag.spv.h"
#include "shaders/hud.vert.spv.h"
#include "shaders/mesh_instanced.vert.spv.h"
#include "shaders/mesh_textured.vert.spv.h"
#include "shaders/prefilter_env.comp.spv.h"
#include "shaders/shadow.vert.spv.h"
#include "shaders/shadow_foliage.frag.spv.h"
#include "shaders/shadow_instanced.vert.spv.h"
#include "shaders/terrain.frag.spv.h"
#include "shaders/wire.frag.spv.h"
#include "shaders/wire.vert.spv.h"
#include "shaders/skybox.frag.spv.h"
#include "shaders/skybox.vert.spv.h"

namespace noire::render {

namespace {
VkPrimitiveTopology to_vk_topology(Topology topology) {
    return topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// Miroir std140 du bloc GlobalUBO des shaders : c'est CE que voit le GPU. Superset
// de FrameUniforms — le Renderer y ajoute ce qu'il calcule lui-même (le cadrage des
// cascades d'ombre). Uniquement des vec4/mat4 => alignements std140 naturels, aucun
// padding manuel. Toute évolution doit être répercutée dans les 4 shaders.
// Miroir exact du bloc push du prefilter_env.comp.
struct PrefilterPush {
    float roughness;
    float src_resolution;
    std::uint32_t sample_count;
    std::uint32_t dst_size;
};

struct GpuFrameUniforms {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 fog_color_density;
    glm::vec4 params;
    glm::vec4 sun_direction;
    glm::vec4 sun_color;
    glm::mat4 light_view_proj[kShadowCascades];
    glm::vec4 cascade_splits;  // x,y = distance de fin de chaque cascade
    // Irradiance du ciel en SH9. vec4 et non vec3 : en std140 le stride d'un tableau est
    // de 16 octets de toute façon, et vec3[9] désaligne silencieusement (même piège que
    // cascade_splits, qui n'est pas un float[]). Seul .rgb porte l'information.
    glm::vec4 sh[9];
};
static_assert(kShadowCascades <= 4, "cascade_splits (vec4) ne porte que 4 distances");

// Une instance du HUD (M13) : UN glyphe, ou UNE plaque de fond (= un glyphe plein).
// Miroir exact des entrées de hud.vert. C'est un tampon de SOMMETS, pas un bloc
// uniforme : aucune règle std140 ne s'y applique, seul l'alignement des attributs
// compte (tous multiples de 4 octets, stride 40).
struct GlyphInstance {
    glm::vec2 position;  // px, coin haut-gauche
    glm::vec2 size;      // px
    glm::uvec2 bits;     // masque 5x7 : x = bits 0..31, y = bits 32..34
    glm::vec4 color;     // LINÉAIRE
};
static_assert(sizeof(GlyphInstance) == 40, "stride décrit à la main dans create_hud_pipeline");

// Push constants du pipeline texturé : miroir exact du bloc du même nom dans
// mesh_textured.vert/.frag. Le modèle est lu au vertex, les facteurs au fragment.
struct TexturedPushConstants {
    glm::mat4 model;               // offset 0
    glm::vec4 base_color_factor;   // offset 64
    glm::vec4 pbr_factors;         // offset 80 : x=metallic, y=roughness, z=normal_scale
};
static_assert(sizeof(TexturedPushConstants) == 96,
              "les push constants doivent tenir dans les 128 octets garantis par la spec");

// Push constants de l'ombre du FEUILLAGE. Le temps y passe plutôt que par le set 0 :
// ce set porte aussi les sampler2DShadow, or la passe est en train d'écrire dedans.
struct ShadowFoliagePushConstants {
    glm::mat4 light_mvp;           // offset 0  : lightViewProj * model
    glm::vec4 wind_params;         // offset 64 : x = temps (s)
    glm::vec4 base_color_factor;   // offset 80 : son .a entre dans le test alpha
};
static_assert(sizeof(ShadowFoliagePushConstants) == 96,
              "les push constants doivent tenir dans les 128 octets garantis par la spec");

// Format des shadow maps : profondeur pure 32 bits (pas de stencil).
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

// Depth bias slope-scaled : pousse les casters loin de la lumière au moment de
// l'écriture de profondeur. Le terme « slope » monte avec l'inclinaison de la face
// vis-à-vis du soleil, là où l'acné apparaît en premier (surfaces rasantes).
// Relevés le 2026-07-16 sur du gazon SANS caster (où « ombres on » doit égaler « ombres
// off » : tout écart y est de l'acné) : 1.25/1.75 laissait 0.77 % de pixels acnéiques,
// 4/6 n'en laisse que 0.03 %, sans décollement mesurable de l'ombre. Les anciennes
// valeurs n'étaient pas mauvaises — elles n'avaient JAMAIS été exercées, les cascades
// étant dégénérées depuis le reverse-z du M9 (cf. update_shadow_cascades).
constexpr float kDepthBiasConstant = 4.0f;
constexpr float kDepthBiasSlope = 6.0f;

// Marge (m) ajoutée devant chaque cascade le long de l'axe du soleil : capte les
// casters situés HORS de la tranche de frustum mais qui projettent dedans.
constexpr float kShadowCasterMargin = 60.0f;

// Répartition des cascades : 1 = purement logarithmique (précision au plus près),
// 0 = uniforme. 0.7 = compromis classique.
constexpr float kCascadeSplitLambda = 0.7f;
}  // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialize(const RendererCreateInfo& info) {
    get_framebuffer_size_ = info.get_framebuffer_size;

    if (!context_.initialize(info.context)) {
        return false;
    }

    // Système de téléversement GPU asynchrone (staging buffers + fences pollées).
    if (!transfer_.initialize(&context_)) {
        return false;
    }

    VkExtent2D extent = get_framebuffer_size_ ? get_framebuffer_size_() : VkExtent2D{1280, 720};
    if (extent.width == 0) extent.width = 1;
    if (extent.height == 0) extent.height = 1;
    if (!swapchain_.initialize(context_, extent)) {
        return false;
    }

    if (!create_render_pass() || !create_descriptor_set_layout() || !create_pipeline_layout() ||
        !create_pipelines() || !create_depth_resources() || !create_framebuffers() ||
        !create_command_objects() || !create_sync_objects()) {
        return false;
    }

    // Ombres du soleil (M8) : passe depth-only + cascades. Leur taille est fixe
    // (kShadowMapSize) => indépendantes de la swapchain, donc créées une seule fois et
    // jamais recréées au redimensionnement. AVANT les descriptor sets, qui référencent
    // les vues des cascades et le sampler comparatif (set 0, binding 1).
    if (!create_shadow_render_pass() || !create_shadow_resources() || !create_shadow_sampler() ||
        !create_shadow_pipeline_layout() || !create_shadow_pipelines()) {
        return false;
    }

    if (!create_uniform_buffers() || !create_descriptor_sets()) {
        return false;
    }

    // Environnement (étapes 6a/6b) : le layout du set 2 DOIT exister avant le pipeline
    // layout texturé, qui l'y référence. La cubemap elle-même arrive bien plus tard
    // (chargement asynchrone) — seule sa forme est figée ici.
    if (!create_env_sampler() || !create_env_descriptor_layout() || !create_env_descriptor_pool() ||
        !create_prefilter_descriptor_layout() || !create_prefilter_pipeline()) {
        return false;
    }

    // Textures / matériaux : sampler partagé, layout+pool du set 1 (3 textures PBR),
    // pipeline texturé, puis les secours 1x1 et le matériau par défaut.
    if (!create_sampler() || !create_material_descriptor_layout() ||
        !create_terrain_descriptor_layout() || !create_material_descriptor_pool() ||
        !create_textured_pipeline_layout() || !create_terrain_pipeline_layout()) {
        return false;
    }
    // L'ombre du FEUILLAGE ne peut être bâtie qu'ICI, et pas plus haut avec les autres
    // pipelines d'ombre : son layout référence material_set_layout_ (elle lit l'alpha de
    // la base color), qui vient tout juste d'être créé.
    if (!create_shadow_foliage_pipeline_layout()) {
        return false;
    }
    shadow_pipeline_foliage_ = build_shadow_foliage_pipeline();
    if (shadow_pipeline_foliage_ == VK_NULL_HANDLE) {
        return false;
    }

    pipeline_textured_ = build_textured_pipeline();
    pipeline_terrain_ = build_terrain_pipeline();
    pipeline_foliage_ = build_foliage_pipeline();
    pipeline_wire_ = build_wire_pipeline();
    if (pipeline_textured_ == VK_NULL_HANDLE || pipeline_terrain_ == VK_NULL_HANDLE ||
        pipeline_foliage_ == VK_NULL_HANDLE || pipeline_wire_ == VK_NULL_HANDLE) {
        return false;
    }
    if (!create_default_textures() || !create_default_material() || !create_default_environment()) {
        return false;
    }

    if (!create_skybox_pipeline_layout() || !create_skybox_pipeline()) {
        return false;
    }

    // HUD (M13). Ne dépend que de render_pass_ et descriptor_set_layout_ (set 0) : ni
    // matériau, ni texture, ni environnement. Sa police est embarquée en dur, donc il est
    // dessinable dès la toute première frame.
    if (!create_hud_pipeline_layout() || !create_hud_buffers() || !create_hud_pipeline()) {
        return false;
    }

    // Chronométrage GPU. timestampPeriod donne les nanosecondes par tick : il VARIE d'un
    // pilote à l'autre, une différence brute de ticks ne veut donc rien dire sans lui.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context_.physical_device(), &props);
    timestamp_period_ = props.limits.timestampPeriod;
    if (timestamp_period_ <= 0.0f) {
        // Le device ne sait pas horodater : on renonce, sans faire échouer le rendu.
        log::warn("Renderer : timestamps GPU indisponibles, profil désactivé");
    } else {
        VkQueryPoolCreateInfo qp{};
        qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = kFramesInFlight * 2;  // début + fin par frame en vol
        if (vkCreateQueryPool(context_.device(), &qp, nullptr, &timestamp_pool_) != VK_SUCCESS) {
            timestamp_pool_ = VK_NULL_HANDLE;
            log::warn("Renderer : création du query pool échouée, profil désactivé");
        }
    }

    log::info("Renderer 3D prêt : {} images, {}x{}", swapchain_.image_count(),
              swapchain_.extent().width, swapchain_.extent().height);
    return true;
}

MeshId Renderer::create_mesh(const std::vector<Vertex>& vertices, Topology topology) {
    Mesh mesh;
    mesh.vertex_count = static_cast<std::uint32_t>(vertices.size());
    mesh.topology = topology;

    const VkDeviceSize size = sizeof(Vertex) * vertices.size();
    if (!context_.create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, /*host_visible=*/true,
                                mesh.vertex_buffer)) {
        log::error("Renderer : échec de création d'un vertex buffer");
        return 0;
    }
    std::memcpy(mesh.vertex_buffer.mapped, vertices.data(), static_cast<std::size_t>(size));

    const MeshId id = next_mesh_id_++;
    meshes_.emplace(id, std::move(mesh));
    return id;
}

MeshId Renderer::create_mesh_indexed(const std::vector<MeshVertex>& vertices,
                                     const std::vector<std::uint32_t>& indices) {
    if (vertices.empty() || indices.empty()) {
        return 0;
    }

    Mesh mesh;
    mesh.vertex_count = static_cast<std::uint32_t>(vertices.size());
    mesh.index_count = static_cast<std::uint32_t>(indices.size());
    mesh.topology = Topology::Triangles;
    mesh.indexed = true;
    mesh.ready = false;  // dessinable seulement après complétion du transfert GPU.

    const VkDeviceSize vsize = sizeof(MeshVertex) * vertices.size();
    const VkDeviceSize isize = sizeof(std::uint32_t) * indices.size();

    // Tampons DESTINATION device-local (TRANSFER_DST + VERTEX/INDEX).
    if (!context_.create_buffer(vsize,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                /*host_visible=*/false, mesh.vertex_buffer)) {
        log::error("Renderer : échec d'allocation du vertex buffer device-local");
        return 0;
    }
    if (!context_.create_buffer(isize,
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                /*host_visible=*/false, mesh.index_buffer)) {
        log::error("Renderer : échec d'allocation de l'index buffer device-local");
        context_.destroy_buffer(mesh.vertex_buffer);
        return 0;
    }

    // Staging host-visible (SOURCE) : on y recopie les données CPU.
    GpuBuffer staging_v;
    GpuBuffer staging_i;
    if (!context_.create_buffer(vsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*host_visible=*/true,
                                staging_v) ||
        !context_.create_buffer(isize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*host_visible=*/true,
                                staging_i)) {
        log::error("Renderer : échec d'allocation d'un staging buffer");
        context_.destroy_buffer(staging_v);
        context_.destroy_buffer(mesh.index_buffer);
        context_.destroy_buffer(mesh.vertex_buffer);
        return 0;
    }
    std::memcpy(staging_v.mapped, vertices.data(), static_cast<std::size_t>(vsize));
    std::memcpy(staging_i.mapped, indices.data(), static_cast<std::size_t>(isize));

    // Enregistrement du transfert : copies staging -> device-local + barrière vers
    // l'étage d'assemblage des sommets. Tout est asynchrone (fence pollée).
    TransferManager::Transfer t = transfer_.begin();

    VkBufferCopy vcopy{};
    vcopy.size = vsize;
    vkCmdCopyBuffer(t.cmd, staging_v.buffer, mesh.vertex_buffer.buffer, 1, &vcopy);
    VkBufferCopy icopy{};
    icopy.size = isize;
    vkCmdCopyBuffer(t.cmd, staging_i.buffer, mesh.index_buffer.buffer, 1, &icopy);

    std::array<VkBufferMemoryBarrier, 2> barriers{};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer = mesh.vertex_buffer.buffer;
    barriers[0].offset = 0;
    barriers[0].size = VK_WHOLE_SIZE;
    barriers[1] = barriers[0];
    barriers[1].dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
    barriers[1].buffer = mesh.index_buffer.buffer;
    vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr,
                         static_cast<std::uint32_t>(barriers.size()), barriers.data(), 0, nullptr);

    // Les staging buffers seront libérés à la complétion (fence signalée).
    transfer_.stage(t, staging_v);
    transfer_.stage(t, staging_i);

    const MeshId id = next_mesh_id_++;
    // À la complétion : marque le maillage prêt à dessiner (sur le thread principal).
    transfer_.on_complete(t, [this, id] {
        const auto it = meshes_.find(id);
        if (it != meshes_.end()) {
            it->second.ready = true;
        }
    });
    transfer_.submit(t);

    meshes_.emplace(id, std::move(mesh));
    return id;
}

bool Renderer::is_mesh_ready(MeshId id) const {
    const auto it = meshes_.find(id);
    return it != meshes_.end() && it->second.ready;
}

void Renderer::destroy_mesh(MeshId id) {
    const auto it = meshes_.find(id);
    if (it == meshes_.end()) {
        return;
    }
    // Destruction différée : le tampon peut encore être référencé par une frame en vol.
    const std::uint64_t destroy_at = frame_index_ + kFramesInFlight + 1;
    pending_deletes_.push_back(PendingDelete{it->second.vertex_buffer, destroy_at});
    if (it->second.indexed) {
        pending_deletes_.push_back(PendingDelete{it->second.index_buffer, destroy_at});
    }
    meshes_.erase(it);
}

void Renderer::process_deferred_deletes() {
    for (auto it = pending_deletes_.begin(); it != pending_deletes_.end();) {
        if (frame_index_ >= it->destroy_at_frame) {
            context_.destroy_buffer(it->buffer);
            it = pending_deletes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pending_texture_deletes_.begin(); it != pending_texture_deletes_.end();) {
        if (frame_index_ >= it->destroy_at_frame) {
            free_texture(it->texture);
            it = pending_texture_deletes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pending_material_deletes_.begin(); it != pending_material_deletes_.end();) {
        if (frame_index_ >= it->destroy_at_frame) {
            vkFreeDescriptorSets(context_.device(), material_pool_, 1, &it->descriptor);
            it = pending_material_deletes_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Renderer::create_render_pass() {
    VkAttachmentDescription color{};
    color.format = swapchain_.image_format();
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = depth_format_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments{color, depth};
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    rp.pAttachments = attachments.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dependency;

    if (vkCreateRenderPass(context_.device(), &rp, nullptr, &render_pass_) != VK_SUCCESS) {
        log::error("Vulkan : création de la render pass échouée");
        return false;
    }
    return true;
}

bool Renderer::create_descriptor_set_layout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    // Lu par le vertex ET le fragment (météo : wetness + brouillard côté frag).
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Cascades d'ombre (M8 étape 2) : un tableau de kShadowCascades samplers
    // comparatifs. Le pipeline debug (mesh.frag) n'en déclare aucun — un shader n'est
    // pas tenu d'utiliser tous les bindings de son layout, et les deux pipelines
    // partagent ce set 0.
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = kShadowCascades;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(context_.device(), &layout_info, nullptr,
                                    &descriptor_set_layout_) != VK_SUCCESS) {
        log::error("Vulkan : création du descriptor set layout échouée");
        return false;
    }
    return true;
}

bool Renderer::create_pipeline_layout() {
    // Push constant : la matrice Model (relative caméra), par objet.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du pipeline layout échouée");
        return false;
    }
    return true;
}

VkShaderModule Renderer::create_shader_module(const unsigned char* code, std::size_t size) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = reinterpret_cast<const std::uint32_t*>(code);
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context_.device(), &ci, nullptr, &module) != VK_SUCCESS) {
        log::error("Vulkan : création d'un shader module échouée");
        return VK_NULL_HANDLE;
    }
    return module;
}

VkPipeline Renderer::build_pipeline(Topology topology) {
    VkShaderModule vert = create_shader_module(mesh_vert_spv, mesh_vert_spv_size);
    VkShaderModule frag = create_shader_module(mesh_frag_spv, mesh_frag_spv_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";

    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // Format de sommet : position (loc 0) + couleur (loc 1).
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = sizeof(glm::vec3);  // position (vec3) puis color (vec3), sans padding

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = to_vk_topology(topology);

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // pas de culling au M2 (grille + cubes simples)
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    // REVERSE-Z (M9) : le proche projette sur 1.0, le lointain sur 0.0 (cf. camera.cpp).
    depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &input_asm;
    pipe.pViewportState = &viewport_state;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &multisample;
    pipe.pDepthStencilState = &depth_stencil;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = pipeline_layout_;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe,
                                                nullptr, &pipeline);

    vkDestroyShaderModule(context_.device(), vert, nullptr);
    vkDestroyShaderModule(context_.device(), frag, nullptr);

    if (result != VK_SUCCESS) {
        log::error("Vulkan : création d'un pipeline graphique échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool Renderer::create_pipelines() {
    pipeline_triangles_ = build_pipeline(Topology::Triangles);
    pipeline_lines_ = build_pipeline(Topology::Lines);
    return pipeline_triangles_ != VK_NULL_HANDLE && pipeline_lines_ != VK_NULL_HANDLE;
}

bool Renderer::create_depth_resources() {
    const VkExtent2D extent = swapchain_.extent();
    if (!context_.create_image(extent.width, extent.height, depth_format_,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth_image_,
                               depth_allocation_)) {
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = depth_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = depth_format_;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(context_.device(), &view_info, nullptr, &depth_view_) != VK_SUCCESS) {
        log::error("Vulkan : création de la vue de profondeur échouée");
        return false;
    }
    return true;
}

void Renderer::destroy_depth_resources() {
    if (depth_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(context_.device(), depth_view_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
    }
    if (depth_image_ != VK_NULL_HANDLE) {
        context_.destroy_image(depth_image_, depth_allocation_);
        depth_image_ = VK_NULL_HANDLE;
        depth_allocation_ = nullptr;
    }
}

bool Renderer::create_framebuffers() {
    const auto& views = swapchain_.image_views();
    framebuffers_.resize(views.size());
    for (std::size_t i = 0; i < views.size(); ++i) {
        std::array<VkImageView, 2> attachments{views[i], depth_view_};
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = render_pass_;
        fb.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        fb.pAttachments = attachments.data();
        fb.width = swapchain_.extent().width;
        fb.height = swapchain_.extent().height;
        fb.layers = 1;
        if (vkCreateFramebuffer(context_.device(), &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            log::error("Vulkan : création d'un framebuffer échouée");
            return false;
        }
    }
    return true;
}

bool Renderer::create_command_objects() {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = context_.graphics_queue_family();
    if (vkCreateCommandPool(context_.device(), &pool, nullptr, &command_pool_) != VK_SUCCESS) {
        log::error("Vulkan : création du command pool échouée");
        return false;
    }

    command_buffers_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = static_cast<std::uint32_t>(command_buffers_.size());
    if (vkAllocateCommandBuffers(context_.device(), &alloc, command_buffers_.data()) != VK_SUCCESS) {
        log::error("Vulkan : allocation des command buffers échouée");
        return false;
    }
    return true;
}

bool Renderer::create_sync_objects() {
    image_available_.resize(kFramesInFlight);
    in_flight_.resize(kFramesInFlight);

    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < kFramesInFlight; ++i) {
        if (vkCreateSemaphore(context_.device(), &sem, nullptr, &image_available_[i]) != VK_SUCCESS ||
            vkCreateFence(context_.device(), &fence, nullptr, &in_flight_[i]) != VK_SUCCESS) {
            log::error("Vulkan : création des objets de synchro (frame) échouée");
            return false;
        }
    }
    return create_present_semaphores();
}

bool Renderer::create_present_semaphores() {
    for (VkSemaphore s : render_finished_) {
        if (s != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_.device(), s, nullptr);
        }
    }
    render_finished_.assign(swapchain_.image_count(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& s : render_finished_) {
        if (vkCreateSemaphore(context_.device(), &sem, nullptr, &s) != VK_SUCCESS) {
            log::error("Vulkan : création des sémaphores de présentation échouée");
            return false;
        }
    }
    return true;
}

bool Renderer::create_uniform_buffers() {
    uniform_buffers_.resize(kFramesInFlight);
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (!context_.create_buffer(sizeof(GpuFrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    /*host_visible=*/true, uniform_buffers_[static_cast<std::size_t>(i)])) {
            return false;
        }
    }
    return true;
}

bool Renderer::create_descriptor_sets() {
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = kFramesInFlight;
    // Les cascades d'ombre : kShadowCascades samplers par frame en vol.
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = kFramesInFlight * kShadowCascades;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.maxSets = kFramesInFlight;
    if (vkCreateDescriptorPool(context_.device(), &pool_info, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du descriptor pool échouée");
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(kFramesInFlight, descriptor_set_layout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pool_;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts.data();

    descriptor_sets_.resize(kFramesInFlight);
    if (vkAllocateDescriptorSets(context_.device(), &alloc, descriptor_sets_.data()) != VK_SUCCESS) {
        log::error("Vulkan : allocation des descriptor sets échouée");
        return false;
    }

    // Les shadow maps sont UNIQUES (pas une par frame en vol) : la render pass d'ombre
    // porte déjà la dépendance qui empêche la frame N d'écraser la carte pendant que la
    // frame N-1 la lit encore. Les descripteurs sont donc écrits une fois pour toutes.
    std::array<VkDescriptorImageInfo, kShadowCascades> shadow_infos{};
    for (std::size_t c = 0; c < kShadowCascades; ++c) {
        shadow_infos[c].sampler = shadow_sampler_;
        shadow_infos[c].imageView = shadow_cascades_[c].view;
        // Layout atteint à la fin de chaque passe d'ombre, donc avant tout
        // échantillonnage par la passe principale.
        shadow_infos[c].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = uniform_buffers_[static_cast<std::size_t>(i)].buffer;
        buffer_info.offset = 0;
        buffer_info.range = sizeof(GpuFrameUniforms);

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_sets_[static_cast<std::size_t>(i)];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &buffer_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptor_sets_[static_cast<std::size_t>(i)];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = static_cast<std::uint32_t>(shadow_infos.size());
        writes[1].pImageInfo = shadow_infos.data();

        vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    return true;
}

// --- Ombres du soleil (M8 étape 1) ------------------------------------------

bool Renderer::create_shadow_render_pass() {
    // Une seule pièce jointe : la profondeur, qu'on CONSERVE (storeOp STORE) puisque
    // la passe principale l'échantillonnera. Chaque frame repart d'un clear, donc
    // initialLayout = UNDEFINED (on ne relit jamais l'ancien contenu).
    VkAttachmentDescription depth{};
    depth.format = kShadowFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 0;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Passe depth-only : AUCUNE pièce jointe de couleur.
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depth_ref;

    // Deux dépendances externes encadrent l'usage de la carte :
    //   [0] l'écriture de profondeur attend la lecture frag de la frame précédente ;
    //   [1] la lecture frag (passe principale) attend la fin de l'écriture.
    // Pas de BY_REGION : l'échantillonnage d'une shadow map lit des texels arbitraires,
    // sans correspondance avec la région du fragment lecteur.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &depth;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = static_cast<std::uint32_t>(deps.size());
    rp.pDependencies = deps.data();

    if (vkCreateRenderPass(context_.device(), &rp, nullptr, &shadow_render_pass_) != VK_SUCCESS) {
        log::error("Vulkan : création de la render pass d'ombre échouée");
        return false;
    }
    return true;
}

bool Renderer::create_shadow_resources() {
    for (ShadowCascade& cascade : shadow_cascades_) {
        // SAMPLED dès maintenant : la passe principale échantillonnera ces cartes
        // (étape 2) sans qu'il faille recréer les images.
        if (!context_.create_image(kShadowMapSize, kShadowMapSize, kShadowFormat,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT,
                                   cascade.image, cascade.allocation)) {
            log::error("Vulkan : allocation d'une shadow map échouée");
            return false;
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = cascade.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = kShadowFormat;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(context_.device(), &view_info, nullptr, &cascade.view) != VK_SUCCESS) {
            log::error("Vulkan : création de la vue d'une shadow map échouée");
            return false;
        }

        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = shadow_render_pass_;
        fb.attachmentCount = 1;
        fb.pAttachments = &cascade.view;
        fb.width = kShadowMapSize;
        fb.height = kShadowMapSize;
        fb.layers = 1;
        if (vkCreateFramebuffer(context_.device(), &fb, nullptr, &cascade.framebuffer) !=
            VK_SUCCESS) {
            log::error("Vulkan : création du framebuffer d'une shadow map échouée");
            return false;
        }
    }
    return true;
}

bool Renderer::create_shadow_sampler() {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // compareEnable + filtrage LINEAR => le matériel compare la profondeur PUIS
    // interpole les 4 résultats booléens : un PCF 2x2 gratuit. Combiné à la grille
    // 3x3 du shader, on obtient un adoucissement 6x6 effectif.
    info.compareEnable = VK_TRUE;
    info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;  // pas de mipmaps sur une depth map
    // Hors de la cascade : bordure blanche (profondeur 1.0) => la comparaison passe
    // toujours => pas d'ombre. Sans ça, le mode REPEAT ferait réapparaître l'ombre
    // du train répétée à l'infini sur le sol.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    info.minLod = 0.0f;
    info.maxLod = 0.0f;
    info.anisotropyEnable = VK_FALSE;  // sans objet pour une comparaison de profondeur
    info.maxAnisotropy = 1.0f;
    if (vkCreateSampler(context_.device(), &info, nullptr, &shadow_sampler_) != VK_SUCCESS) {
        log::error("Vulkan : création du sampler comparatif d'ombre échouée");
        return false;
    }
    return true;
}

bool Renderer::create_shadow_pipeline_layout() {
    // Aucun descriptor set : la seule donnée nécessaire est lightViewProj * model,
    // poussée par objet. Pas d'UBO => la passe d'ombre ne dépend d'aucune frame en vol.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 0;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr,
                               &shadow_pipeline_layout_) != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline layout d'ombre échouée");
        return false;
    }
    return true;
}

VkPipeline Renderer::build_shadow_pipeline(std::uint32_t vertex_stride) {
    VkShaderModule vert = create_shader_module(shadow_vert_spv, shadow_vert_spv_size);
    if (vert == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    // Un seul étage : pas de fragment shader, seule la profondeur nous intéresse.
    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";

    // Seule la position est lue (offset 0 dans Vertex comme dans MeshVertex) ; le
    // reste du sommet est simplement ignoré, d'où le stride en paramètre.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = vertex_stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribute{};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 1;
    vertex_input.pVertexAttributeDescriptions = &attribute;

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // winding des modèles non garanti (cf. M7)
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = VK_TRUE;  // anti-acné, dès maintenant (cf. constantes)
    raster.depthBiasConstantFactor = kDepthBiasConstant;
    raster.depthBiasSlopeFactor = kDepthBiasSlope;
    raster.depthBiasClamp = 0.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Aucune pièce jointe de couleur dans le subpass => aucun attachment de blend.
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 0;

    std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = 1;
    pipe.pStages = &vs;
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &input_asm;
    pipe.pViewportState = &viewport_state;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &multisample;
    pipe.pDepthStencilState = &depth_stencil;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = shadow_pipeline_layout_;
    pipe.renderPass = shadow_render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);

    vkDestroyShaderModule(context_.device(), vert, nullptr);

    if (result != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline d'ombre échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool Renderer::create_shadow_foliage_pipeline_layout() {
    // Deux sets déclarés, un seul lié. Le set 0 (global) n'est PAS utilisé par ces
    // shaders — mais le matériau vit au set 1 partout dans ce moteur, et changer sa place
    // ici pour économiser une déclaration rendrait le shader trompeur. Vulkan n'exige une
    // liaison que pour les bindings réellement lus : le set 0 restera vide.
    const std::array<VkDescriptorSetLayout, 2> layouts{descriptor_set_layout_,
                                                       material_set_layout_};

    // La plage couvre les DEUX étages : le vertex lit light_mvp et wind_params, le
    // fragment lit base_color_factor.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ShadowFoliagePushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
    layout_info.pSetLayouts = layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr,
                               &shadow_foliage_pipeline_layout_) != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline layout d'ombre du feuillage échouée");
        return false;
    }
    return true;
}

VkPipeline Renderer::build_shadow_foliage_pipeline() {
    VkShaderModule vert =
        create_shader_module(shadow_instanced_vert_spv, shadow_instanced_vert_spv_size);
    VkShaderModule frag =
        create_shader_module(shadow_foliage_frag_spv, shadow_foliage_frag_spv_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";
    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // Mêmes deux bindings que le pipeline de feuillage de la vue caméra : le maillage par
    // sommet, les instances par ARBRE.
    std::array<VkVertexInputBindingDescription, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(MeshVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(InstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 6> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, tangent)};
    attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, position_scale)};
    attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, rotation_phase)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // une carte de feuillage projette des deux côtés
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;
    rs.depthBiasConstantFactor = kDepthBiasConstant;
    rs.depthBiasSlopeFactor = kDepthBiasSlope;
    rs.depthBiasClamp = 0.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    // La passe d'ombre est ORTHOGRAPHIQUE et garde la profondeur NORMALE : le reverse-z
    // du M9 ne concerne que la caméra (une ortho est déjà linéaire, il ne lui apporterait
    // rien). D'où LESS ici, et non GREATER.
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 0;  // depth-only : la render pass n'a aucune cible de couleur
    // Biais STATIQUE, comme le pipeline d'ombre ordinaire : le déclarer dynamique
    // obligerait à un vkCmdSetDepthBias, sans quoi il serait indéfini.
    const std::array<VkDynamicState, 2> dyn_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vi;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &rs;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &ds;
    pipe.pColorBlendState = &cb;
    pipe.pDynamicState = &dyn;
    pipe.layout = shadow_foliage_pipeline_layout_;
    pipe.renderPass = shadow_render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);
    vkDestroyShaderModule(context_.device(), frag, nullptr);
    vkDestroyShaderModule(context_.device(), vert, nullptr);
    if (res != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline d'ombre du feuillage échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool Renderer::create_shadow_pipelines() {
    shadow_pipeline_mesh_ = build_shadow_pipeline(sizeof(MeshVertex));
    shadow_pipeline_legacy_ = build_shadow_pipeline(sizeof(Vertex));
    return shadow_pipeline_mesh_ != VK_NULL_HANDLE && shadow_pipeline_legacy_ != VK_NULL_HANDLE;
}

void Renderer::update_shadow_cascades(const FrameUniforms& uniforms) {
    shadow_time_ = uniforms.params.y;  // horloge du vent, pour shadow_instanced.vert

    // Direction VERS le soleil. L'app la fournit ; on se protège d'un vecteur nul.
    glm::vec3 to_sun(uniforms.sun_direction);
    if (glm::dot(to_sun, to_sun) < 1e-6f) {
        to_sun = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    to_sun = glm::normalize(to_sun);

    // Plans near/far de la projection, SANS présumer de la convention de profondeur.
    //   z_ndc = -p22 - p32/z_view  =>  la distance où z_ndc vaut d est p32/(d + p22).
    // On évalue donc les deux plans du NDC (d=0 et d=1) et on les trie : le plan proche
    // est le plus proche, par définition. C'est ce qui rend ce cadrage indifférent au
    // REVERSE-Z — la version précédente codait en dur « d=0 est le near », convention que
    // le reverse-z du M9 a inversée, et elle rendait alors near et far INTERVERTIS
    // (mesuré : near=9999.9, far=0.1), d'où des cascades de 12 km et des texels de 12 m.
    const float p22 = uniforms.proj[2][2];
    const float p32 = uniforms.proj[3][2];
    const float depth0 = p32 / p22;           // distance où z_ndc = 0
    const float depth1 = p32 / (p22 + 1.0f);  // distance où z_ndc = 1
    const float near_plane = std::min(depth0, depth1);
    const float far_plane = std::max(depth0, depth1);
    // z NDC de CHAQUE plan : c'est ce qui permet de déprojeter les coins dans le bon ordre.
    const float ndc_near = depth1 < depth0 ? 1.0f : 0.0f;
    const float ndc_far = depth1 < depth0 ? 0.0f : 1.0f;
    const float shadow_far = std::min(kShadowDistance, far_plane);
    const float depth_range = far_plane - near_plane;

    // Coins du frustum caméra dans l'ESPACE FLOTTANT : la vue ne portant aucune
    // translation, la caméra est à l'origine et tout ce cadrage reste en float
    // proche de zéro — c'est exactement ce qui rend les ombres compatibles avec
    // l'origine flottante. NDC Vulkan : x,y ∈ [-1,1], z ∈ [0,1].
    const glm::mat4 inv_view_proj = glm::inverse(uniforms.proj * uniforms.view);
    std::array<glm::vec3, 8> corners{};  // [0..3] = plan proche, [4..7] = plan lointain
    std::size_t index = 0;
    for (const float z : {ndc_near, ndc_far}) {
        for (int y = -1; y <= 1; y += 2) {
            for (int x = -1; x <= 1; x += 2) {
                const glm::vec4 p = inv_view_proj * glm::vec4(static_cast<float>(x),
                                                              static_cast<float>(y), z, 1.0f);
                corners[index++] = glm::vec3(p) / p.w;
            }
        }
    }

    // Le soleil est directionnel : sa « vue » est une pure ROTATION (pas d'origine).
    // La garder indépendante du centre de la cascade est ce qui rend le snap au texel
    // possible plus bas.
    const glm::vec3 up =
        std::abs(to_sun.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 light_rotation = glm::lookAt(glm::vec3(0.0f), -to_sun, up);

    float slice_near = near_plane;
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        // Découpe pratique : mélange log/uniforme (lambda).
        const float ratio = static_cast<float>(i + 1) / static_cast<float>(kShadowCascades);
        const float log_split = near_plane * std::pow(shadow_far / near_plane, ratio);
        const float uniform_split = near_plane + (shadow_far - near_plane) * ratio;
        const float slice_far =
            kCascadeSplitLambda * log_split + (1.0f - kCascadeSplitLambda) * uniform_split;

        // Coins de la tranche : la position le long d'une arête du frustum est
        // LINÉAIRE en profondeur de vue, d'où une simple interpolation proche->lointain.
        const float t_near = (slice_near - near_plane) / depth_range;
        const float t_far = (slice_far - near_plane) / depth_range;
        std::array<glm::vec3, 8> slice{};
        for (std::size_t k = 0; k < 4; ++k) {
            const glm::vec3 edge = corners[k + 4] - corners[k];
            slice[k] = corners[k] + edge * t_near;
            slice[k + 4] = corners[k] + edge * t_far;
        }

        // Sphère englobante de la tranche : son rayon ne dépend QUE de la forme de la
        // tranche (fov/aspect/splits), pas de l'orientation caméra => volume d'ombre de
        // taille constante, donc pas de pompage de résolution quand on tourne la tête.
        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : slice) {
            center += corner;
        }
        center /= static_cast<float>(slice.size());
        float radius = 0.0f;
        for (const glm::vec3& corner : slice) {
            radius = std::max(radius, glm::length(corner - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;  // absorbe le bruit flottant

        // Snap du centre sur la grille de texels de la carte, en espace lumière :
        // sans ça, un déplacement sub-texel fait grouiller le bord des ombres.
        glm::vec3 center_light(light_rotation * glm::vec4(center, 1.0f));
        const float units_per_texel = (2.0f * radius) / static_cast<float>(kShadowMapSize);
        // Le snap doit ancrer la grille de texels sur le MONDE, pas sur la caméra. Or tout
        // ce cadrage vit en caméra-relatif, où `center_light` est CONSTANT à orientation
        // fixe : y appliquer floor() quantifiait une constante en une constante — un snap
        // purement décoratif, qui ne se déclenchait jamais (vérifié en le journalisant sur
        // un train en marche : valeur identique à 1e-6 près sur des centaines de frames).
        // On ajoute donc la position monde de la caméra pour snapper en absolu, puis on la
        // retire. En DOUBLE : elle vaut des centaines de milliers de mètres et on la
        // compare à un texel de quelques centimètres — un float n'a pas les chiffres pour.
        const glm::dmat4 light_rotation_d(light_rotation);
        const glm::dvec3 cam_light(light_rotation_d *
                                   glm::dvec4(uniforms.camera_world_position, 1.0));
        const auto snap_axis = [](double camera_rel, double cam, double unit) {
            return static_cast<float>(std::floor((camera_rel + cam) / unit) * unit - cam);
        };
        const double unit = static_cast<double>(units_per_texel);
        center_light.x = snap_axis(center_light.x, cam_light.x, unit);
        center_light.y = snap_axis(center_light.y, cam_light.y, unit);
        // Le z AUSSI. Il fixe z_near/z_far, donc tout l'encodage de profondeur : le
        // laisser glisser avec la caméra fait varier la profondeur stockée pour un même
        // point du monde d'une frame à l'autre — et l'acné se remet à clignoter même
        // avec x et y parfaitement ancrés.
        center_light.z = snap_axis(center_light.z, cam_light.z, unit);

        // Ortho cadrée sur la sphère. La lumière regarde vers -Z : la profondeur croît
        // quand z décroît, d'où l'inversion de signe. kShadowCasterMargin recule le plan
        // proche pour attraper les casters situés au-dessus du volume.
        const float z_near = -(center_light.z + radius) - kShadowCasterMargin;
        const float z_far = -(center_light.z - radius);
        const glm::mat4 light_projection =
            glm::ortho(center_light.x - radius, center_light.x + radius,
                       center_light.y - radius, center_light.y + radius, z_near, z_far);

        shadow_cascades_[i].light_view_proj = light_projection * light_rotation;
        shadow_cascades_[i].split_depth = slice_far;
        slice_near = slice_far;
    }
}

void Renderer::record_shadow_pass(VkCommandBuffer cmd, const std::vector<DrawItem>& items) {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    // Viewport/scissor dynamiques (comme les autres pipelines) : ici toujours la
    // taille fixe de la carte.
    VkViewport viewport{};
    viewport.width = static_cast<float>(kShadowMapSize);
    viewport.height = static_cast<float>(kShadowMapSize);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {kShadowMapSize, kShadowMapSize};

    for (const ShadowCascade& cascade : shadow_cascades_) {
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = shadow_render_pass_;
        rp.framebuffer = cascade.framebuffer;
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = {kShadowMapSize, kShadowMapSize};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        for (const DrawItem& item : items) {
            const auto mesh_it = meshes_.find(item.mesh);
            if (mesh_it == meshes_.end()) {
                continue;
            }
            const Mesh& mesh = mesh_it->second;
            if (!mesh.ready || mesh.topology == Topology::Lines) {
                continue;  // transfert en cours, ou géométrie filaire (ne porte pas d'ombre)
            }
            // Les CÂBLES ne portent pas d'ombre, et ne le peuvent pas : leur ruban est déplié
            // face à la CAMÉRA par le vertex shader, alors que cette passe regarde depuis le
            // SOLEIL. Ce pipeline les dessinerait à leur ligne médiane — deux sommets
            // confondus, donc des triangles dégénérés. L'ombre d'un fil de 15 mm est de
            // toute façon sous-pixel : on ne perd rien qu'on puisse voir.
            {
                const MaterialId mid = item.material != 0 ? item.material : default_material_;
                const auto mit = materials_.find(mid);
                if (mit != materials_.end() && mit->second.shading == Shading::Wire) {
                    continue;
                }
            }
            // model est déjà relatif à la caméra, et lightViewProj est cadrée dans ce
            // même espace : le produit reste petit et précis.
            const glm::mat4 light_mvp = cascade.light_view_proj * item.model;
            const bool instanced = item.instances != 0 && item.instance_count > 0;
            const auto inst_it =
                instanced ? instance_buffers_.find(item.instances) : instance_buffers_.end();
            if (instanced && inst_it == instance_buffers_.end()) {
                continue;  // tampon détruit entre-temps
            }
            VkDeviceSize offset = 0;

            if (instanced) {
                // Végétation : pipeline dédié (binding d'instances + test alpha). Son set 1
                // porte la base color, dont on ne lit QUE l'alpha — mais c'est ce qui fait
                // la différence entre l'ombre d'un arbre et celle d'un rectangle.
                const MaterialId material_id = item.material != 0 ? item.material : default_material_;
                const auto material_it = materials_.find(material_id);
                if (material_it == materials_.end() || !material_it->second.written) {
                    continue;  // set 1 pas encore écrit : ses textures arrivent
                }
                const Material& material = material_it->second;

                ShadowFoliagePushConstants push{};
                push.light_mvp = light_mvp;
                push.wind_params = glm::vec4(shadow_time_, 0.0f, 0.0f, 0.0f);
                push.base_color_factor = material.base_color_factor;

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_foliage_);
                vkCmdPushConstants(cmd, shadow_foliage_pipeline_layout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        shadow_foliage_pipeline_layout_, 1, 1,
                                        &material.descriptor, 0, nullptr);
                vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer.buffer, &offset);
                vkCmdBindVertexBuffers(cmd, 1, 1, &inst_it->second.buffer, &offset);
                vkCmdBindIndexBuffer(cmd, mesh.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh.index_count, item.instance_count, 0, 0, 0);
                continue;
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              mesh.indexed ? shadow_pipeline_mesh_ : shadow_pipeline_legacy_);
            vkCmdPushConstants(cmd, shadow_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &light_mvp);
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer.buffer, &offset);
            if (mesh.indexed) {
                vkCmdBindIndexBuffer(cmd, mesh.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, mesh.index_count, 1, 0, 0, 0);
            } else {
                vkCmdDraw(cmd, mesh.vertex_count, 1, 0, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
    }
}

void Renderer::destroy_shadow_resources() {
    VkDevice dev = context_.device();
    for (ShadowCascade& cascade : shadow_cascades_) {
        if (cascade.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, cascade.framebuffer, nullptr);
            cascade.framebuffer = VK_NULL_HANDLE;
        }
        if (cascade.view != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, cascade.view, nullptr);
            cascade.view = VK_NULL_HANDLE;
        }
        if (cascade.image != VK_NULL_HANDLE) {
            context_.destroy_image(cascade.image, cascade.allocation);
            cascade.image = VK_NULL_HANDLE;
            cascade.allocation = nullptr;
        }
    }
}

// --- Textures / matériaux (M7 étape 3) --------------------------------------

bool Renderer::create_sampler() {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.minLod = 0.0f;
    // Mips ACTIVÉS depuis le M9 : sans eux, une texture à haute fréquence (le gravier du
    // ballast, période 2 m) grouille dès quelques dizaines de mètres — chaque pixel
    // couvrant alors des dizaines de texels. VK_LOD_CLAMP_NONE = toute la chaîne, quelle
    // que soit sa longueur.
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (context_.sampler_anisotropy()) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(8.0f, context_.max_sampler_anisotropy());
    } else {
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
    }
    if (vkCreateSampler(context_.device(), &info, nullptr, &sampler_) != VK_SUCCESS) {
        log::error("Vulkan : création du sampler échouée");
        return false;
    }
    return true;
}

bool Renderer::create_material_descriptor_layout() {
    // set 1 = LE MATÉRIAU : les 3 textures PBR (glTF metallic-roughness).
    //   0 = base color (sRGB), 1 = metallic-roughness, 2 = normal map.
    std::array<VkDescriptorSetLayoutBinding, kMaterialTextures> bindings{};
    for (std::uint32_t i = 0; i < kMaterialTextures; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(context_.device(), &info, nullptr, &material_set_layout_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du descriptor set layout de matériau échouée");
        return false;
    }
    return true;
}

bool Renderer::create_terrain_descriptor_layout() {
    // set 1 du terrain : DEUX jeux PBR (herbe 0-2, craie 3-5). C'est la seule raison
    // d'être d'un pipeline distinct — le set 1 ordinaire n'a que 3 bindings, et on ne
    // peut pas lier 6 textures sur 3.
    std::array<VkDescriptorSetLayoutBinding, kTerrainTextures> bindings{};
    for (std::uint32_t i = 0; i < kTerrainTextures; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(context_.device(), &info, nullptr, &terrain_set_layout_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du set layout de terrain échouée");
        return false;
    }
    return true;
}

bool Renderer::create_terrain_pipeline_layout() {
    // Mêmes sets 0 et 2 et MÊMES push constants que le pipeline texturé : seul le set 1
    // change. C'est ce qui permet de réutiliser mesh_textured.vert tel quel.
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(TexturedPushConstants);
    const std::array<VkDescriptorSetLayout, 3> sets{descriptor_set_layout_, terrain_set_layout_,
                                                    env_set_layout_};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(context_.device(), &info, nullptr, &terrain_pipeline_layout_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du pipeline layout de terrain échouée");
        return false;
    }
    return true;
}

MaterialId Renderer::create_terrain_material(const TerrainMaterialDesc& desc) {
    Material material;
    material.textures = {desc.grass_base, desc.grass_metallic_rough, desc.grass_normal,
                         desc.chalk_base, desc.chalk_metallic_rough, desc.chalk_normal};
    material.texture_count = kTerrainTextures;
    material.shading = Shading::Terrain;
    // Le terrain ne pousse aucun facteur : ses deux jeux portent tout. Les push constants
    // restent envoyés (même layout) mais terrain.frag ne lit que `model`.
    material.base_color_factor = glm::vec4(1.0f);
    material.pbr_factors = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = material_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &terrain_set_layout_;
    if (vkAllocateDescriptorSets(context_.device(), &alloc, &material.descriptor) != VK_SUCCESS) {
        log::error("Renderer : allocation du set de terrain échouée");
        return 0;
    }
    const MaterialId id = next_material_id_++;
    materials_.emplace(id, material);
    return id;
}

bool Renderer::create_material_descriptor_pool() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // Dimensionné sur le PIRE cas (6 descripteurs/set, cf. terrain) : à 3, allouer un
    // seul matériau de terrain épuiserait le pool bien avant kMaxMaterials sets.
    pool_size.descriptorCount = kMaxMaterials * kTerrainTextures;

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;  // libération individuelle
    info.poolSizeCount = 1;
    info.pPoolSizes = &pool_size;
    info.maxSets = kMaxMaterials;
    if (vkCreateDescriptorPool(context_.device(), &info, nullptr, &material_pool_) != VK_SUCCESS) {
        log::error("Vulkan : création du descriptor pool de matériaux échouée");
        return false;
    }
    return true;
}

bool Renderer::create_textured_pipeline_layout() {
    // Push constants : la matrice Model (lue au vertex) + les facteurs du matériau
    // (lus au fragment). 96 octets => sous les 128 garantis par la spec Vulkan.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(TexturedPushConstants);

    // set 0 = UBO global + cascades (identique au pipeline debug => layouts compatibles
    // pour le set 0) ; set 1 = les 3 textures du matériau ; set 2 = la cubemap d'env
    // (M8 étape 6b), globale à la frame mais placée APRÈS le matériau pour ne pas
    // renuméroter les sets existants.
    std::array<VkDescriptorSetLayout, 3> sets{descriptor_set_layout_, material_set_layout_,
                                              env_set_layout_};

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(sets.size());
    layout_info.pSetLayouts = sets.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr,
                               &textured_pipeline_layout_) != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline layout texturé échouée");
        return false;
    }
    return true;
}

// Le pipeline terrain est le pipeline texturé à DEUX différences près : son fragment
// (terrain.frag, qui mélange deux jeux) et son layout (set 1 à 6 bindings). Le VERTEX
// est partagé — la géométrie du terrain est un MeshVertex comme les autres.
// Pipeline de végétation : mêmes sets et push constants que le texturé (donc même
// layout), mais un vertex dédié (binding 1 par INSTANCE + vent) et un fragment qui
// discard. On ne peut donc pas passer par build_surface_pipeline, dont l'entrée de
// sommets est figée.
VkPipeline Renderer::build_foliage_pipeline() {
    VkShaderModule vert = create_shader_module(mesh_instanced_vert_spv, mesh_instanced_vert_spv_size);
    VkShaderModule frag = create_shader_module(foliage_frag_spv, foliage_frag_spv_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";
    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // DEUX bindings : le maillage (par sommet) et les instances (par INSTANCE). C'est
    // tout le mécanisme — l'arbre est lu une fois, ses centaines de copies ne coûtent
    // qu'un jeu de 32 octets chacune.
    std::array<VkVertexInputBindingDescription, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(MeshVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(InstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 6> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, tangent)};
    attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, position_scale)};
    attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, rotation_phase)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // une carte de feuillage se voit des DEUX côtés
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER;  // REVERSE-Z
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Pas de blending : le discard suffit, et il ne demande AUCUN tri par profondeur —
    // c'est précisément pourquoi on le préfère à la transparence pour du feuillage.
    ba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const std::array<VkDynamicState, 2> dyn_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vi;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &rs;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &ds;
    pipe.pColorBlendState = &cb;
    pipe.pDynamicState = &dyn;
    pipe.layout = textured_pipeline_layout_;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);
    vkDestroyShaderModule(context_.device(), frag, nullptr);
    vkDestroyShaderModule(context_.device(), vert, nullptr);
    if (res != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline de végétation échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

InstanceBufferId Renderer::create_instances(const std::vector<InstanceData>& instances) {
    if (instances.empty()) {
        return 0;
    }
    const VkDeviceSize size = sizeof(InstanceData) * instances.size();
    GpuBuffer buffer;
    if (!context_.create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, /*host_visible=*/true,
                                buffer)) {
        log::error("Renderer : allocation d'un tampon d'instances échouée");
        return 0;
    }
    std::memcpy(buffer.mapped, instances.data(), static_cast<std::size_t>(size));
    const InstanceBufferId id = next_instance_id_++;
    instance_buffers_.emplace(id, buffer);
    return id;
}

void Renderer::destroy_instances(InstanceBufferId id) {
    const auto it = instance_buffers_.find(id);
    if (it == instance_buffers_.end()) {
        return;
    }
    // Même règle que les maillages : destruction DIFFÉRÉE, le tampon peut encore être
    // référencé par une frame en vol.
    pending_deletes_.push_back(PendingDelete{it->second, frame_index_ + kFramesInFlight + 1});
    instance_buffers_.erase(it);
}

// Pipeline des câbles (M12). Il ne peut pas passer par build_surface_pipeline : son vertex
// déplie un ruban face-caméra et son fragment sort une COUVERTURE en alpha, donc il lui faut
// un mélange — ce que la fabrique commune ne fait pas.
VkPipeline Renderer::build_wire_pipeline() {
    VkShaderModule vert = create_shader_module(wire_vert_spv, wire_vert_spv_size);
    VkShaderModule frag = create_shader_module(wire_frag_spv, wire_frag_spv_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";
    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // Entrée de sommets : un MeshVertex ORDINAIRE, réutilisé tel quel. uv.x y porte le côté
    // du ruban et uv.y le rayon vrai — pas de format de sommet supplémentaire à décrire,
    // pas de chemin de téléversement à dupliquer.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(MeshVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 4> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, tangent)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // le ruban se retourne selon l'angle de vue
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    // PAS d'écriture de profondeur : un fragment à 30 % de couverture n'a pas « occupé » sa
    // profondeur, et l'y inscrire ferait disparaître ce qui passe derrière. Le TEST, lui,
    // reste actif : un câble derrière un talus est bien rejeté.
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER;  // REVERSE-Z

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Mélange « over » classique : la couverture EST l'alpha.
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    const std::array<VkDynamicState, 2> dyn_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vi;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &rs;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &ds;
    pipe.pColorBlendState = &cb;
    pipe.pDynamicState = &dyn;
    pipe.layout = textured_pipeline_layout_;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);
    vkDestroyShaderModule(context_.device(), frag, nullptr);
    vkDestroyShaderModule(context_.device(), vert, nullptr);
    if (res != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline de câble échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// --- HUD (M13) ---------------------------------------------------------------------
//
// TOUT LE HUD TIENT EN UN vkCmdDraw. Un glyphe = une instance ; une plaque de fond = une
// instance (un glyphe dont les 35 bits sont à 1). Le quad est engendré depuis
// gl_VertexIndex, le masque de la police voyage DANS l'instance : donc aucune texture,
// aucun sampler, aucun descriptor set propre, et rien à attendre d'un téléversement
// asynchrone. ~100 glyphes de 15x21 px sans profondeur ni fetch => ~0,02 ms.

bool Renderer::create_hud_pipeline_layout() {
    // set 0 SEUL, et zéro push constant : le HUD n'a besoin que de la taille du viewport,
    // déjà publiée dans l'UBO global (params.zw). Le layout du set 0 porte aussi le
    // sampler des cascades d'ombre, que hud.frag ne consomme pas — c'est permis, et déjà
    // le cas de mesh.frag (cf. create_descriptor_set_layout).
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(context_.device(), &info, nullptr, &hud_pipeline_layout_) !=
        VK_SUCCESS) {
        log::error("Renderer : création du pipeline layout du HUD échouée");
        return false;
    }
    return true;
}

bool Renderer::create_hud_buffers() {
    hud_buffers_.resize(kFramesInFlight);
    const VkDeviceSize size = sizeof(GlyphInstance) * kMaxGlyphs;
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (!context_.create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    /*host_visible=*/true,
                                    hud_buffers_[static_cast<std::size_t>(i)])) {
            return false;
        }
    }
    return true;
}

bool Renderer::create_hud_pipeline() {
    VkShaderModule vert = create_shader_module(hud_vert_spv, hud_vert_spv_size);
    VkShaderModule frag = create_shader_module(hud_frag_spv, hud_frag_spv_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return false;
    }
    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";
    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // UNE SEULE binding, et elle est par INSTANCE : il n'y a aucun tampon de géométrie,
    // les 6 sommets du quad sortent de gl_VertexIndex. Un pipeline dont la seule entrée
    // est cadencée à l'instance est parfaitement légal.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(GlyphInstance);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 4> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GlyphInstance, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GlyphInstance, size)};
    // _UINT et non _SFLOAT : hud.vert lit un uvec2. Un décalage de type numérique entre
    // l'attribut et l'entrée du shader est une erreur de création de pipeline, pas une
    // conversion silencieuse.
    attrs[2] = {2, 0, VK_FORMAT_R32G32_UINT, offsetof(GlyphInstance, bits)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlyphInstance, color)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    // SEUL pipeline du moteur sans test de profondeur : le HUD est à l'écran, pas dans
    // le monde. La structure doit rester renseignée (sType compris) et fournie : le
    // subpass a un attachement de profondeur, un pDepthStencilState nul y serait une faute.
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Mélange « over » classique (même bloc que les câbles). Il a lieu en espace LINÉAIRE :
    // l'attachement est _SRGB, donc le matériel décode avant de mélanger et réencode après.
    // C'est le comportement CORRECT pour nos couleurs linéaires — ne pas le « corriger ».
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    const std::array<VkDynamicState, 2> dyn_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vi;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &rs;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &ds;
    pipe.pColorBlendState = &cb;
    pipe.pDynamicState = &dyn;
    pipe.layout = hud_pipeline_layout_;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    const VkResult res = vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe,
                                                   nullptr, &hud_pipeline_);
    vkDestroyShaderModule(context_.device(), frag, nullptr);
    vkDestroyShaderModule(context_.device(), vert, nullptr);
    if (res != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline du HUD échouée");
        hud_pipeline_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

std::uint32_t Renderer::upload_hud(const Hud& hud) {
    if (hud.rects.empty() && hud.texts.empty()) {
        return 0;
    }

    // On construit CÔTÉ CPU puis on écrit d'un seul memcpy : la mémoire mappée est
    // allouée en HOST_ACCESS_SEQUENTIAL_WRITE, donc potentiellement write-combined. La
    // relire (ou l'écrire dans le désordre) coûterait très cher.
    std::vector<GlyphInstance> instances;
    instances.reserve(hud.rects.size() + 64);

    auto pack_bits = [](std::uint64_t bits) {
        return glm::uvec2(static_cast<std::uint32_t>(bits & 0xffffffffu),
                          static_cast<std::uint32_t>(bits >> 32));
    };

    // Les plaques D'ABORD : un seul draw => l'ordre des instances est l'ordre de mélange.
    for (const HudRect& rect : hud.rects) {
        if (instances.size() >= kMaxGlyphs) break;
        GlyphInstance gi{};
        gi.position = glm::round(rect.position);
        gi.size = glm::round(rect.size);
        gi.bits = pack_bits(font::kGlyphBlock);
        gi.color = rect.color;
        instances.push_back(gi);
    }

    for (const TextDraw& text : hud.texts) {
        // ÉCHELLE ENTIÈRE : c'est ce qui rend le texte net. Le fragment retrouve son
        // texel par un floor() sur une grille 5x7 ; si un texel couvrait 2,5 px, il en
        // prendrait tantôt 2 tantôt 3 et les jambages seraient irréguliers.
        const auto px = static_cast<float>(std::max(1L, std::lround(text.scale)));
        const float advance = (font::kGlyphWidth + 1) * px;  // 1 texel de chasse
        const glm::vec2 origin = glm::round(text.position);
        float pen = 0.0f;
        for (const char c : text.text) {
            const std::uint64_t bits = font::glyph_bits(c);
            // Espace et caractères hors police : on avance sans émettre d'instance.
            if (bits != 0) {
                if (instances.size() >= kMaxGlyphs) break;
                GlyphInstance gi{};
                gi.position = origin + glm::vec2(pen, 0.0f);
                gi.size = glm::vec2(font::kGlyphWidth, font::kGlyphHeight) * px;
                gi.bits = pack_bits(bits);
                gi.color = text.color;
                instances.push_back(gi);
            }
            pen += advance;
        }
    }

    if (instances.empty()) {
        return 0;
    }
    if (instances.size() >= kMaxGlyphs) {
        log::warn("HUD : {} glyphes demandés, tronqué à {}", instances.size(), kMaxGlyphs);
    }

    // Même point, et même garantie, que le memcpy de l'UBO juste au-dessus : le
    // vkWaitForFences de CE slot est déjà passé, donc plus aucune frame en vol ne lit ce
    // tampon. Mémoire cohérente => aucun flush (le moteur entier en fait déjà le pari).
    std::memcpy(hud_buffers_[current_frame_].mapped, instances.data(),
                instances.size() * sizeof(GlyphInstance));
    return static_cast<std::uint32_t>(instances.size());
}

void Renderer::record_hud(VkCommandBuffer cmd, std::uint32_t glyph_count) {
    // Chemin NORMAL au chargement (HUD vide) : dessiner 0 instance reste une faute, les
    // bindings de sommets étant vérifiés quel qu'en soit le nombre.
    if (glyph_count == 0 || hud_pipeline_ == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hud_pipeline_);
    // CE BIND N'EST PAS FACULTATIF. hud_pipeline_layout_ n'a aucun push constant, alors
    // que textured_pipeline_layout_ en a 96 octets : des plages de push différentes
    // rendent les layouts INCOMPATIBLES, ce qui défait TOUS les sets déjà liés. Et on ne
    // peut pas non plus hériter du bind de la skybox : record_skybox retourne sans rien
    // faire quand aucun environnement n'est prêt.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hud_pipeline_layout_, 0, 1,
                            &descriptor_sets_[current_frame_], 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &hud_buffers_[current_frame_].buffer, &offset);
    vkCmdDraw(cmd, 6, glyph_count, 0, 0);
}

VkPipeline Renderer::build_terrain_pipeline() {
    return build_surface_pipeline(terrain_frag_spv, terrain_frag_spv_size,
                                  terrain_pipeline_layout_);
}

VkPipeline Renderer::build_textured_pipeline() {
    return build_surface_pipeline(mesh_textured_frag_spv, mesh_textured_frag_spv_size,
                                  textured_pipeline_layout_);
}

VkPipeline Renderer::build_surface_pipeline(const unsigned char* frag_spv, std::size_t frag_size,
                                            VkPipelineLayout layout) {
    VkShaderModule vert = create_shader_module(mesh_textured_vert_spv, mesh_textured_vert_spv_size);
    VkShaderModule frag = create_shader_module(frag_spv, frag_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";

    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // Format de sommet : position (0) + normale (1) + UV (2) + tangente (3).
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(MeshVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(MeshVertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(MeshVertex, normal);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(MeshVertex, uv);
    attributes[3].location = 3;
    attributes[3].binding = 0;
    attributes[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[3].offset = offsetof(MeshVertex, tangent);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // winding des modèles non garanti au M7 => pas de culling
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    // REVERSE-Z (M9) : le proche projette sur 1.0, le lointain sur 0.0 (cf. camera.cpp).
    depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &input_asm;
    pipe.pViewportState = &viewport_state;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &multisample;
    pipe.pDepthStencilState = &depth_stencil;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = layout;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result =
        vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);

    vkDestroyShaderModule(context_.device(), vert, nullptr);
    vkDestroyShaderModule(context_.device(), frag, nullptr);

    if (result != VK_SUCCESS) {
        log::error("Vulkan : création d'un pipeline de surface échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VkImageView Renderer::create_image_view_2d(VkImage image, VkFormat format, std::uint32_t mips) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = mips;
    view_info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &view_info, nullptr, &view) != VK_SUCCESS) {
        log::error("Vulkan : création d'une vue de texture échouée");
        return VK_NULL_HANDLE;
    }
    return view;
}

bool Renderer::create_default_textures() {
    // Une texture de secours 1x1 par rôle PBR : elles rendent tout matériau valide même
    // sans aucune texture, et laissent alors les facteurs seuls décider du rendu.
    const unsigned char white[4] = {255, 255, 255, 255};
    white_texture_ = create_texture(1, 1, white, TextureFormat::SrgbColor);

    // Convention glTF : G = roughness, B = metallic (R et A inutilisés). Neutre =
    // metallic 0 / roughness 1, donc le matériau retombe sur ses seuls facteurs.
    const unsigned char metal_rough[4] = {255, 255, 0, 255};
    default_mr_texture_ = create_texture(1, 1, metal_rough, TextureFormat::LinearData);

    // Normale plate : (0,0,1) encodé en 0..255 => la normale géométrique est conservée.
    const unsigned char flat_normal[4] = {128, 128, 255, 255};
    flat_normal_texture_ = create_texture(1, 1, flat_normal, TextureFormat::LinearData);

    if (white_texture_ == 0 || default_mr_texture_ == 0 || flat_normal_texture_ == 0) {
        log::error("Renderer : création des textures de secours 1x1 échouée");
        return false;
    }
    return true;
}

bool Renderer::create_default_material() {
    // Matériau par défaut : tout en secours. C'est ce que référence DrawItem::material
    // == 0 (ex. le sol avant que sa texture ne soit prête, ou un modèle sans matériau).
    default_material_ = create_material(MaterialDesc{});
    if (default_material_ == 0) {
        log::error("Renderer : création du matériau par défaut échouée");
        return false;
    }
    return true;
}

TextureId Renderer::create_texture(std::uint32_t width, std::uint32_t height,
                                   const void* rgba_pixels, TextureFormat format) {
    if (width == 0 || height == 0 || rgba_pixels == nullptr) {
        return 0;
    }

    Texture tex;
    // SRGB => décodage matériel vers le linéaire à l'échantillonnage (couleurs).
    // UNORM => octets bruts (metallic/roughness, normal map) : les décoder fausserait
    // complètement le PBR.
    const VkFormat vk_format = format == TextureFormat::SrgbColor ? VK_FORMAT_R8G8B8A8_SRGB
                                                                  : VK_FORMAT_R8G8B8A8_UNORM;
    // Mips (M9) : TRANSFER_SRC en plus de DST, car chaque niveau est la SOURCE du blit
    // qui engendre le suivant. Une 1x1 (secours par rôle) donne mips = 1, sans blit.
    tex.mips = mip_count(width, height);
    if (!context_.create_image(width, height, vk_format,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                               tex.image, tex.allocation, tex.mips)) {
        return 0;
    }
    tex.view = create_image_view_2d(tex.image, vk_format, tex.mips);
    if (tex.view == VK_NULL_HANDLE) {
        context_.destroy_image(tex.image, tex.allocation);
        return 0;
    }

    // Staging host-visible + téléversement asynchrone (copie + transitions de layout).
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
    GpuBuffer staging;
    if (!context_.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*host_visible=*/true,
                                staging)) {
        log::error("Renderer : échec d'allocation du staging buffer d'une texture");
        vkDestroyImageView(context_.device(), tex.view, nullptr);
        context_.destroy_image(tex.image, tex.allocation);
        return 0;
    }
    std::memcpy(staging.mapped, rgba_pixels, static_cast<std::size_t>(size));

    TransferManager::Transfer t = transfer_.begin();

    // 1) UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = tex.image;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = tex.mips;  // TOUS les niveaux, pas seulement le 0
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);

    // 2) Copie staging -> image
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1u};
    vkCmdCopyBufferToImage(t.cmd, staging.buffer, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // 3) Chaîne de mips par blits + transition finale de TOUS les niveaux vers
    //    SHADER_READ_ONLY. Une texture 1x1 (les secours par rôle) a mips = 1 : la boucle
    //    de blit ne tourne pas, seule la transition finale s'applique.
    record_mip_chain(t.cmd, tex.image, width, height, tex.mips, 1u,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    transfer_.stage(t, staging);
    const TextureId id = next_texture_id_++;
    transfer_.on_complete(t, [this, id] {
        const auto it = textures_.find(id);
        if (it != textures_.end()) {
            it->second.ready = true;
        }
    });
    transfer_.submit(t);

    textures_.emplace(id, tex);
    return id;
}

bool Renderer::is_texture_ready(TextureId id) const {
    const auto it = textures_.find(id);
    return it != textures_.end() && it->second.ready;
}

TextureId Renderer::resolve_texture(TextureId requested, TextureId fallback) const {
    // Une texture demandée mais inconnue (échec de chargement côté app) retombe sur le
    // secours du rôle : un matériau reste toujours dessinable.
    return (requested != 0 && textures_.count(requested) != 0) ? requested : fallback;
}

MaterialId Renderer::create_material(const MaterialDesc& desc) {
    Material material;
    material.textures = {desc.base_color, desc.metallic_roughness, desc.normal, 0, 0, 0};
    material.texture_count = kMaterialTextures;
    material.base_color_factor = desc.base_color_factor;
    material.shading = desc.shading;
    // .w porte l'« être du feuillage ». Le mettre dans le MATÉRIAU et non dans le pipeline
    // est ce qui permet à l'arbre et au poteau de partager le pipeline instancié.
    material.pbr_factors = glm::vec4(desc.metallic_factor, desc.roughness_factor,
                                     desc.normal_scale, desc.foliage ? 1.0f : 0.0f);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = material_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &material_set_layout_;
    if (vkAllocateDescriptorSets(context_.device(), &alloc, &material.descriptor) != VK_SUCCESS) {
        log::error("Renderer : allocation d'un descriptor set de matériau échouée (pool plein ?)");
        return 0;
    }

    // Le set n'est PAS écrit ici : ses textures sont peut-être encore en transfert.
    // update_pending_materials() s'en charge dès qu'elles sont toutes prêtes.
    const MaterialId id = next_material_id_++;
    materials_.emplace(id, material);
    return id;
}

void Renderer::update_pending_materials() {
    for (auto& [id, material] : materials_) {
        if (material.written) {
            continue;
        }
        // Secours PAR RÔLE : le terrain répète simplement le triplet (base, mr, normale)
        // pour son second jeu, donc le rôle se déduit de l'indice modulo 3.
        const TextureId fallbacks[3] = {white_texture_, default_mr_texture_, flat_normal_texture_};
        std::array<TextureId, kTerrainTextures> resolved{};
        bool all_ready = true;
        for (std::uint32_t i = 0; i < material.texture_count; ++i) {
            resolved[i] = resolve_texture(material.textures[i], fallbacks[i % 3]);
            // Tant qu'UNE seule n'est pas téléversée, on attend : écrire maintenant
            // lierait une image encore en TRANSFER_DST (donc pas échantillonnable).
            if (!is_texture_ready(resolved[i])) {
                all_ready = false;
                break;
            }
        }
        if (!all_ready) {
            continue;
        }

        std::array<VkDescriptorImageInfo, kTerrainTextures> infos{};
        std::array<VkWriteDescriptorSet, kTerrainTextures> writes{};
        for (std::uint32_t i = 0; i < material.texture_count; ++i) {
            infos[i].sampler = sampler_;
            infos[i].imageView = textures_.at(resolved[i]).view;
            infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = material.descriptor;
            writes[i].dstBinding = i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &infos[i];
        }
        // Sûr : le set n'a jamais pu être lié (les draws sautent les matériaux non
        // écrits), donc aucune frame en vol ne le référence.
        vkUpdateDescriptorSets(context_.device(), material.texture_count, writes.data(), 0, nullptr);
        material.written = true;
    }
}

bool Renderer::is_material_ready(MaterialId id) const {
    const auto it = materials_.find(id);
    return it != materials_.end() && it->second.written;
}

void Renderer::destroy_material(MaterialId id) {
    const auto it = materials_.find(id);
    if (it == materials_.end()) {
        return;
    }
    // Différé : le set peut encore être référencé par une frame en vol.
    pending_material_deletes_.push_back(
        PendingMaterialDelete{it->second.descriptor, frame_index_ + kFramesInFlight + 1});
    materials_.erase(it);
}

void Renderer::free_texture(Texture& texture) {
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(context_.device(), texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        context_.destroy_image(texture.image, texture.allocation);
        texture.image = VK_NULL_HANDLE;
        texture.allocation = nullptr;
    }
}

// =============================================================================
// Environnement / skybox (M8 étape 6a)
// =============================================================================

bool Renderer::create_env_sampler() {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    // Le premier sampler du moteur à réellement consommer des mips (le sampler des
    // matériaux est encore à maxLod = 0) : LINEAR entre niveaux, c'est ce qui donnera un
    // fondu continu en fonction de la rugosité à l'étape 6b.
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // CLAMP_TO_EDGE : sur une cubemap, Vulkan filtre nativement à travers les arêtes
    // (seamless), le mode d'adressage ne sert qu'à ne pas déborder.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.anisotropyEnable = VK_FALSE;  // inutile : on n'échantillonne jamais en rasant
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    info.compareEnable = VK_FALSE;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;  // toute la chaîne, quelle que soit sa longueur
    if (vkCreateSampler(context_.device(), &info, nullptr, &env_sampler_) != VK_SUCCESS) {
        log::error("Renderer : création du sampler d'environnement échouée");
        return false;
    }
    return true;
}

bool Renderer::create_env_descriptor_layout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(context_.device(), &info, nullptr, &env_set_layout_) !=
        VK_SUCCESS) {
        log::error("Renderer : création du layout de set d'environnement échouée");
        return false;
    }
    return true;
}

bool Renderer::create_env_descriptor_pool() {
    // Par environnement : 1 set skybox (cube source) + 1 set spéculaire (cube préfiltré)
    // + kSpecularMips sets de compute (source + 1 mip destination chacun).
    const std::uint32_t sets_per_env = 2u + kSpecularMips;
    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // 2 cubes échantillonnés + 1 source par set de compute.
    sizes[0].descriptorCount = kMaxEnvironments * (2u + kSpecularMips);
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  // 1 mip destination par set compute
    sizes[1].descriptorCount = kMaxEnvironments * kSpecularMips;

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = kMaxEnvironments * sets_per_env;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(context_.device(), &info, nullptr, &env_pool_) != VK_SUCCESS) {
        log::error("Renderer : création du pool de descripteurs d'environnement échouée");
        return false;
    }
    return true;
}

bool Renderer::create_prefilter_descriptor_layout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;  // cubemap source (le ciel net + sa chaîne de mips)
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;  // LE mip destination, vu en 2D_ARRAY
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(context_.device(), &info, nullptr, &prefilter_set_layout_) !=
        VK_SUCCESS) {
        log::error("Renderer : création du layout de set du préfiltrage échouée");
        return false;
    }
    return true;
}

bool Renderer::create_prefilter_pipeline() {
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(PrefilterPush);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &prefilter_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr,
                               &prefilter_pipeline_layout_) != VK_SUCCESS) {
        log::error("Renderer : création du pipeline layout du préfiltrage échouée");
        return false;
    }

    VkShaderModule module = create_shader_module(prefilter_env_comp_spv, prefilter_env_comp_spv_size);
    if (module == VK_NULL_HANDLE) {
        return false;
    }

    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = prefilter_pipeline_layout_;

    const VkResult res = vkCreateComputePipelines(context_.device(), VK_NULL_HANDLE, 1, &info,
                                                  nullptr, &prefilter_pipeline_);
    vkDestroyShaderModule(context_.device(), module, nullptr);
    if (res != VK_SUCCESS) {
        log::error("Renderer : création du pipeline compute de préfiltrage échouée");
        return false;
    }
    return true;
}

bool Renderer::create_skybox_pipeline_layout() {
    // set 0 = UBO global (le vertex y lit view/proj), set 1 = la cubemap. Aucun push
    // constant : le ciel n'a ni modèle ni matériau.
    const std::array<VkDescriptorSetLayout, 2> layouts{descriptor_set_layout_, env_set_layout_};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
    info.pSetLayouts = layouts.data();
    if (vkCreatePipelineLayout(context_.device(), &info, nullptr, &skybox_pipeline_layout_) !=
        VK_SUCCESS) {
        log::error("Renderer : création du pipeline layout de la skybox échouée");
        return false;
    }
    return true;
}

bool Renderer::create_skybox_pipeline() {
    VkShaderModule vert = create_shader_module(skybox_vert_spv, skybox_vert_spv_size);
    VkShaderModule frag = create_shader_module(skybox_frag_spv, skybox_frag_spv_size);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        return false;
    }

    VkPipelineShaderStageCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";

    VkPipelineShaderStageCreateInfo fs{};
    fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // AUCUN tampon de sommets : le triangle plein écran est déduit de gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    // Le ciel n'écrit PAS la profondeur : il est infiniment loin, rien ne doit être
    // testé contre lui. Le test, lui, reste actif en LESS_OR_EQUAL — le ciel sort à
    // exactement 1.0, donc il ne survit que là où la profondeur est encore au clear.
    depth_stencil.depthWriteEnable = VK_FALSE;
    // REVERSE-Z : le ciel sort à z = 0 (le plan lointain), il ne survit donc que là où
    // la profondeur vaut encore 0 — c'est-à-dire là où rien n'a été dessiné.
    depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_asm;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = skybox_pipeline_layout_;
    info.renderPass = render_pass_;
    info.subpass = 0;

    const VkResult res = vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info,
                                                   nullptr, &skybox_pipeline_);
    vkDestroyShaderModule(context_.device(), frag, nullptr);
    vkDestroyShaderModule(context_.device(), vert, nullptr);
    if (res != VK_SUCCESS) {
        log::error("Renderer : création du pipeline skybox échouée");
        return false;
    }
    return true;
}

std::uint32_t Renderer::mip_count(std::uint32_t width, std::uint32_t height) {
    const std::uint32_t largest = std::max(width, height);
    return static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(largest)))) + 1u;
}

void Renderer::record_mip_chain(VkCommandBuffer cmd, VkImage image, std::uint32_t width,
                                std::uint32_t height, std::uint32_t mips, std::uint32_t layers,
                                VkPipelineStageFlags dst_stage) {
    // À l'entrée : TOUS les niveaux sont en TRANSFER_DST et seul le mip 0 est rempli.
    // Chaque itération réduit le niveau précédent de moitié par un blit filtré
    // linéairement. Tous les layers sont traités d'un seul blit (cubemap : layers = 6).
    auto w = static_cast<std::int32_t>(width);
    auto h = static_cast<std::int32_t>(height);
    for (std::uint32_t level = 1; level < mips; ++level) {
        // Le niveau source vient d'être écrit : on l'amène en TRANSFER_SRC et on rend
        // cette écriture visible à la lecture du blit.
        VkImageMemoryBarrier to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1u, 0u, layers};
        to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &to_src);

        // Chaque dimension se divise INDÉPENDAMMENT et plancher à 1 : une texture non
        // carrée devient 1xN puis 1x1, elle ne « saute » pas de niveau.
        const std::int32_t nw = w > 1 ? w / 2 : 1;
        const std::int32_t nh = h > 1 ? h / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0u, layers};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, layers};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        w = nw;
        h = nh;
    }

    // Transition finale vers l'échantillonnage. Deux plages distinctes car elles ne sont
    // PAS dans le même layout : tous les niveaux sauf le dernier ont servi de source de
    // blit (TRANSFER_SRC), le dernier n'a été qu'une destination (TRANSFER_DST).
    std::array<VkImageMemoryBarrier, 2> finals{};
    std::uint32_t count = 0;
    if (mips > 1) {
        VkImageMemoryBarrier& src = finals[count++];
        src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        src.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src.image = image;
        src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, mips - 1u, 0u, layers};
        src.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        src.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    VkImageMemoryBarrier& last = finals[count++];
    last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    last.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    last.image = image;
    last.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mips - 1u, 1u, 0u, layers};
    last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    // `dst_stage` est fourni par l'appelant : une texture n'est lue qu'au FRAGMENT, mais
    // la cubemap d'environnement est aussi lue par le COMPUTE de préfiltrage, dans le
    // MÊME command buffer. Omettre cet étage laisserait le préfiltrage lire des mips pas
    // encore blités — un bug qui ne se verrait pas forcément sur cette machine.
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0, 0, nullptr, 0, nullptr,
                         count, finals.data());
}

bool Renderer::create_environment_specular(Environment& env, std::uint32_t face_size) {
    // Jamais plus grand que la source : pour la cubemap 1x1 de secours, spec_ fait 1x1.
    const std::uint32_t size = std::min(kSpecularFaceSize, face_size);
    const auto max_mips =
        static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(size)))) + 1u;
    env.spec_mips = std::min(kSpecularMips, max_mips);

    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // STORAGE pour l'écriture par le compute, SAMPLED pour la lecture par le PBR.
    // Pas de TRANSFER : rien ne se copie ici, tout est écrit par les dispatches.
    if (!context_.create_image(size, size, format,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               env.spec_image, env.spec_allocation, env.spec_mips, 6u,
                               VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
        log::error("Renderer : création de l'image spéculaire préfiltrée échouée");
        return false;
    }

    // Vue d'ÉCHANTILLONNAGE : cube, toute la chaîne (c'est elle que lit mesh_textured.frag).
    VkImageViewCreateInfo cube{};
    cube.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cube.image = env.spec_image;
    cube.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cube.format = format;
    cube.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, env.spec_mips, 0u, 6u};
    if (vkCreateImageView(context_.device(), &cube, nullptr, &env.spec_view) != VK_SUCCESS) {
        log::error("Renderer : création de la vue cube spéculaire échouée");
        return false;
    }

    // Vues d'ÉCRITURE : une par mip, en 2D_ARRAY (6 layers).
    env.spec_mip_views.resize(env.spec_mips, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < env.spec_mips; ++i) {
        VkImageViewCreateInfo mip{};
        mip.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        mip.image = env.spec_image;
        mip.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        mip.format = format;
        mip.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1u, 0u, 6u};
        if (vkCreateImageView(context_.device(), &mip, nullptr, &env.spec_mip_views[i]) !=
            VK_SUCCESS) {
            log::error("Renderer : création de la vue de mip spéculaire {} échouée", i);
            return false;
        }
    }

    // Set d'échantillonnage (set 2 du pipeline texturé) : même forme que celui de la
    // skybox, donc même layout réutilisé.
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = env_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &env_set_layout_;
    if (vkAllocateDescriptorSets(context_.device(), &alloc, &env.spec_descriptor) != VK_SUCCESS) {
        log::error("Renderer : allocation du set spéculaire échouée");
        return false;
    }
    VkDescriptorImageInfo spec_info{};
    spec_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    spec_info.imageView = env.spec_view;
    spec_info.sampler = env_sampler_;
    VkWriteDescriptorSet spec_write{};
    spec_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    spec_write.dstSet = env.spec_descriptor;
    spec_write.dstBinding = 0;
    spec_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    spec_write.descriptorCount = 1;
    spec_write.pImageInfo = &spec_info;
    vkUpdateDescriptorSets(context_.device(), 1, &spec_write, 0, nullptr);

    // Un set de compute par mip : source (cube du ciel) + destination (ce mip).
    env.prefilter_sets.resize(env.spec_mips, VK_NULL_HANDLE);
    const std::vector<VkDescriptorSetLayout> layouts(env.spec_mips, prefilter_set_layout_);
    VkDescriptorSetAllocateInfo comp_alloc{};
    comp_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    comp_alloc.descriptorPool = env_pool_;
    comp_alloc.descriptorSetCount = env.spec_mips;
    comp_alloc.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(context_.device(), &comp_alloc, env.prefilter_sets.data()) !=
        VK_SUCCESS) {
        log::error("Renderer : allocation des sets de préfiltrage échouée");
        return false;
    }
    for (std::uint32_t i = 0; i < env.spec_mips; ++i) {
        VkDescriptorImageInfo src{};
        // La source sera en SHADER_READ_ONLY au moment du dispatch (la barrière finale
        // des blits l'y amène) — c'est à l'ACCÈS que le layout doit être exact.
        src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src.imageView = env.view;
        src.sampler = env_sampler_;
        VkDescriptorImageInfo dst{};
        dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // obligatoire pour une storage image
        dst.imageView = env.spec_mip_views[i];

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = env.prefilter_sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &src;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = env.prefilter_sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &dst;
        vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    return true;
}

void Renderer::record_environment_prefilter(VkCommandBuffer cmd, const Environment& env,
                                            std::uint32_t face_size) {
    const std::uint32_t size = std::min(kSpecularFaceSize, face_size);

    // 1) spec_ : UNDEFINED -> GENERAL (tous mips, 6 layers). GENERAL est le seul layout
    //    admis pour une storage image écrite en compute.
    VkImageMemoryBarrier to_general{};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = env.spec_image;
    to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, env.spec_mips, 0u, 6u};
    to_general.srcAccessMask = 0;
    to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_general);

    // 2) Un dispatch par mip. AUCUNE barrière entre eux : chacun écrit une sous-ressource
    //    différente et ne lit que la source. Ils sont indépendants, le GPU peut les
    //    recouvrir librement.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_pipeline_);
    for (std::uint32_t i = 0; i < env.spec_mips; ++i) {
        const std::uint32_t mip_size = std::max(size >> i, 1u);

        PrefilterPush push{};
        // Chaque mip EST un niveau de rugosité : 0, 1/6, ... 1. C'est ce qui rend le
        // mapping rugosité -> lod exact côté PBR.
        push.roughness = env.spec_mips > 1 ? static_cast<float>(i) /
                                                 static_cast<float>(env.spec_mips - 1u)
                                           : 0.0f;
        push.src_resolution = static_cast<float>(face_size);
        // Le mip 0 (rugosité 0) sort au premier textureLod : son sampleCount est ignoré.
        // Les mips fins portent la plupart des texels, les grossiers ont besoin de plus
        // de samples pour couvrir un lobe large.
        push.sample_count = i <= 1u ? 64u : 128u;
        push.dst_size = mip_size;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_pipeline_layout_, 0,
                                1, &env.prefilter_sets[i], 0, nullptr);
        vkCmdPushConstants(cmd, prefilter_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);
        // Division ARRONDIE AU SUPÉRIEUR : sinon les mips < 8 (taille 4, 2, 1) donneraient
        // zéro groupe et ne seraient jamais écrits. Le shader borne en retour.
        const std::uint32_t groups = (mip_size + 7u) / 8u;
        vkCmdDispatch(cmd, groups, groups, 6u);
    }

    // 3) spec_ : GENERAL -> SHADER_READ_ONLY. C'est CETTE barrière qui garantit que les
    //    écritures du compute sont visibles au fragment shader du PBR.
    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = env.spec_image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, env.spec_mips, 0u, 6u};
    to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_read);
}

EnvironmentId Renderer::create_environment(std::uint32_t face_size, const void* faces_rgba16f) {
    if (face_size == 0 || faces_rgba16f == nullptr) {
        return 0;
    }

    Environment env;
    env.mips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(face_size)))) + 1u;

    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // TRANSFER_SRC en plus de TRANSFER_DST : chaque mip est la SOURCE du blit qui
    // engendre le suivant. L'oublier est l'erreur classique de ce chemin.
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!context_.create_image(face_size, face_size, format, usage, env.image, env.allocation,
                               env.mips, 6u, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
        log::error("Renderer : création de l'image d'environnement échouée");
        return 0;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = env.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;  // les 6 layers vus comme un cube
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, env.mips, 0u, 6u};
    if (vkCreateImageView(context_.device(), &view_info, nullptr, &env.view) != VK_SUCCESS) {
        log::error("Renderer : création de la vue cube d'environnement échouée");
        context_.destroy_image(env.image, env.allocation);
        return 0;
    }

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = env_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &env_set_layout_;
    if (vkAllocateDescriptorSets(context_.device(), &alloc, &env.descriptor) != VK_SUCCESS) {
        log::error("Renderer : allocation du set d'environnement échouée");
        vkDestroyImageView(context_.device(), env.view, nullptr);
        context_.destroy_image(env.image, env.allocation);
        return 0;
    }

    // Le set est écrit TOUT DE SUITE : contrairement à un matériau, il ne référence qu'une
    // vue, qui existe déjà. Le layout annoncé (SHADER_READ_ONLY) ne sera vrai qu'à la fin
    // du transfert, mais c'est au moment de l'ACCÈS qu'il doit l'être — et on ne dessine
    // la skybox que lorsque `ready`. Aucun risque de réécrire un set en vol.
    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = env.view;
    image_info.sampler = env_sampler_;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = env.descriptor;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);

    // Spéculaire préfiltré (étape 7) : image + vues + sets. Alloué AVANT le transfert,
    // car ses dispatches vivent dans le même command buffer.
    if (!create_environment_specular(env, face_size)) {
        destroy_environment_gpu(env);
        return 0;
    }

    // 6 faces contiguës, RGBA half => 8 octets par texel.
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(face_size) * face_size * 6u * 4u * sizeof(std::uint16_t);
    GpuBuffer staging;
    if (!context_.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*host_visible=*/true,
                                staging)) {
        log::error("Renderer : staging d'environnement ({} Mio) refusé",
                   size / (1024u * 1024u));
        vkFreeDescriptorSets(context_.device(), env_pool_, 1, &env.descriptor);
        vkDestroyImageView(context_.device(), env.view, nullptr);
        context_.destroy_image(env.image, env.allocation);
        return 0;
    }
    std::memcpy(staging.mapped, faces_rgba16f, static_cast<std::size_t>(size));

    const EnvironmentId id = next_environment_id_++;
    TransferManager::Transfer t = transfer_.begin();

    // 1) TOUS les niveaux et TOUS les layers : UNDEFINED -> TRANSFER_DST.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = env.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, env.mips, 0u, 6u};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);

    // 2) Une seule copie pour les 6 faces : elles sont contiguës dans le staging, dans
    //    l'ordre des layers (+X, -X, +Y, -Y, +Z, -Z).
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 6u};
    region.imageExtent = {face_size, face_size, 1u};
    vkCmdCopyBufferToImage(t.cmd, staging.buffer, env.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    // 3) Chaîne de mips + transition finale vers SHADER_READ_ONLY (visible au compute).
    // dst_stage inclut COMPUTE : le préfiltrage GGX lit cette chaîne juste après, dans
    // le même command buffer (cf. record_environment_prefilter).
    record_mip_chain(t.cmd, env.image, face_size, face_size, env.mips, 6u,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 4) Préfiltrage GGX, dans le MÊME command buffer : la famille de files du
    //    TransferManager est GRAPHICS|COMPUTE, donc les dispatches y sont légaux. Un seul
    //    submit, une seule fence — l'asynchronisme est celui, déjà éprouvé, des uploads,
    //    et le `ready` existant interdit tout usage avant la fin du préfiltrage.
    record_environment_prefilter(t.cmd, env, face_size);

    transfer_.stage(t, staging);
    transfer_.on_complete(t, [this, id] {
        const auto it = environments_.find(id);
        if (it != environments_.end()) {
            it->second.ready = true;
        }
    });
    transfer_.submit(t);

    environments_.emplace(id, env);
    log::info("Environnement : cubemap {}x{}, {} mips ({:.1f} Mio VRAM) — téléversement lancé",
              face_size, face_size, env.mips,
              static_cast<double>(size) * 4.0 / 3.0 / (1024.0 * 1024.0));
    return id;
}

bool Renderer::is_environment_ready(EnvironmentId id) const {
    const auto it = environments_.find(id);
    return it != environments_.end() && it->second.ready;
}

bool Renderer::create_default_environment() {
    // Cubemap 1x1 grise, dans le même esprit que les textures 1x1 par rôle : le set 2 du
    // pipeline texturé doit être liable même sans ciel chargé. Sans elle, il faudrait
    // sauter tous les draws texturés en l'absence de HDR.
    // Valeur en half : 0.05 ~ un gris sombre neutre. Les SH restant à zéro dans ce cas,
    // le rendu est volontairement terne — c'est un secours, pas un éclairage.
    const std::uint16_t grey = glm::packHalf1x16(0.05f);
    const std::uint16_t one = glm::packHalf1x16(1.0f);
    std::array<std::uint16_t, 6u * 4u> faces{};
    for (std::size_t i = 0; i < 6u; ++i) {
        faces[i * 4u + 0] = grey;
        faces[i * 4u + 1] = grey;
        faces[i * 4u + 2] = grey;
        faces[i * 4u + 3] = one;
    }
    default_environment_ = create_environment(1u, faces.data());
    if (default_environment_ == 0) {
        log::error("Renderer : création de la cubemap d'environnement de secours échouée");
        return false;
    }
    return true;
}

VkDescriptorSet Renderer::resolve_environment_set() const {
    // Rend le set SPÉCULAIRE (préfiltré GGX) : c'est lui que lit mesh_textured.frag, pas
    // le ciel brut. L'actif d'abord, le secours ensuite. `ready` est indispensable : lier
    // une image encore en GENERAL déclencherait une erreur de layout au draw.
    for (const EnvironmentId id : {active_environment_, default_environment_}) {
        if (id == 0) {
            continue;
        }
        const auto it = environments_.find(id);
        if (it != environments_.end() && it->second.ready) {
            return it->second.spec_descriptor;
        }
    }
    return VK_NULL_HANDLE;
}

void Renderer::destroy_environment_gpu(Environment& env) {
    VkDevice dev = context_.device();
    // Sets : seulement si le pool existe encore (au shutdown il part en premier et
    // emporte tous ses sets, cf. la boucle de shutdown qui les met à VK_NULL_HANDLE).
    if (env_pool_ != VK_NULL_HANDLE) {
        if (env.descriptor != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(dev, env_pool_, 1, &env.descriptor);
        }
        if (env.spec_descriptor != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(dev, env_pool_, 1, &env.spec_descriptor);
        }
        if (!env.prefilter_sets.empty()) {
            vkFreeDescriptorSets(dev, env_pool_,
                                 static_cast<std::uint32_t>(env.prefilter_sets.size()),
                                 env.prefilter_sets.data());
        }
    }
    env.descriptor = VK_NULL_HANDLE;
    env.spec_descriptor = VK_NULL_HANDLE;
    env.prefilter_sets.clear();

    for (VkImageView view : env.spec_mip_views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, view, nullptr);
        }
    }
    env.spec_mip_views.clear();
    if (env.spec_view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, env.spec_view, nullptr);
        env.spec_view = VK_NULL_HANDLE;
    }
    if (env.spec_image != VK_NULL_HANDLE) {
        context_.destroy_image(env.spec_image, env.spec_allocation);
        env.spec_image = VK_NULL_HANDLE;
        env.spec_allocation = nullptr;
    }
    if (env.view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, env.view, nullptr);
        env.view = VK_NULL_HANDLE;
    }
    if (env.image != VK_NULL_HANDLE) {
        context_.destroy_image(env.image, env.allocation);
        env.image = VK_NULL_HANDLE;
        env.allocation = nullptr;
    }
}

void Renderer::destroy_environment(EnvironmentId id) {
    const auto it = environments_.find(id);
    if (it == environments_.end()) {
        return;
    }
    // Un environnement se détruit rarement (changement de ciel). On paie donc une
    // synchro franche plutôt que d'ajouter une file de destruction différée de plus.
    vkDeviceWaitIdle(context_.device());
    if (active_environment_ == id) {
        active_environment_ = 0;
    }
    destroy_environment_gpu(it->second);
    environments_.erase(it);
}

void Renderer::record_skybox(VkCommandBuffer cmd) {
    if (active_environment_ == 0) {
        return;
    }
    const auto it = environments_.find(active_environment_);
    if (it == environments_.end() || !it->second.ready) {
        return;  // pas encore téléversée : le clear tient lieu de fond
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline_layout_, 0, 1,
                            &descriptor_sets_[current_frame_], 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline_layout_, 1, 1,
                            &it->second.descriptor, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // triangle plein écran, sans tampon de sommets
}

void Renderer::destroy_texture(TextureId id) {
    const auto it = textures_.find(id);
    if (it == textures_.end()) {
        return;
    }
    // Destruction différée (l'image/descriptor peut être référencé par une frame en vol).
    pending_texture_deletes_.push_back(
        PendingTextureDelete{it->second, frame_index_ + kFramesInFlight + 1});
    textures_.erase(it);
}

void Renderer::record_commands(VkCommandBuffer cmd, std::uint32_t image_index,
                               const std::vector<DrawItem>& items, std::uint32_t glyph_count) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    // Le reset doit être commandé sur la file, PAS depuis le CPU : les deux requêtes du
    // slot ont été relues juste avant, et leur ancienne valeur doit disparaître avant
    // d'être réécrite — sinon vkGetQueryPoolResults rendrait un résultat périmé.
    const std::uint32_t query_base = current_frame_ * 2;
    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, timestamp_pool_, query_base, 2);
        // TOP_OF_PIPE : dès que la commande entre dans la file. Le pendant BOTTOM_OF_PIPE
        // n'est écrit qu'une fois TOUT le travail précédent retiré — l'intervalle couvre
        // donc bien l'exécution complète, ombres comprises.
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_pool_,
                            query_base + 0);
        timestamp_written_[current_frame_] = true;
    }

    // Ombres d'abord : les cartes doivent être remplies avant que la passe principale
    // ne les échantillonne (la synchro est portée par les dépendances de la render
    // pass d'ombre). Le cadrage des cascades a été calculé dans draw_frame.
    record_shadow_pass(cmd, items);

    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{background_color_.r, background_color_.g, background_color_.b, 1.0f}};
    clears[1].depthStencil = {0.0f, 0};  // REVERSE-Z : 0 = plan lointain

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain_.extent();
    rp.clearValueCount = static_cast<std::uint32_t>(clears.size());
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchain_.extent().width);
    viewport.height = static_cast<float>(swapchain_.extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain_.extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkDescriptorSet frame_ubo = descriptor_sets_[current_frame_];

    for (const DrawItem& item : items) {
        const auto mesh_it = meshes_.find(item.mesh);
        if (mesh_it == meshes_.end()) {
            continue;
        }
        const Mesh& mesh = mesh_it->second;
        if (!mesh.ready) {
            continue;  // transfert GPU asynchrone pas encore terminé (chemin M7).
        }

        VkDeviceSize offset = 0;
        if (mesh.indexed) {
            // Modèle texturé : pipeline dédié + set 0 (UBO) + set 1 (matériau).
            const MaterialId material_id = item.material != 0 ? item.material : default_material_;
            const auto material_it = materials_.find(material_id);
            if (material_it == materials_.end() || !material_it->second.written) {
                continue;  // matériau inconnu, ou ses textures pas encore téléversées.
            }
            const Material& material = material_it->second;

            TexturedPushConstants push{};
            push.model = item.model;
            push.base_color_factor = material.base_color_factor;
            push.pbr_factors = material.pbr_factors;

            // set 2 = la cubemap d'environnement (IBL). Résolue à chaque item mais
            // constante sur la frame ; sans elle (même pas le secours téléversé, ce qui
            // ne dure qu'une frame ou deux au démarrage) on ne peut pas dessiner.
            const VkDescriptorSet env_set = resolve_environment_set();
            if (env_set == VK_NULL_HANDLE) {
                continue;
            }
            // Le terrain a son propre pipeline et son propre layout (set 1 à 6 bindings) ;
            // tout le reste — vertex, sets 0 et 2, push constants — est commun.
            const bool instanced = item.instances != 0 && item.instance_count > 0;
            const auto inst_it = instanced ? instance_buffers_.find(item.instances)
                                           : instance_buffers_.end();
            if (instanced && inst_it == instance_buffers_.end()) {
                continue;  // tampon détruit entre-temps
            }
            const bool terrain = material.shading == Shading::Terrain;
            const VkPipeline pipeline =
                material.shading == Shading::Wire ? pipeline_wire_
                : terrain                         ? pipeline_terrain_
                : instanced                       ? pipeline_foliage_
                                                  : pipeline_textured_;
            const VkPipelineLayout layout =
                terrain ? terrain_pipeline_layout_ : textured_pipeline_layout_;
            const std::array<VkDescriptorSet, 3> sets{frame_ubo, material.descriptor, env_set};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                                    static_cast<std::uint32_t>(sets.size()), sets.data(), 0,
                                    nullptr);
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer.buffer, &offset);
            if (instanced) {
                vkCmdBindVertexBuffers(cmd, 1, 1, &inst_it->second.buffer, &offset);
            }
            vkCmdBindIndexBuffer(cmd, mesh.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.index_count, instanced ? item.instance_count : 1, 0, 0, 0);
        } else {
            // Géométrie debug (grille, rails, cubes de secours) : couleur par sommet.
            VkPipeline pipeline =
                mesh.topology == Topology::Lines ? pipeline_lines_ : pipeline_triangles_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                    &frame_ubo, 0, nullptr);
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &item.model);
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer.buffer, &offset);
            vkCmdDraw(cmd, mesh.vertex_count, 1, 0, 0);
        }
    }

    // Le ciel EN DERNIER, volontairement : toute la géométrie a déjà écrit sa profondeur,
    // donc l'early-z rejette le ciel partout où elle est passée. Le dessiner en premier
    // coûterait un fragment plein écran systématiquement.
    record_skybox(cmd);

    // Le HUD APRÈS le ciel, donc en tout dernier : il est à l'écran, sans profondeur, et
    // doit couvrir la scène entière. Toujours dans la MÊME passe : aucun attachement à
    // retransitionner, aucune render pass de plus.
    record_hud(cmd, glyph_count);

    vkCmdEndRenderPass(cmd);

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool_,
                            query_base + 1);
    }
    vkEndCommandBuffer(cmd);
}

void Renderer::draw_frame(const FrameUniforms& uniforms, const std::vector<DrawItem>& items,
                          const Hud& hud) {
    VkDevice dev = context_.device();

    // Compteur de frames monotone + traitement des destructions GPU différées.
    ++frame_index_;
    process_deferred_deletes();

    // Récupère les téléversements asynchrones terminés (fences pollées, non bloquant) :
    // libère les staging buffers et marque les maillages prêts à dessiner.
    transfer_.poll();
    // Puis écrit le set 1 des matériaux dont les textures viennent d'arriver.
    update_pending_materials();

    vkWaitForFences(dev, 1, &in_flight_[current_frame_], VK_TRUE, UINT64_MAX);

    // Le fence vient de garantir que la précédente utilisation de CE slot est terminée :
    // ses deux timestamps sont donc disponibles, et on les lit SANS attendre. Les
    // premières frames n'ont encore rien écrit -> VK_NOT_READY, qu'on ignore.
    if (timestamp_pool_ != VK_NULL_HANDLE && timestamp_written_[current_frame_]) {
        std::array<std::uint64_t, 2> ticks{};
        const VkResult qres = vkGetQueryPoolResults(
            dev, timestamp_pool_, current_frame_ * 2, 2, sizeof(ticks), ticks.data(),
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (qres == VK_SUCCESS && ticks[1] > ticks[0]) {
            last_gpu_ms_ = static_cast<float>(ticks[1] - ticks[0]) * timestamp_period_ * 1e-6f;
        }
    }

    std::uint32_t image_index = 0;
    VkResult acquire = vkAcquireNextImageKHR(dev, swapchain_.handle(), UINT64_MAX,
                                             image_available_[current_frame_], VK_NULL_HANDLE,
                                             &image_index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        log::error("Vulkan : acquisition d'image échouée");
        return;
    }

    // Le fond suit la couleur du brouillard (cohérence visuelle par temps couvert).
    background_color_ = glm::vec3(uniforms.fog_color_density);

    // Cadrage des cascades d'ombre pour cette frame (dépend de la caméra + du soleil).
    update_shadow_cascades(uniforms);

    // Mise à jour de l'UBO de CETTE frame (mapping persistant) : entrées de l'app
    // + ce que le Renderer a calculé lui-même (matrices et portées des cascades).
    GpuFrameUniforms gpu{};
    gpu.view = uniforms.view;
    gpu.proj = uniforms.proj;
    gpu.fog_color_density = uniforms.fog_color_density;
    gpu.params = uniforms.params;
    // z,w = taille du viewport en pixels. Renseignée ICI et pas par l'app : le Renderer est
    // le seul à connaître l'étendue réelle de la swapchain, et elle change au redimensionnement.
    // wire.vert en a besoin pour convertir une largeur en MÈTRES en une largeur en PIXELS.
    gpu.params.z = static_cast<float>(swapchain_.extent().width);
    gpu.params.w = static_cast<float>(swapchain_.extent().height);
    gpu.sun_direction = uniforms.sun_direction;
    gpu.sun_color = uniforms.sun_color;
    for (std::size_t i = 0; i < uniforms.sh.size(); ++i) {
        gpu.sh[i] = uniforms.sh[i];
    }
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        gpu.light_view_proj[i] = shadow_cascades_[i].light_view_proj;
        gpu.cascade_splits[static_cast<glm::length_t>(i)] = shadow_cascades_[i].split_depth;
    }
    std::memcpy(uniform_buffers_[current_frame_].mapped, &gpu, sizeof(GpuFrameUniforms));

    // Glyphes du HUD : même slot, même fence, donc même garantie que l'UBO ci-dessus.
    const std::uint32_t glyph_count = upload_hud(hud);

    vkResetFences(dev, 1, &in_flight_[current_frame_]);

    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);
    record_commands(cmd, image_index, items, glyph_count);

    VkSemaphore wait_semaphores[] = {image_available_[current_frame_]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal_semaphores[] = {render_finished_[image_index]};

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = wait_semaphores;
    submit.pWaitDstStageMask = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = signal_semaphores;
    if (vkQueueSubmit(context_.graphics_queue(), 1, &submit, in_flight_[current_frame_]) !=
        VK_SUCCESS) {
        log::error("Vulkan : soumission à la file graphique échouée");
        return;
    }

    VkSwapchainKHR chains[] = {swapchain_.handle()};
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = signal_semaphores;
    present.swapchainCount = 1;
    present.pSwapchains = chains;
    present.pImageIndices = &image_index;

    VkResult present_result = vkQueuePresentKHR(context_.present_queue(), &present);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR ||
        framebuffer_resized_) {
        framebuffer_resized_ = false;
        recreate_swapchain();
    } else if (present_result != VK_SUCCESS) {
        log::error("Vulkan : présentation échouée");
    }

    current_frame_ = (current_frame_ + 1) % kFramesInFlight;
}

void Renderer::recreate_swapchain() {
    VkExtent2D extent = get_framebuffer_size_ ? get_framebuffer_size_() : swapchain_.extent();
    if (extent.width == 0 || extent.height == 0) {
        return;  // minimisé : on saute (l'app attend les événements)
    }

    vkDeviceWaitIdle(context_.device());

    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(context_.device(), fb, nullptr);
    }
    framebuffers_.clear();
    destroy_depth_resources();

    swapchain_.recreate(extent);
    create_present_semaphores();  // le nombre d'images peut changer
    create_depth_resources();
    create_framebuffers();
}

void Renderer::wait_idle() {
    if (context_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context_.device());
    }
}

void Renderer::shutdown() {
    VkDevice dev = context_.device();
    if (dev == VK_NULL_HANDLE) {
        return;  // déjà détruit (idempotent)
    }
    vkDeviceWaitIdle(dev);

    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(dev, timestamp_pool_, nullptr);
        timestamp_pool_ = VK_NULL_HANDLE;
    }

    for (auto& [id, mesh] : meshes_) {
        context_.destroy_buffer(mesh.vertex_buffer);
        if (mesh.indexed) {
            context_.destroy_buffer(mesh.index_buffer);
        }
    }
    meshes_.clear();

    // Termine les téléversements encore en vol et libère leurs ressources (staging,
    // fences, pool). meshes_ est déjà vidé : les callbacks de complétion sont no-op.
    transfer_.shutdown();
    for (PendingDelete& pending : pending_deletes_) {
        context_.destroy_buffer(pending.buffer);
    }
    pending_deletes_.clear();

    // Textures / matériaux : les descriptor sets des matériaux partent avec leur pool
    // (pas besoin de les libérer un par un), puis vues + images des textures.
    materials_.clear();
    pending_material_deletes_.clear();
    for (auto& [id, tex] : textures_) {
        free_texture(tex);
    }
    textures_.clear();
    for (PendingTextureDelete& pending : pending_texture_deletes_) {
        free_texture(pending.texture);
    }
    pending_texture_deletes_.clear();
    if (pipeline_textured_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_textured_, nullptr);
        pipeline_textured_ = VK_NULL_HANDLE;
    }
    if (pipeline_terrain_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_terrain_, nullptr);
        pipeline_terrain_ = VK_NULL_HANDLE;
    }
    if (pipeline_foliage_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_foliage_, nullptr);
        pipeline_foliage_ = VK_NULL_HANDLE;
    }
    if (pipeline_wire_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_wire_, nullptr);
        pipeline_wire_ = VK_NULL_HANDLE;
    }
    // HUD (M13) : pipeline avant son layout, comme partout ailleurs.
    if (hud_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, hud_pipeline_, nullptr);
        hud_pipeline_ = VK_NULL_HANDLE;
    }
    if (hud_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, hud_pipeline_layout_, nullptr);
        hud_pipeline_layout_ = VK_NULL_HANDLE;
    }
    for (GpuBuffer& buffer : hud_buffers_) {
        context_.destroy_buffer(buffer);
    }
    hud_buffers_.clear();
    for (auto& [id, buffer] : instance_buffers_) {
        context_.destroy_buffer(buffer);
    }
    instance_buffers_.clear();
    if (terrain_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, terrain_pipeline_layout_, nullptr);
        terrain_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (terrain_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, terrain_set_layout_, nullptr);
        terrain_set_layout_ = VK_NULL_HANDLE;
    }
    if (textured_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, textured_pipeline_layout_, nullptr);
        textured_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (material_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, material_pool_, nullptr);  // libère aussi les sets
        material_pool_ = VK_NULL_HANDLE;
    }
    if (material_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, material_set_layout_, nullptr);
        material_set_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }

    // Environnement / skybox (étape 6a). Les sets partent avec le pool : on ne libère
    // ici que vues + images (destroy_environment_gpu les remettrait au pool inutilement).
    // Le pool emporte tous ses sets : on le met à zéro le temps de la boucle pour que
    // destroy_environment_gpu ne cherche pas à les rendre un par un, mais on garde son
    // handle pour le détruire réellement plus bas.
    VkDescriptorPool env_pool_pending = env_pool_;
    env_pool_ = VK_NULL_HANDLE;
    for (auto& [id, env] : environments_) {
        destroy_environment_gpu(env);
    }
    env_pool_ = env_pool_pending;
    environments_.clear();
    active_environment_ = 0;
    default_environment_ = 0;
    if (prefilter_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, prefilter_pipeline_, nullptr);
        prefilter_pipeline_ = VK_NULL_HANDLE;
    }
    if (prefilter_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, prefilter_pipeline_layout_, nullptr);
        prefilter_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (prefilter_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, prefilter_set_layout_, nullptr);
        prefilter_set_layout_ = VK_NULL_HANDLE;
    }
    if (skybox_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, skybox_pipeline_, nullptr);
        skybox_pipeline_ = VK_NULL_HANDLE;
    }
    if (skybox_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, skybox_pipeline_layout_, nullptr);
        skybox_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (env_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, env_pool_, nullptr);  // libère aussi les sets
        env_pool_ = VK_NULL_HANDLE;
    }
    if (env_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, env_set_layout_, nullptr);
        env_set_layout_ = VK_NULL_HANDLE;
    }
    if (env_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, env_sampler_, nullptr);
        env_sampler_ = VK_NULL_HANDLE;
    }

    // Ombres (M8) : framebuffers/vues/images, puis sampler, pipelines, layout, passe.
    destroy_shadow_resources();
    if (shadow_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, shadow_sampler_, nullptr);
        shadow_sampler_ = VK_NULL_HANDLE;
    }
    if (shadow_pipeline_foliage_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, shadow_pipeline_foliage_, nullptr);
        shadow_pipeline_foliage_ = VK_NULL_HANDLE;
    }
    if (shadow_foliage_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, shadow_foliage_pipeline_layout_, nullptr);
        shadow_foliage_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (shadow_pipeline_mesh_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, shadow_pipeline_mesh_, nullptr);
        shadow_pipeline_mesh_ = VK_NULL_HANDLE;
    }
    if (shadow_pipeline_legacy_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, shadow_pipeline_legacy_, nullptr);
        shadow_pipeline_legacy_ = VK_NULL_HANDLE;
    }
    if (shadow_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, shadow_pipeline_layout_, nullptr);
        shadow_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (shadow_render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, shadow_render_pass_, nullptr);
        shadow_render_pass_ = VK_NULL_HANDLE;
    }

    for (GpuBuffer& ubo : uniform_buffers_) {
        context_.destroy_buffer(ubo);
    }
    uniform_buffers_.clear();

    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);  // libère aussi les sets
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }

    for (VkSemaphore s : render_finished_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
    }
    render_finished_.clear();
    for (VkSemaphore s : image_available_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(dev, s, nullptr);
    }
    image_available_.clear();
    for (VkFence f : in_flight_) {
        if (f != VK_NULL_HANDLE) vkDestroyFence(dev, f, nullptr);
    }
    in_flight_.clear();

    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    for (VkFramebuffer fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(dev, fb, nullptr);
    }
    framebuffers_.clear();
    destroy_depth_resources();

    if (pipeline_triangles_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_triangles_, nullptr);
        pipeline_triangles_ = VK_NULL_HANDLE;
    }
    if (pipeline_lines_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_lines_, nullptr);
        pipeline_lines_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    swapchain_.shutdown();
    context_.shutdown();
}

}  // namespace noire::render
