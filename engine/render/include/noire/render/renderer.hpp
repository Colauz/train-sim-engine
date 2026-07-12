#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
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

// UBO GLOBAL envoyé au GPU (uniquement des float : l'origine flottante est déjà
// résolue côté CPU). Contient les matrices caméra + les paramètres météo.
struct FrameUniforms {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 fog_color_density;  // rgb = couleur du brouillard, a = densité
    glm::vec4 params;             // x = wetness (humidité 0..1), reste = réserve
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

    // Téléverse un maillage sur le GPU (via VMA) et renvoie son identifiant (0 = échec).
    MeshId create_mesh(const std::vector<Vertex>& vertices, Topology topology);
    // Détruit un maillage. La destruction GPU est DIFFÉRÉE de kFramesInFlight frames
    // pour ne jamais libérer un tampon encore référencé par une frame en vol.
    void destroy_mesh(MeshId id);

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
    void process_deferred_deletes();

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

    // Maillages indexés par identifiant (permet créations/suppressions dynamiques
    // pour le streaming). 0 = identifiant invalide.
    std::unordered_map<MeshId, Mesh> meshes_;
    MeshId next_mesh_id_ = 1;

    // File de destruction GPU différée : chaque tampon n'est libéré qu'une fois
    // qu'aucune frame en vol ne peut plus le référencer.
    struct PendingDelete {
        GpuBuffer buffer;
        std::uint64_t destroy_at_frame = 0;
    };
    std::vector<PendingDelete> pending_deletes_;
    std::uint64_t frame_index_ = 0;

    // Couleur de fond = couleur du brouillard (mise à jour depuis les uniforms).
    glm::vec3 background_color_{0.01f, 0.01f, 0.03f};

    std::uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
};

}  // namespace noire::render
