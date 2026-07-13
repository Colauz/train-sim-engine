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
#include "noire/render/transfer_manager.hpp"
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

// Un objet à dessiner : sa matrice Model (déjà relative à la caméra, en float),
// le maillage à utiliser et, pour les maillages indexés (MeshVertex), la texture
// de base à appliquer. `texture == 0` => texture de secours (blanche 1x1). Ignoré
// pour les maillages legacy (Vertex pos+couleur), dessinés par le pipeline debug.
struct DrawItem {
    glm::mat4 model;
    MeshId mesh;
    TextureId texture = 0;
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
    // Chemin M2 : tampon host-visible à mapping persistant, immédiatement dessinable.
    MeshId create_mesh(const std::vector<Vertex>& vertices, Topology topology);

    // Chemin M7 : maillage INDEXÉ device-local (position/normale/UV), téléversé de
    // façon ASYNCHRONE via staging buffers (TransferManager). Renvoie immédiatement
    // un identifiant (0 = échec), mais le maillage n'est dessiné qu'une fois le
    // transfert GPU terminé — cf. is_mesh_ready(). Aucune attente sur la boucle.
    MeshId create_mesh_indexed(const std::vector<MeshVertex>& vertices,
                               const std::vector<std::uint32_t>& indices);
    // true quand le transfert GPU d'un maillage indexé est terminé (donc dessinable).
    // Toujours true pour les maillages du chemin M2 (create_mesh).
    [[nodiscard]] bool is_mesh_ready(MeshId id) const;

    // Détruit un maillage. La destruction GPU est DIFFÉRÉE de kFramesInFlight frames
    // pour ne jamais libérer un tampon encore référencé par une frame en vol.
    void destroy_mesh(MeshId id);

    // Crée une texture GPU (RGBA8 SRGB) à partir de pixels CPU, téléversée de façon
    // ASYNCHRONE (staging + TransferManager). `rgba_pixels` = width*height*4 octets.
    // Échantillonnable seulement une fois le transfert terminé (is_texture_ready) ;
    // en attendant, les DrawItem qui la référencent retombent sur la texture de secours.
    TextureId create_texture(std::uint32_t width, std::uint32_t height, const void* rgba_pixels);
    [[nodiscard]] bool is_texture_ready(TextureId id) const;
    void destroy_texture(TextureId id);
    // Texture blanche 1x1 de secours (toujours prête une fois l'init terminée).
    [[nodiscard]] TextureId white_texture() const { return white_texture_; }

    void draw_frame(const FrameUniforms& uniforms, const std::vector<DrawItem>& items);
    void wait_idle();
    void notify_resized() { framebuffer_resized_ = true; }

private:
    struct Mesh {
        GpuBuffer vertex_buffer;
        GpuBuffer index_buffer;  // valide si `indexed` (chemin M7)
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        Topology topology = Topology::Triangles;
        bool indexed = false;
        // false tant que le transfert GPU asynchrone n'est pas terminé (chemin M7).
        // Les maillages host-visible (M2) sont prêts dès leur création.
        bool ready = true;
    };

    // Texture GPU (M7 étape 3) : image + vue + descriptor set=1 (combined image
    // sampler). `ready` bascule à true quand le transfert (copie + transition de
    // layout vers SHADER_READ_ONLY) est terminé ; le sampler est partagé (sampler_).
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        bool ready = false;
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

    // Textures / matériaux (M7 étape 3).
    bool create_sampler();
    bool create_texture_descriptor_layout();  // set=1 : combined image sampler (frag)
    bool create_texture_descriptor_pool();
    bool create_textured_pipeline_layout();    // set0 (UBO) + set1 (sampler) + push model
    bool create_default_texture();             // texture blanche 1x1 de secours
    VkImageView create_image_view_2d(VkImage image, VkFormat format);
    // Texture à lier pour un DrawItem : celle demandée si prête, sinon le fallback ;
    // nullptr si même le fallback n'est pas encore prêt (toutes premières frames).
    [[nodiscard]] const Texture* resolve_texture(TextureId id) const;
    void free_texture(Texture& texture);  // libère descriptor + vue + image (device idle)

    VkPipeline build_pipeline(Topology topology);
    VkPipeline build_textured_pipeline();
    VkShaderModule create_shader_module(const unsigned char* code, std::size_t size);
    void record_commands(VkCommandBuffer cmd, std::uint32_t image_index,
                         const std::vector<DrawItem>& items);
    void recreate_swapchain();
    void destroy_depth_resources();
    void process_deferred_deletes();

    static constexpr int kFramesInFlight = 2;

    VulkanContext context_;
    TransferManager transfer_;  // téléversements GPU asynchrones (staging + fences)
    Swapchain swapchain_;
    std::function<VkExtent2D()> get_framebuffer_size_;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_triangles_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lines_ = VK_NULL_HANDLE;

    // --- Textures / matériaux (M7 étape 3) -----------------------------------
    VkSampler sampler_ = VK_NULL_HANDLE;                          // partagé (linéaire + aniso)
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;   // set=1
    VkDescriptorPool texture_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout textured_pipeline_layout_ = VK_NULL_HANDLE;  // set0 + set1
    VkPipeline pipeline_textured_ = VK_NULL_HANDLE;               // MeshVertex + textures
    std::unordered_map<TextureId, Texture> textures_;
    TextureId next_texture_id_ = 1;
    TextureId white_texture_ = 0;
    static constexpr std::uint32_t kMaxTextures = 128;

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

    // Idem pour les textures (image + vue + descriptor set libérés en différé).
    struct PendingTextureDelete {
        Texture texture;
        std::uint64_t destroy_at_frame = 0;
    };
    std::vector<PendingTextureDelete> pending_texture_deletes_;
    std::uint64_t frame_index_ = 0;

    // Couleur de fond = couleur du brouillard (mise à jour depuis les uniforms).
    glm::vec3 background_color_{0.01f, 0.01f, 0.03f};

    std::uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
};

}  // namespace noire::render
