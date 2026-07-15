#pragma once

#include <array>
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

// Ombres portées du soleil (M8) : cascades de shadow maps depth-only. Deux cascades
// suffisent pour une sim ferroviaire (une proche très détaillée autour du train, une
// large pour le paysage). Au-delà de kShadowDistance, plus d'ombre projetée.
inline constexpr std::uint32_t kShadowCascades = 2;
inline constexpr std::uint32_t kShadowMapSize = 2048;
inline constexpr float kShadowDistance = 250.0f;  // portée max des ombres (m)

// Paramètres d'ENTRÉE de la frame, fournis par l'app (uniquement des float :
// l'origine flottante est déjà résolue côté CPU). Le Renderer en dérive l'UBO
// réellement envoyé au GPU, en y ajoutant ce qu'il calcule lui-même (matrices
// des cascades d'ombre).
struct FrameUniforms {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 fog_color_density;  // rgb = couleur du brouillard, a = densité
    glm::vec4 params;             // x = wetness (humidité 0..1), reste = réserve
    // xyz = direction VERS le soleil (normalisée, espace monde). Cadre les cascades
    // d'ombre ET éclaire les modèles : une seule source de vérité pour le soleil.
    glm::vec4 sun_direction{-0.4f, 0.8f, 0.3f, 0.0f};
    // rgb = couleur/intensité du soleil (espace LINÉAIRE : la swapchain est SRGB,
    // la conversion est faite par le matériel). a = intensité de l'ambiante ciel,
    // dont la teinte est celle du brouillard (= couleur du ciel).
    glm::vec4 sun_color{1.0f, 0.98f, 0.94f, 0.30f};
};

// Description d'un matériau PBR metallic-roughness (convention glTF), fournie par
// l'app à create_material(). Chaque texture peut valoir 0 => texture de secours du
// rôle correspondant, ce qui laisse le facteur seul décider (ex. un rail = pas de
// texture + base_color_factor gris acier).
struct MaterialDesc {
    TextureId base_color = 0;          // sRGB
    TextureId metallic_roughness = 0;  // linéaire — G = roughness, B = metallic (glTF)
    TextureId normal = 0;              // linéaire — normal map en espace tangent
    // Multiplie la texture de base. LINÉAIRE (la conversion sRGB est matérielle).
    glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor = 1.0f;   // défauts glTF
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
};

// Un objet à dessiner : sa matrice Model (déjà relative à la caméra, en float), le
// maillage, et — pour les maillages indexés (MeshVertex) — le matériau à appliquer.
// `material == 0` => matériau par défaut. Ignoré pour les maillages legacy (Vertex
// pos+couleur), dessinés par le pipeline debug.
struct DrawItem {
    glm::mat4 model;
    MeshId mesh;
    MaterialId material = 0;
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

    // Crée une texture GPU RGBA8 à partir de pixels CPU, téléversée de façon ASYNCHRONE
    // (staging + TransferManager). `rgba_pixels` = width*height*4 octets. `format` dit
    // au matériel s'il doit décoder le sRGB (couleur) ou non (données PBR).
    TextureId create_texture(std::uint32_t width, std::uint32_t height, const void* rgba_pixels,
                             TextureFormat format = TextureFormat::SrgbColor);
    [[nodiscard]] bool is_texture_ready(TextureId id) const;
    void destroy_texture(TextureId id);

    // Crée un matériau = un descriptor set 1 (3 textures) + ses facteurs. Le set n'est
    // écrit qu'une fois TOUTES ses textures téléversées : d'ici là is_material_ready()
    // est false et les DrawItem qui le référencent ne sont pas dessinés (même contrat
    // que is_mesh_ready). 0 = échec.
    MaterialId create_material(const MaterialDesc& desc);
    [[nodiscard]] bool is_material_ready(MaterialId id) const;
    void destroy_material(MaterialId id);

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

    // Texture GPU (M7 étape 3) : image + vue. `ready` bascule à true quand le transfert
    // (copie + transition de layout vers SHADER_READ_ONLY) est terminé ; le sampler est
    // partagé (sampler_). Depuis le M8 étape 3, le descriptor set appartient au
    // MATÉRIAU (set 1 = 3 textures), plus à la texture.
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        bool ready = false;
    };

    // Matériau GPU (M8 étape 3) : un descriptor set 1 portant les 3 textures PBR, plus
    // les facteurs (poussés en push constants au moment du draw). `written` bascule à
    // true quand le set a été écrit — ce qui n'a lieu qu'une fois toutes les textures
    // téléversées, donc AVANT tout bind : on ne réécrit jamais un set en vol.
    struct Material {
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        TextureId base_color = 0;
        TextureId metallic_roughness = 0;
        TextureId normal = 0;
        glm::vec4 base_color_factor{1.0f};
        glm::vec4 pbr_factors{1.0f, 1.0f, 1.0f, 0.0f};  // x=metallic, y=roughness, z=normal_scale
        bool written = false;
    };

    // Une cascade d'ombre : sa depth map + la matrice qui l'a cadrée cette frame.
    // Taille fixe (kShadowMapSize) => indépendante de la swapchain, donc jamais
    // recréée au redimensionnement.
    struct ShadowCascade {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        glm::mat4 light_view_proj{1.0f};
        float split_depth = 0.0f;  // distance de vue où finit la cascade (m)
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

    // Textures / matériaux (M7 étape 3, étendu M8 étape 3).
    bool create_sampler();
    bool create_material_descriptor_layout();  // set=1 : 3 combined image samplers (frag)
    bool create_material_descriptor_pool();
    bool create_textured_pipeline_layout();    // set0 (UBO) + set1 (matériau) + push
    bool create_default_textures();            // secours 1x1 : blanc, metal/rough, normale
    bool create_default_material();
    VkImageView create_image_view_2d(VkImage image, VkFormat format);
    // Identifiant réellement liable pour un rôle : celui demandé s'il existe, sinon la
    // texture de secours du rôle.
    [[nodiscard]] TextureId resolve_texture(TextureId requested, TextureId fallback) const;
    // Écrit le set 1 des matériaux dont toutes les textures viennent d'être téléversées.
    void update_pending_materials();
    void free_texture(Texture& texture);  // libère vue + image (device idle)

    // Ombres (M8 étape 1) : passe depth-only du soleil, une par cascade.
    bool create_shadow_render_pass();
    bool create_shadow_resources();        // images + vues + framebuffers des cascades
    bool create_shadow_sampler();          // sampler COMPARATIF (PCF matériel)
    bool create_shadow_pipeline_layout();  // push constant : lightViewProj * model
    bool create_shadow_pipelines();
    VkPipeline build_shadow_pipeline(std::uint32_t vertex_stride);
    // Recalcule le cadrage des cascades pour cette frame (dans l'espace flottant).
    void update_shadow_cascades(const FrameUniforms& uniforms);
    void record_shadow_pass(VkCommandBuffer cmd, const std::vector<DrawItem>& items);
    void destroy_shadow_resources();

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

    // --- Textures / matériaux (M7 étape 3, étendu M8 étape 3) ----------------
    VkSampler sampler_ = VK_NULL_HANDLE;                          // partagé (linéaire + aniso)
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;  // set=1 (3 bindings)
    VkDescriptorPool material_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout textured_pipeline_layout_ = VK_NULL_HANDLE;  // set0 + set1
    VkPipeline pipeline_textured_ = VK_NULL_HANDLE;               // MeshVertex + matériau
    std::unordered_map<TextureId, Texture> textures_;
    std::unordered_map<MaterialId, Material> materials_;
    TextureId next_texture_id_ = 1;
    MaterialId next_material_id_ = 1;
    // Textures de secours 1x1, une par rôle PBR (cf. create_default_textures).
    TextureId white_texture_ = 0;         // albédo : blanc pur
    TextureId default_mr_texture_ = 0;    // metallic = 0, roughness = 1
    TextureId flat_normal_texture_ = 0;   // normale plate (128,128,255)
    MaterialId default_material_ = 0;     // tout en secours (DrawItem::material == 0)
    static constexpr std::uint32_t kMaxTextures = 128;
    static constexpr std::uint32_t kMaxMaterials = 64;
    static constexpr std::uint32_t kMaterialTextures = 3;  // bindings du set 1

    // --- Ombres du soleil (M8 étape 1) ---------------------------------------
    // Passe depth-only rendue AVANT la passe principale. Deux pipelines car les deux
    // formats de sommet ont la position à l'offset 0 mais des strides différents.
    VkRenderPass shadow_render_pass_ = VK_NULL_HANDLE;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;  // comparatif : lié au set 0, binding 1
    VkPipelineLayout shadow_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_mesh_ = VK_NULL_HANDLE;    // MeshVertex (modèles indexés)
    VkPipeline shadow_pipeline_legacy_ = VK_NULL_HANDLE;  // Vertex (rails, géométrie debug)
    std::array<ShadowCascade, kShadowCascades> shadow_cascades_{};

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

    // Idem pour les textures (image + vue libérées en différé)...
    struct PendingTextureDelete {
        Texture texture;
        std::uint64_t destroy_at_frame = 0;
    };
    std::vector<PendingTextureDelete> pending_texture_deletes_;

    // ...et pour les matériaux (leur descriptor set rendu au pool).
    struct PendingMaterialDelete {
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        std::uint64_t destroy_at_frame = 0;
    };
    std::vector<PendingMaterialDelete> pending_material_deletes_;
    std::uint64_t frame_index_ = 0;

    // Couleur de fond = couleur du brouillard (mise à jour depuis les uniforms).
    glm::vec3 background_color_{0.01f, 0.01f, 0.03f};

    std::uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
};

}  // namespace noire::render
