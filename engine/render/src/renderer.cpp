#include "noire/render/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "noire/core/log.hpp"

// En-têtes SPIR-V embarqués (générés au build : voir cmake/Shaders.cmake).
#include "shaders/mesh.frag.spv.h"
#include "shaders/mesh.vert.spv.h"

namespace noire::render {

namespace {
VkPrimitiveTopology to_vk_topology(Topology topology) {
    return topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
}  // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialize(const RendererCreateInfo& info) {
    get_framebuffer_size_ = info.get_framebuffer_size;

    if (!context_.initialize(info.context)) {
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
        !create_command_objects() || !create_sync_objects() || !create_uniform_buffers() ||
        !create_descriptor_sets()) {
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

void Renderer::destroy_mesh(MeshId id) {
    const auto it = meshes_.find(id);
    if (it == meshes_.end()) {
        return;
    }
    // Destruction différée : le tampon peut encore être référencé par une frame en vol.
    pending_deletes_.push_back(
        PendingDelete{it->second.vertex_buffer, frame_index_ + kFramesInFlight + 1});
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
    VkDescriptorSetLayoutBinding ubo_binding{};
    ubo_binding.binding = 0;
    ubo_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo_binding.descriptorCount = 1;
    ubo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &ubo_binding;

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
        if (!context_.create_buffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    /*host_visible=*/true, uniform_buffers_[static_cast<std::size_t>(i)])) {
            return false;
        }
    }
    return true;
}

bool Renderer::create_descriptor_sets() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
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

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = uniform_buffers_[static_cast<std::size_t>(i)].buffer;
        buffer_info.offset = 0;
        buffer_info.range = sizeof(FrameUniforms);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_sets_[static_cast<std::size_t>(i)];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
    }
    return true;
}

void Renderer::record_commands(VkCommandBuffer cmd, std::uint32_t image_index,
                               const std::vector<DrawItem>& items) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

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

    // UBO caméra : lié une fois (identique pour tous les objets de la frame).
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_sets_[current_frame_], 0, nullptr);

    for (const DrawItem& item : items) {
        const auto mesh_it = meshes_.find(item.mesh);
        if (mesh_it == meshes_.end()) {
            continue;
        }
        const Mesh& mesh = mesh_it->second;
        VkPipeline pipeline =
            mesh.topology == Topology::Lines ? pipeline_lines_ : pipeline_triangles_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(glm::mat4), &item.model);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer.buffer, &offset);
        vkCmdDraw(cmd, mesh.vertex_count, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void Renderer::draw_frame(const FrameUniforms& uniforms, const std::vector<DrawItem>& items) {
    VkDevice dev = context_.device();

    // Compteur de frames monotone + traitement des destructions GPU différées.
    ++frame_index_;
    process_deferred_deletes();

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

    // Mise à jour de l'UBO de CETTE frame (mapping persistant).
    std::memcpy(uniform_buffers_[current_frame_].mapped, &uniforms, sizeof(FrameUniforms));

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
    }
    meshes_.clear();
    for (PendingDelete& pending : pending_deletes_) {
        context_.destroy_buffer(pending.buffer);
    }
    pending_deletes_.clear();

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
