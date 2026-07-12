#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

#include "noire/core/math.hpp"
#include "noire/render/gpu_buffer.hpp"
#include "noire/render/swapchain.hpp"
#include "noire/render/vertex.hpp"
#include "noire/render/vulkan_context.hpp"

typedef struct VmaAllocation_T* VmaAllocation;

namespace noire::render {

struct RendererCreateInfo {
    ContextCreateInfo context;                         // extensions + fabrique de surface
    std::function<VkExtent2D()> get_framebuffer_size;  // taille courante (redimensionnement)
};

// Matrices caméra envoyées au GPU via UBO (uniquement des float : l'origine
// flottante est déjà résolue côté CPU).
struct FrameUniforms {
    glm::mat4 view;
    glm::mat4 proj;
};

// Un objet à dessiner : sa matrice Model (déjà relative à la caméra, en float)
// et le maillage à utiliser.
struct DrawItem {
    glm::mat4 model;
    MeshId mesh;
};

// Renderer générique : il ne connaît PAS la scène. L'app crée des maillages puis
// soumet une liste de DrawItem par frame. Toute la logique de simulation/monde
// (et les double) reste au-dessus.
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(const RendererCreateInfo& info);
    void shutdown();

    // Téléverse un maillage sur le GPU (via VMA) et renvoie son identifiant.
    MeshId create_mesh(const std::vector<Vertex>& vertices, Topology topology);

    void draw_frame(const FrameUniforms& uniforms, const std::vector<DrawItem>& items);
    void wait_idle();
    void notify_resized() { framebuffer_resized_ = true; }

private:
    struct Mesh {
        GpuBuffer vertex_buffer;
        std::uint32_t vertex_count = 0;
        Topology topology = Topology::Triangles;
    };

    bool create_descriptor_set_layout();
    bool create_pipeline_layout();
    bool create_pipelines();
    bool create_render_pass();
    bool create_depth_resources();
    bool create_framebuffers();
    bool create_command_objects();
    bool create_sync_objects();
    bool create_present_semaphores();
    bool create_uniform_buffers();
    bool create_descriptor_sets();

    VkPipeline build_pipeline(Topology topology);
    VkShaderModule create_shader_module(const unsigned char* code, std::size_t size);
    void record_commands(VkCommandBuffer cmd, std::uint32_t image_index,
                         const std::vector<DrawItem>& items);
    void recreate_swapchain();
    void destroy_depth_resources();

    static constexpr int kFramesInFlight = 2;

    VulkanContext context_;
    Swapchain swapchain_;
    std::function<VkExtent2D()> get_framebuffer_size_;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_triangles_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lines_ = VK_NULL_HANDLE;

    // Profondeur (partagée) pour le test de profondeur 3D.
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VmaAllocation depth_allocation_ = nullptr;
    VkImageView depth_view_ = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;  // kFramesInFlight

    std::vector<VkSemaphore> image_available_;  // kFramesInFlight
    std::vector<VkSemaphore> render_finished_;  // une par image de swapchain
    std::vector<VkFence> in_flight_;            // kFramesInFlight

    // UBO caméra : un tampon + un descriptor set par frame en vol.
    std::vector<GpuBuffer> uniform_buffers_;      // kFramesInFlight
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets_;  // kFramesInFlight

    std::vector<Mesh> meshes_;

    std::uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
};

}  // namespace noire::render
