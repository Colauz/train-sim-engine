#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
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
    // Position MONDE de la caméra, en double. Ne part PAS au GPU (les shaders travaillent
    // en caméra-relatif) : elle sert à ancrer le snap des cascades d'ombre sur une grille
    // de texels solidaire du MONDE. Sans elle, le snap se fait dans un repère qui suit la
    // caméra — donc il ne snappe rien du tout (le centre y est constant).
    glm::dvec3 camera_world_position{0.0, 0.0, 0.0};
    // INITIALISÉS : glm ne construit pas ses vecteurs par défaut (GLM_FORCE_CTOR_INIT
    // n'est pas posé, cf. cmake/Dependencies.cmake). L'écran de chargement instancie un
    // FrameUniforms sans renseigner params, que draw_frame lit et dont
    // update_shadow_cascades latche .y comme horloge du vent : sans initialiseur, c'est
    // une lecture indéterminée.
    glm::vec4 fog_color_density{0.0f};  // rgb = couleur du brouillard, a = densité
    glm::vec4 params{0.0f};             // x = wetness (humidité 0..1), y = temps (vent)
    // xyz = direction VERS le soleil (normalisée, espace monde). Cadre les cascades
    // d'ombre ET éclaire les modèles : une seule source de vérité pour le soleil.
    glm::vec4 sun_direction{-0.4f, 0.8f, 0.3f, 0.0f};
    // rgb = couleur/intensité du soleil (espace LINÉAIRE : la swapchain est SRGB, la
    // conversion est faite par le matériel). Calibré pour qu'une surface blanche
    // lambertienne face au soleil lise ~1.0 => l'irradiance vaut PI * sun_color.
    // Depuis le M8 étape 6b, l'app le renseigne depuis le soleil EXTRAIT du ciel HDR.
    // .a n'est plus utilisé : l'ambiante vient désormais des SH.
    glm::vec4 sun_color{1.0f, 0.98f, 0.94f, 0.30f};
    // Irradiance du ciel en harmoniques sphériques d'ordre 2 (M8 étape 6b), projetée au
    // chargement depuis le HDR (soleil déjà retiré : il est porté par sun_direction /
    // sun_color, l'y laisser le compterait deux fois). Seul .rgb porte l'information.
    // Tout à zéro (défaut) = aucune ambiante image-based, ex. si le ciel est absent.
    std::array<glm::vec4, 9> sh{};

    // --- Phares du TGV (M21) : 2 spots coniques, SANS ombre portée ---------------
    // xyz = position RELATIVE CAMÉRA (les shaders travaillent en origine flottante :
    // une position monde double ne tiendrait pas en float) ; .w = cos(demi-angle INTERNE).
    std::array<glm::vec4, 2> spot_positions{};
    // xyz = direction du faisceau (monde — invariante à la translation de l'origine) ;
    // .w = cos(demi-angle EXTERNE). Entre .w et spot_positions[i].w : pénombre douce.
    std::array<glm::vec4, 2> spot_directions{};
    // rgb = couleur x intensité (même calibration que sun_color : irradiance = PI * rgb) ;
    // .a = 1 phares allumés, 0 éteints (la touche L).
    glm::vec4 spot_color{0.0f};
    // Ciel (M21) : x = facteur nuit (0 = plein jour, 1 = nuit noire), y = couverture
    // nuageuse 0..1, z = horloge de dérive des nuages (s). Lu par skybox.frag et par
    // pbr.glsl (assombrissement du brouillard et de l'IBL spéculaire la nuit).
    glm::vec4 sky_params{0.0f};

    // Pluie (M14) — champs CPU UNIQUEMENT : ils ne partent PAS dans l'UBO std140 (le
    // pipeline de pluie a son propre push constant), donc les ajouter ici ne touche pas
    // au miroir GpuFrameUniforms / global_ubo.glsl. `intensity` = wetness (0 => aucune
    // passe de pluie) ; `tilt` = cisaillement écran croissant avec la vitesse ; `time` =
    // horloge de défilement des gouttes.
    float rain_intensity = 0.0f;
    float rain_tilt = 0.0f;
    float rain_time = 0.0f;
};

// Description d'un matériau PBR metallic-roughness (convention glTF), fournie par
// l'app à create_material(). Chaque texture peut valoir 0 => texture de secours du
// rôle correspondant, ce qui laisse le facteur seul décider (ex. un rail = pas de
// texture + base_color_factor gris acier).
// Quel pipeline dessine une surface. Ce choix NE PEUT PAS se déduire du reste : un poteau
// caténaire est instancié mais s'éclaire comme n'importe quelle surface, alors qu'un arbre
// instancié veut le vent et la transmission. Avant le M12, « instancié » valait « feuillage »
// — les deux étaient confondus, et le premier poteau se serait balancé dans le vent.
enum class Shading {
    Textured,  // surface opaque ordinaire (set 1 = 3 textures)
    Terrain,   // set 1 = 6 textures (2 jeux PBR) + splatting
    Wire,      // câble : ruban face-caméra, largeur clampée au pixel + couverture (M12)
};

struct MaterialDesc {
    TextureId base_color = 0;          // sRGB
    TextureId metallic_roughness = 0;  // linéaire — G = roughness, B = metallic (glTF)
    TextureId normal = 0;              // linéaire — normal map en espace tangent
    // Multiplie la texture de base. LINÉAIRE (la conversion sRGB est matérielle).
    glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor = 1.0f;   // défauts glTF
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
    Shading shading = Shading::Textured;
    // Feuillage : diffus enveloppé + transmission (cf. shadeSurfaceEx). Porté par le
    // MATÉRIAU et non par le pipeline, pour que le pipeline instancié serve aussi bien un
    // arbre qu'un poteau d'acier.
    bool foliage = false;
    // Matériau semi-transparent (vitrage, verre) : alphaMode BLEND. Dessiné APRÈS les
    // opaques, blending alpha src/one-minus-src, sans écriture de profondeur.
    bool transparent = false;
};

// Matériau de terrain : deux jeux PBR complets (M11 phase 2). Chaque texture peut valoir
// 0 => secours 1x1 du rôle, comme pour MaterialDesc.
struct TerrainMaterialDesc {
    TextureId grass_base = 0;          // sRGB
    TextureId grass_metallic_rough = 0;  // linéaire — _arm de Poly Haven convient tel quel
    TextureId grass_normal = 0;        // linéaire
    TextureId chalk_base = 0;
    TextureId chalk_metallic_rough = 0;
    TextureId chalk_normal = 0;
};

// Un objet à dessiner : sa matrice Model (déjà relative à la caméra, en float), le
// maillage, et — pour les maillages indexés (MeshVertex) — le matériau à appliquer.
// `material == 0` => matériau par défaut. Ignoré pour les maillages legacy (Vertex
// pos+couleur), dessinés par le pipeline debug.
struct DrawItem {
    glm::mat4 model;
    MeshId mesh;
    MaterialId material = 0;
    // Végétation (M11 phase 3) : si `instances` != 0, le maillage est dessiné
    // `instance_count` fois, chaque copie étant placée par le tampon d'instances. Le
    // pipeline change (entrée de sommets différente + vent), pas le matériau.
    InstanceBufferId instances = 0;
    std::uint32_t instance_count = 0;
    // Culling CPU (M15) : si non nul, la PASSE PRINCIPALE dessine CES instances (le
    // sous-ensemble visible, filtré par frustum côté app) au lieu de `instances` — elles
    // sont téléversées dans un tampon de streaming par-frame. `instances`/`instance_count`
    // restent utilisés tels quels par la PASSE D'OMBRES : un arbre hors champ caméra peut
    // encore projeter une ombre visible, on ne le retire donc pas des ombres. Le vecteur
    // pointé doit vivre jusqu'au retour de draw_frame (copié pendant l'enregistrement).
    const std::vector<InstanceData>* cpu_instances = nullptr;
};

// HUD (M13) — affichage écran. Générique : le Renderer sait dessiner du texte à des
// pixels, pas ce qu'est une vitesse. Le contenu reste à l'app.
//
// Une ligne de texte. `position` en PIXELS, origine HAUT-GAUCHE. `color` en LINÉAIRE :
// la swapchain est SRGB et l'encodage est matériel, comme partout ailleurs dans le
// moteur (cf. FrameUniforms::sun_color).
struct TextDraw {
    glm::vec2 position{0.0f, 0.0f};
    // Pixels par texel de police. ARRONDI À L'ENTIER par le Renderer : c'est ce qui
    // garantit qu'un texel couvre un bloc exact de pixels, donc que le texte est net.
    // Une échelle fractionnaire donnerait des jambages irréguliers (2 px ici, 3 px là).
    float scale = 2.0f;
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string text;
};

// Plaque de fond, pour que le texte reste lisible sur un ciel clair. Elle emprunte le
// MÊME chemin d'instance que les glyphes : c'est un glyphe dont les 35 bits sont à 1
// (cf. font::kGlyphBlock). Donc aucun pipeline de plus, aucun draw de plus.
struct HudRect {
    glm::vec2 position{0.0f, 0.0f};
    glm::vec2 size{0.0f, 0.0f};
    glm::vec4 color{0.0f, 0.0f, 0.0f, 0.5f};
};

// Tout le HUD d'une frame. Les rects sont émis AVANT les textes : le HUD tient en un
// seul draw, donc l'ordre des instances EST l'ordre de mélange — une plaque émise
// après son texte le recouvrirait.
struct Hud {
    std::vector<HudRect> rects;
    std::vector<TextDraw> texts;
    // Fondu d'ouverture (M15) : voile noir plein écran d'opacité `fade`, émis EN DERNIER
    // (donc au-dessus de tout, HUD compris). 0 = rien, 1 = écran noir. Passe par le même
    // pipeline que les glyphes — c'est un glyphe plein aux dimensions de l'écran.
    float fade = 0.0f;
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
    // Matériau de TERRAIN (M11 phase 2) : deux jeux PBR complets mélangés dans le shader
    // par pente + bruit. Il lui faut un set à 6 bindings, donc son propre layout et son
    // propre pipeline — le set 1 ordinaire n'en a que 3. Même contrat de disponibilité
    // que create_material : rien n'est dessiné avant que les 6 textures soient prêtes.
    MaterialId create_terrain_material(const TerrainMaterialDesc& desc);
    [[nodiscard]] bool is_material_ready(MaterialId id) const;
    void destroy_material(MaterialId id);

    // Crée la cubemap HDR d'environnement (M8 étape 6a) à partir des 6 faces déjà
    // rééchantillonnées côté CPU. `faces_rgba16f` = 6 * face_size² * 4 half-floats, dans
    // l'ordre Vulkan (+X, -X, +Y, -Y, +Z, -Z). Téléversement ASYNCHRONE, puis la chaîne
    // de mips est engendrée sur le GPU par vkCmdBlitImage. 0 = échec.
    EnvironmentId create_environment(std::uint32_t face_size, const void* faces_rgba16f);
    [[nodiscard]] bool is_environment_ready(EnvironmentId id) const;
    void destroy_environment(EnvironmentId id);
    // Environnement affiché en skybox. 0 = aucun (retour au nettoyage à la couleur de
    // fond). Tant qu'il n'est pas prêt, la skybox n'est simplement pas dessinée.
    void set_environment(EnvironmentId id) { active_environment_ = id; }

    // Tampon d'instances (M11 phase 3). Host-visible et immédiatement utilisable : les
    // données sont écrites une fois au semis et ne changent plus, et un accès par
    // INSTANCE (pas par sommet) supporte très bien la mémoire non device-local.
    InstanceBufferId create_instances(const std::vector<InstanceData>& instances);
    void destroy_instances(InstanceBufferId id);

    // `hud` par défaut = aucun HUD : l'écran de chargement appelle draw_frame sans lui.
    void draw_frame(const FrameUniforms& uniforms, const std::vector<DrawItem>& items,
                    const Hud& hud = {});
    void wait_idle();
    void notify_resized() { framebuffer_resized_ = true; }

    // Temps GPU de la dernière frame achevée, en millisecondes. À ne PAS confondre avec
    // le temps de frame CPU : celui-ci est plafonné par le VSync (FIFO), donc il mesure
    // l'ATTENTE, pas le travail. Les timestamps, eux, chronomètrent l'exécution réelle du
    // command buffer et ignorent le VSync — c'est la seule mesure honnête de la marge.
    // 0 tant qu'aucune mesure n'est revenue (les toutes premières frames).
    [[nodiscard]] float last_gpu_ms() const { return last_gpu_ms_; }

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
        std::uint32_t mips = 1;  // chaîne complète depuis le M9 (anti-grouillement)
        bool ready = false;
    };

    // Matériau GPU (M8 étape 3) : un descriptor set 1 portant les 3 textures PBR, plus
    // les facteurs (poussés en push constants au moment du draw). `written` bascule à
    // true quand le set a été écrit — ce qui n'a lieu qu'une fois toutes les textures
    // téléversées, donc AVANT tout bind : on ne réécrit jamais un set en vol.
    struct Material {
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        // 3 textures (matériau ordinaire) ou 6 (terrain : herbe + craie).
        std::array<TextureId, 6> textures{};
        std::uint32_t texture_count = 3;
        Shading shading = Shading::Textured;
        glm::vec4 base_color_factor{1.0f};
        glm::vec4 pbr_factors{1.0f, 1.0f, 1.0f, 0.0f};  // x=metallic, y=roughness, z=normal_scale
        bool written = false;
        // M22 : matériau semi-transparent (vitrage). Dessiné par pipeline_transparent_.
        bool transparent = false;
    };

    // Environnement GPU (M8 étape 6a) : la cubemap HDR du ciel + son descriptor set.
    // Contrairement à un matériau, son set est écrit DÈS la création : il ne référence
    // qu'une seule vue, qui existe déjà à cet instant (rien à attendre). C'est le DRAW
    // qui est conditionné par `ready`, pas l'écriture du set — donc on ne réécrit
    // jamais un set en vol non plus. Un set PAR environnement : changer de ciel = lier
    // un autre set, jamais réécrire celui-ci.
    struct Environment {
        // --- Source : le ciel net (M8 étape 6a) ---
        // Sert la skybox (mip 0) ET de SOURCE au préfiltrage GGX. Sa chaîne de mips
        // (blits) n'est pas décorative : le biais de mip de Karis en dépend.
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;      // CUBE, toute la chaîne
        VkDescriptorSet descriptor = VK_NULL_HANDLE;  // lié par le pipeline skybox
        std::uint32_t mips = 1;

        // --- Spéculaire préfiltré GGX (M8 étape 7) ---
        // Image SÉPARÉE : préfiltrer en place reviendrait à lire les mips qu'on écrit.
        // Chaque mip est un niveau de rugosité => le mapping rugosité -> lod devient
        // exact côté PBR. Sert aussi de couleur de brouillard (mip bas = ciel flou).
        VkImage spec_image = VK_NULL_HANDLE;
        VmaAllocation spec_allocation = nullptr;
        VkImageView spec_view = VK_NULL_HANDLE;  // CUBE, toute la chaîne (échantillonnage)
        VkDescriptorSet spec_descriptor = VK_NULL_HANDLE;  // set 2 du pipeline texturé
        std::uint32_t spec_mips = 1;
        // Une vue 2D_ARRAY par mip (écriture en storage image) + son set de compute.
        // On écrit en 2D_ARRAY et on lit en CUBE : pattern portable.
        std::vector<VkImageView> spec_mip_views;
        std::vector<VkDescriptorSet> prefilter_sets;

        bool ready = false;  // true quand copie + blits + préfiltrage + transitions sont finis
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
    bool create_terrain_descriptor_layout();   // set=1 : 6 (deux jeux PBR)
    VkPipeline build_foliage_pipeline();
    VkPipeline build_wire_pipeline();       // instancié + discard alpha
    bool create_terrain_pipeline_layout();
    VkPipeline build_terrain_pipeline();
    bool create_material_descriptor_pool();
    bool create_textured_pipeline_layout();    // set0 (UBO) + set1 (matériau) + push
    bool create_default_textures();            // secours 1x1 : blanc, metal/rough, normale
    bool create_default_material();
    VkImageView create_image_view_2d(VkImage image, VkFormat format,
                                    std::uint32_t mips = 1);
    // Identifiant réellement liable pour un rôle : celui demandé s'il existe, sinon la
    // texture de secours du rôle.
    [[nodiscard]] TextureId resolve_texture(TextureId requested, TextureId fallback) const;
    // Écrit le set 1 des matériaux dont toutes les textures viennent d'être téléversées.
    void update_pending_materials();
    void free_texture(Texture& texture);  // libère vue + image (device idle)

    // Environnement / skybox (M8 étape 6a).
    bool create_env_sampler();             // linéaire + mips + CLAMP_TO_EDGE
    bool create_env_descriptor_layout();   // set=1 du pipeline skybox : 1 samplerCube
    bool create_env_descriptor_pool();
    bool create_skybox_pipeline_layout();  // set0 (UBO) + set1 (cubemap), pas de push
    bool create_skybox_pipeline();
    // Cubemap de secours 1x1 (même esprit que les textures 1x1 par rôle) : le set 2 du
    // pipeline texturé doit TOUJOURS être liable, même si aucun ciel n'est chargé.
    bool create_default_environment();
    // Set d'environnement réellement liable : l'actif s'il est prêt, sinon le secours.
    // VK_NULL_HANDLE tant que même le secours n'est pas téléversé (1 ou 2 frames).
    [[nodiscard]] VkDescriptorSet resolve_environment_set() const;
    // Engendre la chaîne de mips par blits successifs et laisse TOUS les niveaux en
    // SHADER_READ_ONLY. Enregistré dans le command buffer d'un transfert. Générique :
    // sert aux textures 2D (layers=1) comme à la cubemap d'environnement (layers=6).
    // `dst_stage` doit couvrir TOUS les étages qui liront l'image ensuite.
    void record_mip_chain(VkCommandBuffer cmd, VkImage image, std::uint32_t width,
                          std::uint32_t height, std::uint32_t mips, std::uint32_t layers,
                          VkPipelineStageFlags dst_stage);
    // Nombre de niveaux d'une chaîne complète pour une image w x h.
    [[nodiscard]] static std::uint32_t mip_count(std::uint32_t width, std::uint32_t height);

    // Préfiltrage GGX (M8 étape 7) : première passe COMPUTE du moteur. Elle vit dans le
    // command buffer du TransferManager (famille 0 = GRAPHICS|COMPUTE) : un seul submit,
    // une seule fence, et l'asynchronisme est celui, déjà éprouvé, des téléversements.
    bool create_prefilter_descriptor_layout();  // samplerCube source + image2DArray dest
    bool create_prefilter_pipeline();           // layout + pipeline compute
    // Alloue l'image spec_, ses vues (CUBE + une 2D_ARRAY par mip) et ses sets.
    bool create_environment_specular(Environment& env, std::uint32_t face_size);
    // Enregistre les dispatches (un par mip) + les barrières qui les encadrent.
    void record_environment_prefilter(VkCommandBuffer cmd, const Environment& env,
                                      std::uint32_t face_size);
    void record_skybox(VkCommandBuffer cmd);
    void destroy_environment_gpu(Environment& env);

    // HUD (M13) : pipeline écran, instancié, sans texture ni push constant.
    bool create_hud_pipeline_layout();  // set 0 seul, ZÉRO push constant
    bool create_hud_pipeline();
    bool create_hud_buffers();  // un tampon d'instances par frame en vol
    // Convertit le HUD de l'app en instances et les écrit dans le tampon de la frame
    // courante. Renvoie le nombre d'instances (0 = rien à dessiner).
    std::uint32_t upload_hud(const Hud& hud);
    void record_hud(VkCommandBuffer cmd, std::uint32_t glyph_count);
    // Culling CPU (M15) : tampon d'instances de STREAMING, un par frame en vol, rempli à
    // la volée pendant l'enregistrement avec les sous-ensembles visibles des DrawItem.
    bool create_stream_buffers();

    // Pluie (M14) : plein écran, un seul push constant, ni set ni UBO.
    bool create_rain_pipeline();  // crée aussi son layout (push-constant-only)
    void record_rain(VkCommandBuffer cmd, const FrameUniforms& uniforms);

    // Ombres (M8 étape 1) : passe depth-only du soleil, une par cascade.
    bool create_shadow_render_pass();
    bool create_shadow_resources();        // images + vues + framebuffers des cascades
    bool create_shadow_sampler();          // sampler COMPARATIF (PCF matériel)
    bool create_shadow_pipeline_layout();  // push constant : lightViewProj * model
    bool create_shadow_pipelines();
    VkPipeline build_shadow_pipeline(std::uint32_t vertex_stride);
    // Ombre de la végétation : instanciée (binding 1) ET testée en alpha (donc un
    // fragment shader et le set 1), d'où un layout et un pipeline à part.
    bool create_shadow_foliage_pipeline_layout();
    VkPipeline build_shadow_foliage_pipeline();
    // Recalcule le cadrage des cascades pour cette frame (dans l'espace flottant).
    void update_shadow_cascades(const FrameUniforms& uniforms);
    void record_shadow_pass(VkCommandBuffer cmd, const std::vector<DrawItem>& items);
    void destroy_shadow_resources();

    VkPipeline build_pipeline(Topology topology);
    VkPipeline build_textured_pipeline();
    VkPipeline build_transparent_pipeline();  // M22 : vitrage alpha-blendé
    // Fabrique commune : même vertex (MeshVertex), fragment et layout paramétrés.
    // transparent = true : blend alpha src/one-minus-src, depth-write off, double face.
    VkPipeline build_surface_pipeline(const unsigned char* frag_spv, std::size_t frag_size,
                                      VkPipelineLayout layout, bool transparent = false);
    VkShaderModule create_shader_module(const unsigned char* code, std::size_t size);
    void record_commands(VkCommandBuffer cmd, std::uint32_t image_index,
                         const std::vector<DrawItem>& items, std::uint32_t glyph_count,
                         const FrameUniforms& uniforms);
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
    VkDescriptorSetLayout terrain_set_layout_ = VK_NULL_HANDLE;   // set=1 (6 bindings)
    VkPipelineLayout terrain_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_terrain_ = VK_NULL_HANDLE;
    // Végétation : même layout que le pipeline texturé (set 1 = 3 textures), seule
    // l'entrée de sommets diffère (binding 1 par instance) et le fragment discard.
    // Pipeline INSTANCIÉ (entrée de sommets à 2 bindings). Sert la végétation ET les
    // poteaux caténaire : ce qui les distingue est dans le matériau (foliage) et dans
    // l'instance (amplitude du vent), pas dans le pipeline.
    VkPipeline pipeline_foliage_ = VK_NULL_HANDLE;
    VkPipeline pipeline_wire_ = VK_NULL_HANDLE;  // câbles (M12) : ruban + fondu alpha
    // M22 : vitrage semi-transparent (alphaMode BLEND). Identique au pipeline texturé,
    // mais avec blending alpha activé, depth-write désactivé, et double face.
    VkPipeline pipeline_transparent_ = VK_NULL_HANDLE;

    // --- HUD (M13) -----------------------------------------------------------
    // Dessiné en TOUT DERNIER dans la passe principale, après le ciel : sans profondeur
    // et par-dessus tout le reste. Un seul draw instancié pour l'écran entier.
    VkPipelineLayout hud_pipeline_layout_ = VK_NULL_HANDLE;  // set 0 seul, 0 push
    VkPipeline hud_pipeline_ = VK_NULL_HANDLE;
    // Un tampon PAR FRAME EN VOL : contrairement aux tampons d'instances de la
    // végétation (écrits une fois au semis), celui-ci est réécrit à chaque frame. En
    // écrire un seul reviendrait à modifier de la mémoire encore lue par la frame
    // précédente. Même raisonnement, et même durée de vie, que uniform_buffers_.
    std::vector<GpuBuffer> hud_buffers_;  // kFramesInFlight
    // 512 glyphes = ~8 lignes de 60 caractères, très au-delà d'un HUD de cabine. À
    // 40 octets l'instance, les deux tampons pèsent 40 Ko.
    static constexpr std::uint32_t kMaxGlyphs = 512;

    // --- Instances de streaming (M15, culling CPU) -----------------------------
    // Un tampon par frame en vol, réécrit chaque frame avec les instances visibles des
    // DrawItem qui portent cpu_instances. Curseur remis à zéro à chaque enregistrement.
    std::vector<GpuBuffer> stream_buffers_;   // kFramesInFlight
    std::uint32_t stream_cursor_bytes_ = 0;   // avance pendant record_commands
    // 8192 instances = ~18x notre densité d'arbres typique : marge pour un culling large
    // et pour d'éventuels autres flux instanciés. 8192 * 32 o * 2 frames = 512 Ko.
    static constexpr std::uint32_t kMaxStreamInstances = 8192;

    // --- Pluie (M14) : effet plein écran, push-constant-only --------------------
    VkPipelineLayout rain_pipeline_layout_ = VK_NULL_HANDLE;  // 0 set, 1 push constant
    VkPipeline rain_pipeline_ = VK_NULL_HANDLE;

    std::unordered_map<InstanceBufferId, GpuBuffer> instance_buffers_;
    InstanceBufferId next_instance_id_ = 1;
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
    static constexpr std::uint32_t kTerrainTextures = 6;   // bindings du set 1 terrain

    // --- Environnement / skybox (M8 étape 6a) --------------------------------
    // Dessinée EN DERNIER dans la passe principale : la géométrie déjà en profondeur
    // rejette le ciel en early-z, donc on ne paie le fragment du ciel que sur les
    // pixels réellement vides.
    VkSampler env_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout env_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool env_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout skybox_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline skybox_pipeline_ = VK_NULL_HANDLE;
    std::unordered_map<EnvironmentId, Environment> environments_;
    EnvironmentId next_environment_id_ = 1;
    EnvironmentId active_environment_ = 0;
    EnvironmentId default_environment_ = 0;  // cubemap 1x1 de secours
    static constexpr std::uint32_t kMaxEnvironments = 4;

    // --- Préfiltrage GGX en compute (M8 étape 7) -----------------------------
    VkDescriptorSetLayout prefilter_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout prefilter_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline prefilter_pipeline_ = VK_NULL_HANDLE;
    // 512 par face : compromis VRAM/netteté. 256 rendrait molles nos vitres (rugosité
    // 0.05, quasi miroir) ; 1024 quadruplerait la facture pour un gain invisible.
    static constexpr std::uint32_t kSpecularFaceSize = 512;
    // 7 mips => rugosité 0, 1/6, ... 1. Au-delà, les faces sont trop petites pour porter
    // une information angulaire utile.
    static constexpr std::uint32_t kSpecularMips = 7;

    // --- Ombres du soleil (M8 étape 1) ---------------------------------------
    // Passe depth-only rendue AVANT la passe principale. Deux pipelines car les deux
    // formats de sommet ont la position à l'offset 0 mais des strides différents.
    VkRenderPass shadow_render_pass_ = VK_NULL_HANDLE;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;  // comparatif : lié au set 0, binding 1
    VkPipelineLayout shadow_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_mesh_ = VK_NULL_HANDLE;    // MeshVertex (modèles indexés)
    VkPipeline shadow_pipeline_legacy_ = VK_NULL_HANDLE;  // Vertex (rails, géométrie debug)
    VkPipelineLayout shadow_foliage_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_foliage_ = VK_NULL_HANDLE;  // MeshVertex + instances + discard
    // Horloge du vent, relevée avec les cascades. record_shadow_pass ne reçoit pas les
    // uniforms, et le vent DOIT être le même que dans la vue caméra : sinon l'arbre se
    // balance et son ombre reste plantée.
    float shadow_time_ = 0.0f;
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

    // Chronométrage GPU : 2 timestamps (début/fin du command buffer) par frame en vol.
    // On relit ceux du slot courant APRÈS son fence — donc les valeurs datent de
    // kFramesInFlight frames, ce qui est sans importance pour un profil.
    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
    float timestamp_period_ = 0.0f;  // ns par tick (limite du device)
    float last_gpu_ms_ = 0.0f;
    // Un slot n'est lisible qu'une fois SA première frame enregistrée : relire une requête
    // jamais écrite est une faute (VUID-vkGetQueryPoolResults-None-09401), et pas
    // seulement une valeur douteuse.
    std::array<bool, kFramesInFlight> timestamp_written_{};

    std::uint32_t current_frame_ = 0;
    bool framebuffer_resized_ = false;
};

}  // namespace noire::render
