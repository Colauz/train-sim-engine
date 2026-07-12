#include "noire/render/renderer.hpp"

#include <array>
#include <cstdint>

#include "noire/core/log.hpp"

// En-têtes SPIR-V embarqués (générés au build : voir cmake/Shaders.cmake).
#include "shaders/triangle.frag.spv.h"
#include "shaders/triangle.vert.spv.h"

namespace noire::render {

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

    if (!create_render_pass() || !create_pipeline() || !create_framebuffers() ||
        !create_command_objects() || !create_sync_objects()) {
        return false;
    }

    log::info("Renderer prêt : {} images, {}x{}", swapchain_.image_count(),
              swapchain_.extent().width, swapchain_.extent().height);
    return true;
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

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
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

bool Renderer::create_pipeline() {
    VkShaderModule vert = create_shader_module(triangle_vert_spv, triangle_vert_spv_size);
    VkShaderModule frag = create_shader_module(triangle_frag_spv, triangle_frag_spv_size);
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

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{vs, fs};

    // Aucun buffer de sommets : les positions sont codées dans le vertex shader.
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport et scissor dynamiques : pas de reconstruction du pipeline au resize.
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
        log::error("Vulkan : création du pipeline layout échouée");
        vkDestroyShaderModule(context_.device(), vert, nullptr);
        vkDestroyShaderModule(context_.device(), frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = static_cast<std::uint32_t>(stages.size());
    pipe.pStages = stages.data();
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &input_asm;
    pipe.pViewportState = &viewport_state;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &multisample;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = pipeline_layout_;
    pipe.renderPass = render_pass_;
    pipe.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipe,
                                                nullptr, &pipeline_);

    vkDestroyShaderModule(context_.device(), vert, nullptr);
    vkDestroyShaderModule(context_.device(), frag, nullptr);

    if (result != VK_SUCCESS) {
        log::error("Vulkan : création du pipeline graphique échouée");
        return false;
    }
    return true;
}

bool Renderer::create_framebuffers() {
    const auto& views = swapchain_.image_views();
    framebuffers_.resize(views.size());
    for (std::size_t i = 0; i < views.size(); ++i) {
        VkImageView attachments[] = {views[i]};
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = render_pass_;
        fb.attachmentCount = 1;
        fb.pAttachments = attachments;
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
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 1re frame : pas d'attente initiale

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
    // Un sémaphore « rendu terminé » par image de swapchain : évite qu'un sémaphore
    // soit réutilisé alors qu'une présentation est encore en cours (warning de validation).
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

void Renderer::record_commands(VkCommandBuffer cmd, std::uint32_t image_index) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clear{};
    clear.color = {{0.01f, 0.01f, 0.03f, 1.0f}};  // bleu nuit « Noire »

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = swapchain_.extent();
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // 3 sommets, 1 instance : le triangle

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void Renderer::draw_frame() {
    VkDevice dev = context_.device();
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

    vkResetFences(dev, 1, &in_flight_[current_frame_]);

    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);
    record_commands(cmd, image_index);

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
        return;  // fenêtre minimisée : on saute (l'app attend les événements)
    }

    vkDeviceWaitIdle(context_.device());

    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(context_.device(), fb, nullptr);
    }
    framebuffers_.clear();

    swapchain_.recreate(extent);
    create_present_semaphores();  // le nombre d'images peut changer
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
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
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
