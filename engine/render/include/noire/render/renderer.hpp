#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

#include "noire/render/swapchain.hpp"
#include "noire/render/vulkan_context.hpp"

namespace noire::render {

struct RendererCreateInfo {
    ContextCreateInfo context;                          // extensions + fabrique de surface
    std::function<VkExtent2D()> get_framebuffer_size;   // taille courante (redimensionnement)
};

// Orchestre le rendu d'une frame : render pass, pipeline du triangle, command
// buffers, synchronisation, présentation. Gère la reconstruction de la swapchain.
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(const RendererCreateInfo& info);
    void shutdown();

    void draw_frame();
    void wait_idle();
    void notify_resized() { framebuffer_resized_ = true; }

private:
    bool create_render_pass();
    bool create_pipeline();
    bool create_framebuffers();
    bool create_command_objects();
    bool create_sync_objects();
    bool create_present_semaphores();
    void record_commands(VkCommandBuffer cmd, std::uint32_t image_index);
    void recreate_swapchain();
    VkShaderModule create_shader_module(const unsigned char* code, std::size_t size);

    static constexpr int kFramesInFlight = 2;

    VulkanContext context_;
    Swapchain swapchain_;
    std::function<VkExtent2D()> get_framebuffer_size_;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;  // kFramesInFlight

    std::vector<VkSemaphore> image_available_;  // kFramesInFlight
    std::vector<VkSemaphore> render_finished_;  // une par image de swapchain
    std::vector<VkFence> in_flight_;            // kFramesInFlight

    std::uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
};

}  // namespace noire::render
