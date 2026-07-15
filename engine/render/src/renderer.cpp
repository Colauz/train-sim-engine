#include "noire/render/renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "noire/core/log.hpp"

// En-têtes SPIR-V embarqués (générés au build : voir cmake/Shaders.cmake).
#include "shaders/mesh.frag.spv.h"
#include "shaders/mesh.vert.spv.h"
#include "shaders/mesh_textured.frag.spv.h"
#include "shaders/mesh_textured.vert.spv.h"
#include "shaders/shadow.vert.spv.h"

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
struct GpuFrameUniforms {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 fog_color_density;
    glm::vec4 params;
    glm::vec4 sun_direction;
    glm::vec4 sun_color;
    glm::mat4 light_view_proj[kShadowCascades];
    glm::vec4 cascade_splits;  // x,y = distance de fin de chaque cascade
};
static_assert(kShadowCascades <= 4, "cascade_splits (vec4) ne porte que 4 distances");

// Format des shadow maps : profondeur pure 32 bits (pas de stencil).
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

// Depth bias slope-scaled : pousse les casters loin de la lumière au moment de
// l'écriture de profondeur. Le terme « slope » monte avec l'inclinaison de la face
// vis-à-vis du soleil, là où l'acné apparaît en premier (surfaces rasantes).
constexpr float kDepthBiasConstant = 1.25f;
constexpr float kDepthBiasSlope = 1.75f;

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

    // Textures / matériaux (M7 étape 3) : sampler partagé, layout+pool set=1, pipeline
    // texturé, puis la texture de secours blanche 1x1.
    if (!create_sampler() || !create_texture_descriptor_layout() ||
        !create_texture_descriptor_pool() || !create_textured_pipeline_layout()) {
        return false;
    }
    pipeline_textured_ = build_textured_pipeline();
    if (pipeline_textured_ == VK_NULL_HANDLE) {
        return false;
    }
    if (!create_default_texture()) {
        return false;
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
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

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

bool Renderer::create_shadow_pipelines() {
    shadow_pipeline_mesh_ = build_shadow_pipeline(sizeof(MeshVertex));
    shadow_pipeline_legacy_ = build_shadow_pipeline(sizeof(Vertex));
    return shadow_pipeline_mesh_ != VK_NULL_HANDLE && shadow_pipeline_legacy_ != VK_NULL_HANDLE;
}

void Renderer::update_shadow_cascades(const FrameUniforms& uniforms) {
    // Direction VERS le soleil. L'app la fournit ; on se protège d'un vecteur nul.
    glm::vec3 to_sun(uniforms.sun_direction);
    if (glm::dot(to_sun, to_sun) < 1e-6f) {
        to_sun = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    to_sun = glm::normalize(to_sun);

    // near/far de la projection perspective (profondeur 0..1) :
    //   proj[2][2] = f/(n-f) et proj[3][2] = f*n/(n-f)
    //   => n = proj[3][2] / proj[2][2] et f = proj[3][2] / (proj[2][2] + 1)
    // (l'inversion de Y côté Vulkan ne touche pas ces deux termes)
    const float p22 = uniforms.proj[2][2];
    const float p32 = uniforms.proj[3][2];
    const float near_plane = p32 / p22;
    const float far_plane = p32 / (p22 + 1.0f);
    const float shadow_far = std::min(kShadowDistance, far_plane);
    const float depth_range = far_plane - near_plane;

    // Coins du frustum caméra dans l'ESPACE FLOTTANT : la vue ne portant aucune
    // translation, la caméra est à l'origine et tout ce cadrage reste en float
    // proche de zéro — c'est exactement ce qui rend les ombres compatibles avec
    // l'origine flottante. NDC Vulkan : x,y ∈ [-1,1], z ∈ [0,1].
    const glm::mat4 inv_view_proj = glm::inverse(uniforms.proj * uniforms.view);
    std::array<glm::vec3, 8> corners{};  // [0..3] = plan proche, [4..7] = plan lointain
    std::size_t index = 0;
    for (int z = 0; z <= 1; ++z) {
        for (int y = -1; y <= 1; y += 2) {
            for (int x = -1; x <= 1; x += 2) {
                const glm::vec4 p =
                    inv_view_proj * glm::vec4(static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(z), 1.0f);
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
        center_light.x = std::floor(center_light.x / units_per_texel) * units_per_texel;
        center_light.y = std::floor(center_light.y / units_per_texel) * units_per_texel;

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

            // model est déjà relatif à la caméra, et lightViewProj est cadrée dans ce
            // même espace : le produit reste petit et précis.
            const glm::mat4 light_mvp = cascade.light_view_proj * item.model;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              mesh.indexed ? shadow_pipeline_mesh_ : shadow_pipeline_legacy_);
            vkCmdPushConstants(cmd, shadow_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &light_mvp);
            VkDeviceSize offset = 0;
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
    info.maxLod = 0.0f;  // pas de mipmaps pour l'instant (M8+)
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

bool Renderer::create_texture_descriptor_layout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(context_.device(), &info, nullptr, &texture_set_layout_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du descriptor set layout de texture échouée");
        return false;
    }
    return true;
}

bool Renderer::create_texture_descriptor_pool() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kMaxTextures;

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;  // libération individuelle
    info.poolSizeCount = 1;
    info.pPoolSizes = &pool_size;
    info.maxSets = kMaxTextures;
    if (vkCreateDescriptorPool(context_.device(), &info, nullptr, &texture_pool_) != VK_SUCCESS) {
        log::error("Vulkan : création du descriptor pool de textures échouée");
        return false;
    }
    return true;
}

bool Renderer::create_textured_pipeline_layout() {
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    // set 0 = UBO global (identique au pipeline debug => layouts compatibles pour
    // le set 0) ; set 1 = combined image sampler du matériau.
    std::array<VkDescriptorSetLayout, 2> sets{descriptor_set_layout_, texture_set_layout_};

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

VkPipeline Renderer::build_textured_pipeline() {
    VkShaderModule vert = create_shader_module(mesh_textured_vert_spv, mesh_textured_vert_spv_size);
    VkShaderModule frag = create_shader_module(mesh_textured_frag_spv, mesh_textured_frag_spv_size);
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

    // Format de sommet : position (loc 0) + normale (loc 1) + UV (loc 2).
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(MeshVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributes{};
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
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

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
    pipe.layout = textured_pipeline_layout_;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result =
        vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);

    vkDestroyShaderModule(context_.device(), vert, nullptr);
    vkDestroyShaderModule(context_.device(), frag, nullptr);

    if (result != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline texturé échouée");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VkImageView Renderer::create_image_view_2d(VkImage image, VkFormat format) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &view_info, nullptr, &view) != VK_SUCCESS) {
        log::error("Vulkan : création d'une vue de texture échouée");
        return VK_NULL_HANDLE;
    }
    return view;
}

bool Renderer::create_default_texture() {
    const unsigned char white[4] = {255, 255, 255, 255};
    white_texture_ = create_texture(1, 1, white);
    if (white_texture_ == 0) {
        log::error("Renderer : création de la texture de secours (blanche 1x1) échouée");
        return false;
    }
    return true;
}

TextureId Renderer::create_texture(std::uint32_t width, std::uint32_t height,
                                   const void* rgba_pixels) {
    if (width == 0 || height == 0 || rgba_pixels == nullptr) {
        return 0;
    }

    Texture tex;
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;  // couleur de base => espace sRGB
    if (!context_.create_image(width, height, format,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               tex.image, tex.allocation)) {
        return 0;
    }
    tex.view = create_image_view_2d(tex.image, format);
    if (tex.view == VK_NULL_HANDLE) {
        context_.destroy_image(tex.image, tex.allocation);
        return 0;
    }

    // Descriptor set=1 (combined image sampler), écrit tout de suite (le layout
    // SHADER_READ_ONLY sera atteint par le transfert avant tout échantillonnage).
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = texture_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &texture_set_layout_;
    if (vkAllocateDescriptorSets(context_.device(), &alloc, &tex.descriptor) != VK_SUCCESS) {
        log::error("Renderer : allocation d'un descriptor set de texture échouée (pool plein ?)");
        vkDestroyImageView(context_.device(), tex.view, nullptr);
        context_.destroy_image(tex.image, tex.allocation);
        return 0;
    }

    VkDescriptorImageInfo image_info{};
    image_info.sampler = sampler_;
    image_info.imageView = tex.view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.descriptor;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);

    // Staging host-visible + téléversement asynchrone (copie + transitions de layout).
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
    GpuBuffer staging;
    if (!context_.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*host_visible=*/true,
                                staging)) {
        log::error("Renderer : échec d'allocation du staging buffer d'une texture");
        vkFreeDescriptorSets(context_.device(), texture_pool_, 1, &tex.descriptor);
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
    to_dst.subresourceRange.levelCount = 1;
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

    // 3) TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (échantillonnage frag)
    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_read);

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

const Renderer::Texture* Renderer::resolve_texture(TextureId id) const {
    if (id != 0) {
        const auto it = textures_.find(id);
        if (it != textures_.end() && it->second.ready) {
            return &it->second;
        }
    }
    const auto fallback = textures_.find(white_texture_);
    if (fallback != textures_.end() && fallback->second.ready) {
        return &fallback->second;
    }
    return nullptr;
}

void Renderer::free_texture(Texture& texture) {
    if (texture.descriptor != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(context_.device(), texture_pool_, 1, &texture.descriptor);
        texture.descriptor = VK_NULL_HANDLE;
    }
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
                               const std::vector<DrawItem>& items) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    // Ombres d'abord : les cartes doivent être remplies avant que la passe principale
    // ne les échantillonne (la synchro est portée par les dépendances de la render
    // pass d'ombre). Le cadrage des cascades a été calculé dans draw_frame.
    record_shadow_pass(cmd, items);

    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{background_color_.r, background_color_.g, background_color_.b, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

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
            const Texture* tex = resolve_texture(item.texture);
            if (tex == nullptr) {
                continue;  // même la texture de secours n'est pas encore prête.
            }
            const std::array<VkDescriptorSet, 2> sets{frame_ubo, tex->descriptor};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_textured_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textured_pipeline_layout_,
                                    0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0,
                                    nullptr);
            vkCmdPushConstants(cmd, textured_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &item.model);
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer.buffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.index_count, 1, 0, 0, 0);
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

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void Renderer::draw_frame(const FrameUniforms& uniforms, const std::vector<DrawItem>& items) {
    VkDevice dev = context_.device();

    // Compteur de frames monotone + traitement des destructions GPU différées.
    ++frame_index_;
    process_deferred_deletes();

    // Récupère les téléversements asynchrones terminés (fences pollées, non bloquant) :
    // libère les staging buffers et marque les maillages prêts à dessiner.
    transfer_.poll();

    vkWaitForFences(dev, 1, &in_flight_[current_frame_], VK_TRUE, UINT64_MAX);

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
    gpu.sun_direction = uniforms.sun_direction;
    gpu.sun_color = uniforms.sun_color;
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        gpu.light_view_proj[i] = shadow_cascades_[i].light_view_proj;
        gpu.cascade_splits[static_cast<glm::length_t>(i)] = shadow_cascades_[i].split_depth;
    }
    std::memcpy(uniform_buffers_[current_frame_].mapped, &gpu, sizeof(GpuFrameUniforms));

    vkResetFences(dev, 1, &in_flight_[current_frame_]);

    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);
    record_commands(cmd, image_index, items);

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

    // Textures / matériaux (M7 étape 3) : on libère les descriptor sets tant que le
    // pool est vivant, puis vues + images, puis pool / layout / sampler / pipeline.
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
    if (textured_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, textured_pipeline_layout_, nullptr);
        textured_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (texture_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, texture_pool_, nullptr);
        texture_pool_ = VK_NULL_HANDLE;
    }
    if (texture_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, texture_set_layout_, nullptr);
        texture_set_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }

    // Ombres (M8) : framebuffers/vues/images, puis sampler, pipelines, layout, passe.
    destroy_shadow_resources();
    if (shadow_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, shadow_sampler_, nullptr);
        shadow_sampler_ = VK_NULL_HANDLE;
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
