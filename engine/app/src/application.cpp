#include "noire/app/application.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "noire/audio/audio_engine.hpp"
#include "noire/audio/rail_audio.hpp"
#include "noire/core/camera.hpp"
#include "noire/core/engine.hpp"
#include "noire/core/job_system.hpp"
#include "noire/core/log.hpp"
#include "noire/core/math.hpp"
#include "noire/core/procedural_track.hpp"
#include "noire/core/terrain.hpp"
#include "noire/physics/consist.hpp"
#include "noire/physics/wagon.hpp"
#include "noire/platform/window.hpp"
#include "noire/render/renderer.hpp"
#include "noire/render/vertex.hpp"
#include "noire/resource/asset_paths.hpp"
#include "noire/resource/resource_manager.hpp"
#include "noire/scene/catenary.hpp"
#include "noire/scene/track_mesh.hpp"
#include "noire/scene/viaduct.hpp"
#include "noire/scene/terrain_clipmap.hpp"
#include "noire/scene/world_streamer.hpp"

namespace noire {

namespace {

constexpr WorldPosition kTrackOrigin{1000000.0, 0.0, 1000000.0};

// Rame de métro japonais (M30 — pivot E235). Chiffres calés sur une rame de
// banlieue de Tokyo (type E235, 3 voitures simulées ici), et cohérents entre eux.
physics::WagonConfig make_metro_config() {
    physics::WagonConfig c;
    c.mass = 96000.0;  // 3 voitures x ~32 t en charge
    // Rame de métro : la grande majorité des essieux est moteur => la masse
    // adhérente vaut la masse totale (0 = défaut, tous essieux moteurs).
    c.adhesive_mass = 0.0;
    c.wheelbase = 14.0;
    // Accélération de service ~0,9 m/s² au démarrage (E235 : 3,0 km/h/s).
    c.max_tractive_effort = 90000.0;
    // 1900 kW à la jante => vitesse de base = 1.9e6 / 9e4 = 21 m/s = 76 km/h.
    // Au-delà, l'effort s'effondre en P/v — à 110 km/h il ne reste que 62 kN.
    c.max_power = 1900000.0;
    c.max_brake_force = 100000.0;  // service maximal => ~1,0 m/s²
    // Davis d'une boîte à roulettes : traînée modeste à 110 km/h, roulage dominant.
    c.davis_a = 600.0;
    c.davis_b = 20.0;
    c.davis_c = 2.5;
    return c;
}

std::vector<render::Vertex> make_box_vertices(const glm::vec3& base_color) {
    const glm::vec3 c[8] = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},
    };
    struct Face {
        int a, b, c, d;
        float shade;
    };
    const Face faces[6] = {
        {4, 5, 6, 7, 1.00f}, {1, 0, 3, 2, 0.80f}, {0, 4, 7, 3, 0.70f},
        {5, 1, 2, 6, 0.90f}, {7, 6, 2, 3, 1.15f}, {0, 1, 5, 4, 0.55f},
    };
    std::vector<render::Vertex> vertices;
    vertices.reserve(36);
    for (const Face& f : faces) {
        const glm::vec3 color = base_color * f.shade;
        const int order[6] = {f.a, f.b, f.c, f.a, f.c, f.d};
        for (int i : order) {
            vertices.push_back(render::Vertex{c[i], color});
        }
    }
    return vertices;
}

// Ajoute une boîte orientée-axes (24 sommets, 6 faces à normales propres) au format
// MeshVertex — pour la géométrie PBR construite en code (les signaux ATS du M30). Chaque
// face porte sa propre normale et une tangente dans son plan : arêtes franches, éclairage
// correct.
void append_box(std::vector<render::MeshVertex>& verts, std::vector<std::uint32_t>& indices,
                const glm::vec3& center, const glm::vec3& half) {
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                  {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : normals) {
        // Deux axes dans le plan de la face (u, v) et les demi-longueurs correspondantes.
        const glm::vec3 ref = (std::abs(n.y) > 0.9f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 u = glm::normalize(glm::cross(ref, n));
        const glm::vec3 v = glm::cross(n, u);
        const float hu = std::abs(glm::dot(u, half));
        const float hv = std::abs(glm::dot(v, half));
        const glm::vec3 fc = center + n * glm::abs(glm::dot(n, half));
        const glm::vec4 tangent(u, 1.0f);
        const glm::vec3 corners[4] = {fc - u * hu - v * hv, fc + u * hu - v * hv,
                                      fc + u * hu + v * hv, fc - u * hu + v * hv};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i) {
            verts.push_back(render::MeshVertex{corners[i], n, uvs[i], tangent});
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

// --- Sol (M8 étape 2) -------------------------------------------------------
// Grand plan solide texturé, qui remplace la grille filaire du M2 : c'est lui qui
// reçoit l'ombre portée du train.

// Doit couvrir le plan lointain de la caméra (10 km depuis le M9) dans toutes les
// directions, sinon le vide apparaît au-delà.
// Altitude ABSOLUE du plan de sol. Calée sous le point le plus BAS de la spline
// (amplitude verticale 6 m) moins la profondeur du ballast : la voie est donc toujours
// en remblai au-dessus du sol, jamais en tranchée — un simple quad ne saurait pas se
// percer d'un trou. AVANT le M9, le sol suivait le train (body_y - 3.0) : il divergeait
// de la voie jusqu'à 12 m au loin, d'où les rails qui flottaient ou s'enterraient.
constexpr double kGroundLevel = -6.8;
// Période de répétition de la texture, en mètres. C'est aussi le pas de recentrage du
// sol, pour que les UV ne glissent pas sous le train. (Sa valeur était grande faute de
// mipmaps ; le M9 les a activées, elle pourrait donc être réduite.)
constexpr std::uint32_t kGroundTextureSize = 256;

// Couleurs de la voie, portées par le base_color_factor de leur matériau (M8 étape 3).
// Valeurs LINÉAIRES (la conversion sRGB est matérielle).
constexpr glm::vec3 kRailColor{0.55f, 0.57f, 0.62f};      // acier poli par les roues
constexpr glm::vec3 kSleeperColor{0.28f, 0.27f, 0.25f};   // béton gris, un peu sale
constexpr glm::vec3 kBallastColor{0.20f, 0.19f, 0.17f};   // gravier gris sombre

// --- Semis de bâtiments (M31 — Neo-Tokyo) -------------------------------------
// DÉTERMINISTE par construction : la position d'un immeuble ne dépend QUE du hash de sa
// cellule. Aucun état, aucun compteur, aucun aléa de frame — le train peut repasser, la
// tuile être déchargée puis régénérée, l'app redémarrer : la ville est identique.
// Trois variantes de gabarit (tour, immeuble, barre — tools/gen_building.py) car
// l'échelle d'instance est uniforme : la variété des proportions est cuite dans les
// modèles. --- Caténaire (M12) ---
// Fenêtre engendrée de part et d'autre du train, et pas de re-génération. La fenêtre est
// LARGE parce que c'est justement au loin que le rendu des câbles se joue : à 2 km, un fil
// de contact fait 0,005 pixel, et c'est là qu'on veut le voir tenir.
constexpr double kCatenaryRange = 2000.0;
constexpr double kCatenaryStep = 200.0;

// Viaduc (M31) : tablier + piles, engendré comme la caténaire sur une fenêtre glissante.
// Portée plus grande que les câbles : le tablier doit exister partout où la voie se
// voit encore (les tuiles de voie couvrent 10 km, mais le brouillard voile au-delà).
constexpr double kViaductRange = 3500.0;
constexpr double kViaductStep = 250.0;

constexpr double kBuildingCell = 48.0;        // une cellule de semis, en mètres
constexpr double kBuildingHalfWidth = 380.0;  // portée latérale de part et d'autre du viaduc
constexpr double kBuildingRange = 700.0;      // au-delà, une tour de 100 m fond dans la nuit
constexpr double kBuildingCorridor = 30.0;    // exclusion autour de l'axe du viaduc
constexpr int kBuildingVariants = 3;          // tower / block / slab (gen_building.py)

// --- Calibrage des modèles importés (M9) ------------------------------------
// Un modèle trouvé sur internet n'a JAMAIS la bonne échelle, la bonne orientation ni la
// bonne assiette : centimètres au lieu de mètres, +X vers l'avant au lieu de -Z, origine
// au sol plutôt qu'au centre de caisse... Plutôt que de patcher le .glb, on corrige à
// l'affichage. La transformation s'insère ENTRE la physique (bogies M4) et le modèle :
//
//   monde = relative_model(body_position) * body_orientation * ModelTransform::matrix()
//
// La physique n'en sait donc rien : elle continue de raisonner sur une caisse abstraite,
// et le calibrage ne déplace QUE les triangles.
struct ModelTransform {
    float scale = 1.0f;       // ex. 0.01 pour un modèle exporté en centimètres
    float offset_y = 0.0f;    // remonte/descend la caisse pour poser les roues sur le rail
    float offset_z = 0.0f;    // avance/recule la caisse le long de la voie (M19)
    float rotation_y = 0.0f;  // radians, autour de la verticale : oriente l'avant du modèle
    // Ordre VOULU : rotation d'abord, translation ensuite. L'inverse ferait décrire un
    // arc à l'offset au lieu de rester vertical.
    [[nodiscard]] glm::mat4 matrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, offset_y, offset_z));
        m = glm::rotate(m, rotation_y, glm::vec3(0.0f, 1.0f, 0.0f));
        return glm::scale(m, glm::vec3(scale));
    }
};

// Calibrage de la motrice (tools/gen_metro.py). Le modèle est généré DANS le
// repère caisse et en mètres : échelle 1, rotation nulle.
// offset_y = 0 : le script pose le bas des roues exactement sur y = -2.20 = -body_height,
// c'est-à-dire sur le plan de roulement (vérifié : bbox acier y[-2.20, -1.15]). Un modèle
// importé, lui, aura presque toujours besoin des trois champs.
constexpr ModelTransform kLocoTransform{1.0f, 0.0f, 0.0f};
// Voiture et bogie Jacobs (M16), générés dans le même repère caisse (rail à -2.20 pour la
// voiture ; origine au plan de roulement pour le bogie). Calibrage neutre, comme la motrice.
constexpr ModelTransform kCarTransform{1.0f, 0.0f, 0.0f};
constexpr ModelTransform kBogieTransform{1.0f, 0.0f, 0.0f};

std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float hash_unit(std::uint32_t x, std::uint32_t y) {
    return static_cast<float>(hash_u32(x * 374761393U + y * 668265263U)) /
           static_cast<float>(0xffffffffU);
}

// Bruit de valeur lissé et RACCORDABLE (indices modulo `cells`) : la texture étant
// répétée sur tout le sol, une couture serait visible à chaque période.
float value_noise(float u, float v, std::uint32_t cells) {
    const float sx = u * static_cast<float>(cells);
    const float sy = v * static_cast<float>(cells);
    const auto ix = static_cast<std::uint32_t>(sx);
    const auto iy = static_cast<std::uint32_t>(sy);
    const float tx = sx - static_cast<float>(ix);
    const float ty = sy - static_cast<float>(iy);
    const float ux = tx * tx * (3.0f - 2.0f * tx);  // lissage cubique (pas d'arêtes)
    const float uy = ty * ty * (3.0f - 2.0f * ty);
    const float a = hash_unit(ix % cells, iy % cells);
    const float b = hash_unit((ix + 1) % cells, iy % cells);
    const float c = hash_unit(ix % cells, (iy + 1) % cells);
    const float d = hash_unit((ix + 1) % cells, (iy + 1) % cells);
    return glm::mix(glm::mix(a, b, ux), glm::mix(c, d, ux), uy);
}

unsigned char to_srgb_byte(float v) {
    return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Sol urbain (M31) : asphalte et béton sombres, 2 octaves de bruit lissé, généré à la
// volée (aucun asset). La texture est RGBA8 SRGB => les octets écrits ici sont des
// valeurs sRGB, que le matériel reconvertit en linéaire à l'échantillonnage.
std::vector<unsigned char> make_ground_pixels(std::uint32_t size) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4u);
    const glm::vec3 dark{0.15f, 0.15f, 0.16f};    // asphalte
    const glm::vec3 light{0.30f, 0.30f, 0.31f};   // béton usé
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const float n = 0.65f * value_noise(u, v, 4) + 0.35f * value_noise(u, v, 16);
            const glm::vec3 c = glm::mix(dark, light, n);
            const std::size_t o = (static_cast<std::size_t>(y) * size + x) * 4u;
            pixels[o + 0] = to_srgb_byte(c.r);
            pixels[o + 1] = to_srgb_byte(c.g);
            pixels[o + 2] = to_srgb_byte(c.b);
            pixels[o + 3] = 255;
        }
    }
    return pixels;
}

}  // namespace

struct Application::Impl {
    explicit Impl(ApplicationConfig cfg)
        : config(std::move(cfg)),
          window(platform::WindowConfig{config.width, config.height, config.title}),
          engine(EngineConfig{config.simulation_hz, 0}),
          track(kTrackOrigin),
          terrain(track),
          consist(make_metro_config(), make_consist_config()),
          streamer(track, jobs, make_streamer_config()),
          clipmap(terrain, jobs, make_clipmap_config()),
          resources(renderer, jobs, asset_paths) {}

    static physics::ConsistConfig make_consist_config() {
        physics::ConsistConfig cc;
        cc.car_count = 2;              // motrice + 2 voitures (rame de 3)
        cc.loco_half_length = 10.0;    // voiture de 20 m
        cc.car_spacing = 20.5;         // 20 m de caisse + 0,5 m d'intercirculation
        cc.head_to_first_jacobs = 0.6;
        cc.ats_margin_kmh = 5.0;       // ATS : tolérance avant l'urgence (métro = strict)
        return cc;
    }

    static scene::ClipmapConfig make_clipmap_config() {
        scene::ClipmapConfig cc;
        // Les UV se calent sur l'origine de la voie : fixe, donc la texture ne glisse
        // jamais, et les valeurs restent petites malgré des coordonnées monde à 7 chiffres.
        cc.uv_origin = glm::dvec2(kTrackOrigin.x, kTrackOrigin.z);
        return cc;
    }

    static scene::StreamerConfig make_streamer_config() {
        scene::StreamerConfig sc;
        // Tuiles COURTES (500 m) : c'est la granularité du LOD. À 2 km, le détail complet
        // portait jusqu'à 4 km et les traverses grouillaient ; à 500 m avec un rayon de 1,
        // il s'arrête à 1 km — là où une traverse fait encore plusieurs pixels.
        sc.chunk_length = 500.0;
        sc.chunks_ahead = 20;  // 20 x 500 m = 10 km de voie chargée
        sc.chunks_behind = 2;
        sc.full_lod_radius = 1;
        // 22 tuiles à charger au démarrage : sans budget relevé, l'écran de chargement
        // durerait 22 frames de plus pour rien.
        sc.max_uploads_per_update = 3;
        sc.rail_profile.sample_step = 2.0;
        return sc;
    }

    ApplicationConfig config;
    platform::Window window;
    render::Renderer renderer;
    Engine engine;
    Camera camera;

    ProceduralTrack track;
    Terrain terrain;
    JobSystem jobs;
    // Rame articulée (M16) : motrice + N voitures sur bogies Jacobs. `wagon` reste une
    // référence vers la motrice — tout le code qui lit l'état de tête (vitesse, chainage,
    // caisse...) est inchangé ; seuls les appels qui PILOTENT la rame passent par `consist`.
    physics::Consist consist;
    physics::Wagon& wagon = consist.loco();
    scene::WorldStreamer streamer;
    scene::TerrainClipmap clipmap;

    audio::AudioEngine audio;
    audio::RailAudio rail_audio;

    // Pipeline d'assets (M7 étapes 4-5) : racine assets/ + cache/loaders asynchrones.
    resource::AssetPaths asset_paths;
    resource::ResourceManager resources;
    resource::ModelHandle train_model;
    resource::ModelHandle voiture_model;       // voiture voyageurs (M16)
    resource::ModelHandle jacobs_bogie_model;  // bogie Jacobs partagé (M16)
    resource::ModelHandle station_model;       // module de gare répétable (M18)
    // Bâtiments (M31) : trois gabarits (tour / immeuble / barre, gen_building.py).
    std::array<resource::ModelHandle, kBuildingVariants> building_models;
    resource::AudioHandle rumble_clip;
    resource::EnvironmentHandle sky;
    // Textures PBR du ballast (Poly Haven, CC0). Maintenues vivantes par ces handles :
    // les relâcher recyclerait les textures GPU sous le matériau.
    resource::TextureHandle ballast_diff;
    resource::TextureHandle ballast_arm;
    resource::TextureHandle ballast_nor;
    bool ballast_textured = false;
    // Splatting du terrain (M11 phase 2) : deux jeux PBR complets, herbe et craie.
    std::array<resource::TextureHandle, 6> terrain_maps;
    bool terrain_textured = false;

    // Bâtiments (M31) : un tampon d'instances PAR VARIANTE (le pipeline instancié ne
    // connaît qu'une échelle uniforme, la variété des proportions est dans les modèles).
    // Refaits quand le train change de cellule ; le semis est purement fonction du hash,
    // donc le refaire est idempotent.
    std::array<render::InstanceBufferId, kBuildingVariants> building_instances{};
    std::array<std::uint32_t, kBuildingVariants> building_counts{};
    WorldPosition building_origin{};
    long building_snap_x = 0, building_snap_z = 0;
    bool building_snap_valid = false;
    // Culling CPU (M15) : les listes COMPLÈTES par variante (conservées pour les retester
    // chaque frame contre le frustum) et les sous-ensembles VISIBLES recalculés à chaque
    // frame. Membres pour éviter une réallocation par frame.
    std::array<std::vector<render::InstanceData>, kBuildingVariants> building_lists;
    std::array<std::vector<render::InstanceData>, kBuildingVariants> visible_buildings;
    std::array<std::uint32_t, kBuildingVariants> visible_building_counts{};
    double sim_time = 0.0;  // horloge du vent (s)
    long present_index = 0;  // compte les présentations (banc de mesure, cf. NOIRE_CREEP)
    std::chrono::steady_clock::time_point perf_t0 = std::chrono::steady_clock::now();
    int perf_frames = 0;
    double perf_gpu_sum = 0.0;
    double perf_fps = 0.0;
    double perf_gpu_ms = 0.0;
    bool rumble_source_applied = false;

    // Sol : plan solide texturé (M8 étape 2), il reçoit l'ombre portée du train.
    // Secours du terrain tant que ses textures Poly Haven ne sont pas arrivées. Le
    // MAILLAGE plat, lui, est mort avec le clipmap (M11 phase 1) : il était encore
    // créé sur le GPU sans jamais être dessiné.
    render::TextureId ground_texture = 0;
    render::MaterialId ground_material = 0;
    // Voie : trois matériaux partagés par toutes les tuiles (M9). Sans texture pour
    // l'instant — les UV sont générés et à l'échelle physique, prêts à en recevoir.
    render::MaterialId rail_material = 0;
    // --- Caténaire (M12) ---
    scene::CatenaryProfile catenary_profile{};
    render::MeshId pole_mesh = 0;          // un seul poteau, instancié le long de la ligne
    render::MaterialId pole_material = 0;
    render::MaterialId wire_material = 0;
    render::MeshId catenary_mesh = 0;      // les fils : rubans, pipeline câble
    render::MeshId catenary_uploading = 0; // sas : cf. TerrainClipmap (le sol clignotait)
    WorldPosition catenary_origin{};
    WorldPosition catenary_uploading_origin{};
    render::InstanceBufferId pole_instances = 0;
    std::uint32_t pole_count = 0;
    // Attaches de gare (M19) : dans l'emprise de la verrière, elles remplacent les poteaux.
    // Même origine que les poteaux (écrites en même temps), donc on réutilise pole_origin.
    render::MeshId insulator_mesh = 0;
    render::MaterialId insulator_material = 0;
    render::InstanceBufferId insulator_instances = 0;
    std::uint32_t insulator_count = 0;
    // Origine PROPRE aux poteaux : leurs instances sont écrites tout de suite, alors que les
    // fils attendent le sas. Les deux origines divergent donc pendant un téléversement, et
    // partager `catenary_origin` décalerait les poteaux de la longueur du pas.
    WorldPosition pole_origin{};
    long catenary_snap = 0;
    bool catenary_valid = false;
    // --- Viaduc (M31) : tablier + piles, fenêtre glissante comme la caténaire ---------
    scene::ViaductProfile viaduct_profile{};
    render::MeshId viaduct_mesh = 0;
    render::MeshId viaduct_uploading = 0;  // même sas que la caténaire (jamais de trou)
    render::MaterialId viaduct_material = 0;
    WorldPosition viaduct_origin{};
    WorldPosition viaduct_uploading_origin{};
    long viaduct_snap = 0;
    bool viaduct_valid = false;
    render::MaterialId sleeper_material = 0;
    render::MaterialId ballast_material = 0;
    // Terrain : secours = le sol procédural, remplacé par le splatting dès qu'il est prêt.
    render::MaterialId terrain_material = 0;

    // Cubes de debug M4, dessinés uniquement en fallback / pendant le chargement.
    render::MeshId bogie_mesh = 0;
    render::MeshId body_mesh = 0;

    // Signalisation ATS (M30) : un mât (gris) + un panneau (teinté par aspect),
    // dessinés à chaque début de bloc. 5 matériaux, un par palier (vert → rouge).
    render::MeshId sign_mast_mesh = 0;
    render::MeshId sign_panel_mesh = 0;
    render::MaterialId sign_mast_material = 0;
    std::array<render::MaterialId, 5> tier_material{};

    bool model_ready_reported = false;
    bool sky_ready_reported = false;
    bool loading_done_reported = false;
    // Fondu d'ouverture : armé DÈS le départ (M17 — démarrage instantané), il ramène un
    // voile noir de 1 à 0 sur kFadeDuration secondes, juste pour éviter le flash sec de la
    // première frame. Il ne bloque RIEN : la scène se charge dessous en même temps.
    bool fade_active = true;
    double fade_t = 0.0;
    static constexpr double kFadeDuration = 0.4;

    // --- Écran de chargement (M9 correction) ---------------------------------
    // Tant que les assets FONDATEURS ne sont pas sur le GPU, on ne dessine RIEN (noir).
    // Sans ça, la première frame sort avec des SH à zéro et un environnement 1x1 gris :
    // la scène s'affiche sombre et sans reflets, puis « saute » brutalement ~2,6 s plus
    // tard quand le HDR arrive. Le ciel est le seul asset réellement lent (décodage +
    // rééchantillonnage cube sur un worker) ; les autres suivent.
    //
    // On ne BLOQUE pas la boucle : elle continue de tourner, de pumper les ressources et
    // de streamer la voie. On se contente de ne pas montrer un état intermédiaire.
    // On teste `sky_ready_reported` et NON `sky->ready` : c'est le drapeau posé APRÈS
    // set_environment(). Nuance décisive — pump() rend le ciel prêt en MILIEU de frame,
    // alors que la liaison se fait en TÊTE de la suivante. Se fier à sky->ready laissait
    // donc passer exactement UNE frame dessinée avec l'environnement 1x1 de secours, soit
    // le pop d'origine en miniature (observé dans les logs).
    [[nodiscard]] bool assets_ready() const {
        return sky_ready_reported &&                    // HDRI chargé ET lié (SH + IBL + skybox)
               train_model && train_model->ready &&     // le gabarit
               voiture_model && voiture_model->ready &&  // + les voitures (M16 : rame entière)
               jacobs_bogie_model && jacobs_bogie_model->ready &&  // + les bogies Jacobs
               ballast_textured &&                      // les 3 cartes du ballast
               terrain_textured &&                      // les 6 cartes du splatting
               streamer.active_chunk_count() > 0 &&     // au moins une tuile de voie
               clipmap.ready();                         // et le relief sous le train
    }

    float orbit_yaw = 3.14159f;
    float orbit_pitch = 0.30f;
    float orbit_distance = 42.0f;

    // Manipulateur japonais (M30) : traction À RAPPEL — la consigne retombe à 0 %
    // dès que la touche est lâchée (poignée à ressort / deadman). Le FREIN, lui,
    // est à CRANS persistants (0..7) : un cran engagé reste serré tant qu'on ne le
    // desserre pas (ou qu'on ne tractionne pas). Intégré au pas FIXE dans
    // update_physics, jamais à la fréquence d'affichage : le Wagon est déterministe.
    double throttle_handle = 0.0;
    int brake_notch = 0;             // crans de frein engagés (0..7)
    bool prev_throttle_down = false; // détection de front pour les crans
    bool prev_notch_release = false;
    // Caméra (M23) : Orbite (externe) ou Cabine (FPS poste de conduite), bascule sur C.
    enum class CameraMode {
        Orbit,
        Cab
    };
    CameraMode camera_mode = CameraMode::Orbit;
    bool prev_c_down = false;
    float cab_yaw = 0.0f;
    float cab_pitch = -0.18f;

    // Consignes relevées par update_input (variable), consommées par update_physics (fixe).
    bool key_throttle_up = false;
    bool key_brake = false;
    bool key_emergency = false;
    bool key_horn = false;         // sifflet (H) — maintenu
    // Enveloppe du sifflet : attaque rapide quand H est tenue, détente plus lente au
    // relâché. Évite le clic d'un volume qui saute de 0 à 1.
    float horn_level = 0.0f;

    float wetness = 0.0f;
    float wetness_target = 0.0f;
    bool prev_m_down = false;

    // --- Cycle Jour/Nuit (M21) -----------------------------------------------
    // M31 : la simulation démarre à 22 h — Neo-Tokyo se joue de nuit, fenêtres et néons
    // allumés. day_cycle_speed = rapport de compression temporelle :
    // 1 s réelle = 60 s simulées => une journée complète en ~24 min de jeu.
    double day_time = 22.0 * 3600.0;
    double day_cycle_speed = 60.0;

    // --- Phares (M21) -------------------------------------------------------
    bool headlights_on = false;
    bool prev_l_down = false;

    // --- Portes (M30.5) -----------------------------------------------------
    // door_t : 0 = fermé, 1 = ouvert. Animation en 2 secondes (0.5/s).
    // Phase 1 [0..0.3] : sortie latérale de 10 cm (bouchon).
    // Phase 2 [0.3..1] : coulissement de 60 cm par vantail (ouverture centrale).
    float door_t = 0.0f;
    bool doors_opening = false;
    bool prev_p_down = false;

    // --- Isolation ATS (M30) -----------------------------------------------
    bool prev_k_down = false;

    WorldPosition prev_cam_world{};
    bool prev_cam_valid = false;
    std::chrono::steady_clock::time_point prev_render_time;
    bool prev_render_valid = false;
    std::uint64_t telemetry_ticks = 0;

    bool initialize() {
        if (!window.initialize()) {
            return false;
        }

        render::RendererCreateInfo rc;
        rc.context.instance_extensions = window.required_instance_extensions();
        rc.context.enable_validation = config.enable_validation;
        rc.context.app_name = config.title.c_str();
        rc.context.make_surface = [this](VkInstance instance) {
            return window.create_surface(instance);
        };
        rc.get_framebuffer_size = [this]() -> VkExtent2D {
            const auto size = window.framebuffer_size();
            return VkExtent2D{size.width, size.height};
        };
        if (!renderer.initialize(rc)) {
            return false;
        }

        jobs.start(2);
        audio.initialize();  // non fatal : no-op si aucun périphérique audio

        // Sol : maillage indexé MeshVertex (pipeline texturé => il reçoit les ombres)
        // + sa texture générée à la volée. Les deux montent en GPU de façon asynchrone.
        const std::vector<unsigned char> ground_pixels = make_ground_pixels(kGroundTextureSize);
        ground_texture =
            renderer.create_texture(kGroundTextureSize, kGroundTextureSize, ground_pixels.data());
        render::MaterialDesc ground_desc;
        ground_desc.base_color = ground_texture;
        ground_desc.metallic_factor = 0.0f;   // terre/herbe : diélectrique
        ground_desc.roughness_factor = 0.95f;  // totalement mat
        ground_material = renderer.create_material(ground_desc);

        // Voie (M9) : pas de texture, la couleur vient du seul base_color_factor (le
        // secours blanc 1x1 le laisse passer tel quel).
        render::MaterialDesc rail_desc;
        rail_desc.base_color_factor = glm::vec4(kRailColor, 1.0f);
        rail_desc.metallic_factor = 1.0f;     // acier nu
        rail_desc.roughness_factor = 0.35f;   // poli par le passage des roues
        rail_material = renderer.create_material(rail_desc);

        render::MaterialDesc sleeper_desc;
        sleeper_desc.base_color_factor = glm::vec4(kSleeperColor, 1.0f);
        sleeper_desc.metallic_factor = 0.0f;   // béton : diélectrique
        sleeper_desc.roughness_factor = 0.85f;
        sleeper_material = renderer.create_material(sleeper_desc);

        // Ballast : matériau de secours SANS texture, créé tout de suite pour que la voie
        // soit dessinable dès la première tuile. Il est remplacé par la version texturée
        // dès que les 3 cartes Poly Haven sont téléversées (cf. render_frame).
        render::MaterialDesc ballast_desc;
        ballast_desc.base_color_factor = glm::vec4(kBallastColor, 1.0f);
        ballast_desc.metallic_factor = 0.0f;   // gravier : diélectrique
        ballast_desc.roughness_factor = 0.95f;  // aucun reflet spéculaire net
        ballast_material = renderer.create_material(ballast_desc);

        // Viaduc (M31) : béton clair, mat. Le tablier et les piles partagent ce matériau.
        render::MaterialDesc viaduct_desc;
        viaduct_desc.base_color_factor = glm::vec4(0.46f, 0.47f, 0.48f, 1.0f);  // béton
        viaduct_desc.metallic_factor = 0.0f;
        viaduct_desc.roughness_factor = 0.85f;
        viaduct_material = renderer.create_material(viaduct_desc);

        // --- Caténaire (M12) ---
        // Le poteau : acier galvanisé, mat et clair. `foliage` reste FAUX — il partage le
        // pipeline instancié avec la végétation, mais ni le vent ni la transmission.
        render::MaterialDesc pole_desc;
        pole_desc.base_color_factor = glm::vec4(0.52f, 0.54f, 0.56f, 1.0f);
        pole_desc.metallic_factor = 1.0f;
        pole_desc.roughness_factor = 0.62f;  // galvanisé : mat, pas un miroir
        pole_material = renderer.create_material(pole_desc);

        // Les fils : cuivre patiné pour le contact, acier pour le porteur. Un seul matériau
        // pour les trois — à 0,005 pixel de large, la nuance ne se lit pas ; ce qui se lit,
        // c'est le CONTRASTE sur le ciel.
        render::MaterialDesc wire_desc;
        wire_desc.shading = render::Shading::Wire;
        wire_desc.base_color_factor = glm::vec4(0.28f, 0.20f, 0.15f, 1.0f);  // cuivre oxydé
        wire_desc.metallic_factor = 1.0f;
        wire_desc.roughness_factor = 0.45f;
        wire_material = renderer.create_material(wire_desc);

        // M19/M31 : la gare occupe le chainage 0-400. Sous son grand toit (intrados à
        // 7,40 m), la caténaire ne plante PAS de poteaux — ils percuteraient les quais et
        // le toit ; elle s'y suspend par des attaches, le porteur abaissé sous le toit.
        catenary_profile.canopy_start = 0.0;
        catenary_profile.canopy_end = 400.0;
        catenary_profile.canopy_attach_height = 7.30;  // juste sous le toit (à 7,40 m)

        const scene::RailMeshData pole = scene::generate_pole_mesh(catenary_profile);
        pole_mesh = renderer.create_mesh_indexed(pole.vertices, pole.indices);
        log::info("M12 : poteau caténaire — {} sommets, {} triangles", pole.vertices.size(),
                  pole.indices.size() / 3);
        // Attache de gare (M19) : petite pièce instanciée, porcelaine claire et mate.
        const scene::RailMeshData ins = scene::generate_insulator_mesh(catenary_profile);
        insulator_mesh = renderer.create_mesh_indexed(ins.vertices, ins.indices);
        render::MaterialDesc ins_desc;
        ins_desc.base_color_factor = glm::vec4(0.78f, 0.78f, 0.74f, 1.0f);  // porcelaine
        ins_desc.metallic_factor = 0.0f;
        ins_desc.roughness_factor = 0.55f;
        insulator_material = renderer.create_material(ins_desc);

        bogie_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.85f, 0.15f, 0.15f)),
                                          render::Topology::Triangles);
        body_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.20f, 0.38f, 0.58f)),
                                         render::Topology::Triangles);

        // --- Signalisation ATS (M30) : mât + panneau, en géométrie MeshVertex ---------
        {
            std::vector<render::MeshVertex> mast_v, panel_v;
            std::vector<std::uint32_t> mast_i, panel_i;
            // Mât : poteau carré, du sol (2 m sous le rail) à 5 m au-dessus.
            append_box(mast_v, mast_i, glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(0.10f, 3.5f, 0.10f));
            // Panneau : plaque large déportée vers la voie (−x), face au sens de circulation.
            append_box(panel_v, panel_i, glm::vec3(-1.0f, 4.2f, 0.0f), glm::vec3(1.0f, 0.75f, 0.10f));
            sign_mast_mesh = renderer.create_mesh_indexed(mast_v, mast_i);
            sign_panel_mesh = renderer.create_mesh_indexed(panel_v, panel_i);
        }
        render::MaterialDesc mast_desc;
        mast_desc.base_color_factor = glm::vec4(0.42f, 0.44f, 0.47f, 1.0f);  // acier galvanisé
        mast_desc.metallic_factor = 0.6f;
        mast_desc.roughness_factor = 0.5f;
        sign_mast_material = renderer.create_material(mast_desc);
        // Couleur du signal par aspect ATS (M30). Couleurs VIVES et diélectriques :
        // elles se lisent comme un signal en plein jour.
        const glm::vec3 tier_colors[5] = {
            {0.80f, 0.06f, 0.05f},  // R  : rouge (arrêt)
            {0.85f, 0.62f, 0.03f},  // Y  : jaune (caution, 45)
            {0.55f, 0.72f, 0.05f},  // YG : vert-jaune (reduced, 65)
            {0.35f, 0.60f, 0.05f},  // (inutilisé)
            {0.05f, 0.58f, 0.12f},  // G  : vert (proceed, 90)
        };
        for (int t = 0; t < 5; ++t) {
            render::MaterialDesc d;
            d.base_color_factor = glm::vec4(tier_colors[t], 1.0f);
            d.metallic_factor = 0.0f;
            d.roughness_factor = 0.40f;
            tier_material[static_cast<std::size_t>(t)] = renderer.create_material(d);
        }

        // M7 : découverte du dossier assets/ puis chargement ASYNCHRONE (JobSystem) de la
        // locomotive et du roulement. Fallbacks automatiques : cubes de debug si le .glb
        // est absent, synthèse M6 si le .wav est absent — le moteur ne crashe jamais.
        asset_paths = resource::AssetPaths::discover();
        resources.set_upload_budget(2);
        // Rame de métro japonais (M30 — pivot E235) : caisse cubique à panneaux,
        // pare-brise plat en verre PBR, pupitre T-handle — tools/gen_metro.py.
        train_model = resources.load_model("models/metro_motrice.glb");
        // Voiture intermédiaire (4 doubles portes/face, banquettes longitudinales)
        // et bogie, générés par le même script.
        voiture_model = resources.load_model("models/metro_voiture.glb");
        jacobs_bogie_model = resources.load_model("models/metro_bogie.glb");
        // Gare de départ (M18) : un module répété le long de la voie sur 0-400 m.
        station_model = resources.load_model("models/station.glb");
        // Bâtiments Neo-Tokyo (M31) : trois gabarits, semés par reseed_buildings().
        const char* building_files[kBuildingVariants] = {
            "models/building_a.glb", "models/building_b.glb", "models/building_c.glb"};
        for (int v = 0; v < kBuildingVariants; ++v) {
            building_models[static_cast<std::size_t>(v)] =
                resources.load_model(building_files[v]);
        }
        rumble_clip = resources.load_audio("audio/roulement.wav");

        // Ballast (Poly Haven, CC0). L'ESPACE COLORIMÉTRIQUE est dicté par le RÔLE :
        // la base color est du sRGB, l'ARM et la normal map sont des DONNÉES — les
        // décoder fausserait rugosité et relief.
        ballast_diff = resources.load_texture("textures/ballast/gravel_diff.jpg",
                                              render::TextureFormat::SrgbColor);
        // _arm de Poly Haven = AO / Roughness / Metallic en R/G/B : c'est EXACTEMENT la
        // convention glTF metallic-roughness (G = rough, B = metal). Aucun repack requis.
        ballast_arm = resources.load_texture("textures/ballast/gravel_arm.jpg",
                                             render::TextureFormat::LinearData);
        ballast_nor = resources.load_texture("textures/ballast/gravel_nor_gl.jpg",
                                             render::TextureFormat::LinearData);

        // Terrain (Poly Haven CC0). `aerial_grass_rock` est une texture AÉRIENNE, pensée
        // pour être vue de dessus : c'est exactement notre cas. Même règle qu'ailleurs —
        // l'espace colorimétrique est dicté par le RÔLE, pas par le fichier.
        const char* terrain_files[6] = {
            "textures/terrain/grass_diff.jpg", "textures/terrain/grass_arm.jpg",
            "textures/terrain/grass_nor_gl.jpg", "textures/terrain/chalk_diff.jpg",
            "textures/terrain/chalk_arm.jpg", "textures/terrain/chalk_nor_gl.jpg"};
        for (std::size_t i = 0; i < 6; ++i) {
            terrain_maps[i] = resources.load_texture(
                terrain_files[i], i % 3 == 0 ? render::TextureFormat::SrgbColor
                                             : render::TextureFormat::LinearData);
        }
        // Ciel HDR (M8 étape 6a). 1024 par face => ~64 Mio de VRAM avec les mips, le
        // compromis retenu pour un iGPU. Absent => le fond uni est conservé.
        sky = resources.load_environment("textures/sky/kloofendal_puresky_2k.hdr", 1024);

        consist.attach(&track);
        consist.place_at(0.0);
        // NOIRE_SPEED : vitesse initiale en km/h, pour le banc. Une rame de 400 t met
        // 5 min 30 pour atteindre 320 km/h — c'est le RÉSULTAT VOULU du M13, mais ça rend
        // intenable le moindre essai à grande vitesse. Même esprit que NOIRE_STILL /
        // NOIRE_PIN_CAM / NOIRE_CREEP : un levier de mesure, jamais un défaut de jeu.
        // Départ à 20 km/h, DANS la limite de la zone de gare (30 km/h) : un vrai départ
        // sans que l'ATS ne serre l'urgence dès la première frame (M17.5). NOIRE_SPEED
        // (km/h) reste le levier de banc pour partir lancé.
        const char* speed_env = std::getenv("NOIRE_SPEED");
        consist.set_speed(speed_env != nullptr ? std::atof(speed_env) / 3.6 : 20.0 / 3.6);
        log::info("Commandes : Z=traction (rappel à 0), S/A (ou Flèches)=cran frein +/-, Espace=frein service, E=URGENCE, H=sifflet, K=ATS isolé | L=phares, P=portes, R=pluie, C=caméra (Cabine/Orbite) | souris=orbite/cabine, Ctrl/Maj=zoom, Échap=quitter");
        window.set_cursor_captured(true);

        EngineHooks hooks;
        hooks.poll_events = [this] { window.poll_events(); };
        hooks.should_stop = [this] { return window.should_close(); };
        hooks.variable_update = [this](double dt) { update_input(dt); };
        hooks.fixed_update = [this](double dt) { update_physics(dt); };
        hooks.render = [this](double /*interpolation*/) { render_frame(); };
        engine.set_hooks(std::move(hooks));

        log::info("Commandes : Z=traction (rappel à 0), S/A=cran frein +/-, "
                  "E=URGENCE, H=sifflet, K=ATS isolé | L=phares, P=portes, R=pluie | "
                  "souris=orbite, Ctrl/Maj=zoom, Échap=quitter");
        return engine.initialize();
    }

    void update_input(double dt) {
        using platform::Key;

        // On RELÈVE seulement l'état des touches ici (variable_update, cadencé sur
        // l'affichage). L'intégration du manipulateur se fait au pas fixe dans
        // update_physics. W est synonyme de Z (l'enum sert AZERTY et QWERTY),
        // Flèche Haut/Bas double Z/S. M30 : Z = traction À RAPPEL (lâché = 0 %),
        // S = cran de frein +1 (front montant), A = desserrage d'un cran.
        key_throttle_up = window.is_key_down(Key::Z) || window.is_key_down(Key::W) ||
                          window.is_key_down(Key::Up);
        const bool throttle_down_now =
            window.is_key_down(Key::S) || window.is_key_down(Key::Down);
        const bool notch_release_now = window.is_key_down(Key::A) || window.is_key_down(Key::Q);
        if (throttle_down_now && !prev_throttle_down) {
            brake_notch = std::min(7, brake_notch + 1);
        }
        if (notch_release_now && !prev_notch_release) {
            brake_notch = std::max(0, brake_notch - 1);
        }
        prev_throttle_down = throttle_down_now;
        prev_notch_release = notch_release_now;
        key_brake = window.is_key_down(Key::Space);      // frein de service (maintenu)
        key_emergency = window.is_key_down(Key::E);      // frein d'urgence
        key_horn = window.is_key_down(Key::H);           // sifflet (maintenu)
        if (window.is_key_down(Key::Escape)) {
            window.request_close();
        }

        // Pluie : bascule sur front montant de R (M21 : P libérée pour les portes).
        // M reste synonyme pour la compatibilité. La transition douce pilote brouillard,
        // adhérence ET pluie visible d'un seul curseur.
        const bool rain_down = window.is_key_down(Key::R) || window.is_key_down(Key::M);
        if (rain_down && !prev_m_down) {
            wetness_target = (wetness_target > 0.5f) ? 0.0f : 1.0f;
            log::info("Météo : {}", wetness_target > 0.5f ? "PLUIE" : "temps sec");
        }
        prev_m_down = rain_down;

        // Phares (M21) : touche L, front montant = bascule.
        const bool l_down = window.is_key_down(Key::L);
        if (l_down && !prev_l_down) {
            headlights_on = !headlights_on;
            log::info("Phares : {}", headlights_on ? "ON" : "OFF");
        }
        prev_l_down = l_down;

        // Portes (M21 + M22 + M23) : touche P, front montant = bascule ouverture/fermeture.
        // Sécurité M23 : interdiction d'ouvrir si non immobilisé (vitesse > 0.01 m/s sans clamp).
        const bool p_down = window.is_key_down(Key::P);
        if (p_down && !prev_p_down) {
            if (doors_opening || consist.immobilized() || std::abs(wagon.speed()) <= 0.01) {
                doors_opening = !doors_opening;
                log::info("Portes : {}", doors_opening ? "OUVERTURE" : "FERMETURE");
            } else {
                log::info("Portes : VERROUILLÉES (train non immobilisé)");
            }
        }
        prev_p_down = p_down;

        // ATS isolation (M30) : touche K, front montant = bascule.
        const bool k_down = window.is_key_down(Key::K);
        if (k_down && !prev_k_down) {
            const bool now_isolated = !consist.ats_isolated();
            consist.set_ats_isolated(now_isolated);
            log::info("ATS : {}", now_isolated ? "ISOLÉ (mode Arcade)" : "ACTIF");
        }
        prev_k_down = k_down;

        // Caméra (M23) : touche C, front montant = bascule entre Orbite (externe) et Cabine (FPS).
        const bool c_down = window.is_key_down(Key::C);
        if (c_down && !prev_c_down) {
            camera_mode = (camera_mode == CameraMode::Orbit) ? CameraMode::Cab : CameraMode::Orbit;
            log::info("Caméra : {}", camera_mode == CameraMode::Cab ? "CABINE (FPS)" : "EXTERNE (Orbite)");
        }
        prev_c_down = c_down;

        const float rate = static_cast<float>(dt) * 0.7f;
        wetness += glm::clamp(wetness_target - wetness, -rate, rate);

        // NOIRE_PIN_CAM : caméra verrouillée, pour les A/B à l'image. Sans elle, l'orbite
        // suit la souris, dont la position n'est PAS reproductible d'un lancement à
        // l'autre : deux runs cadrent des scènes différentes et toute comparaison entre
        // configs est fausse. Ça a invalidé une campagne de mesures entière le 2026-07-16.
        static const bool pinned = std::getenv("NOIRE_PIN_CAM") != nullptr;
        if (pinned) {
            orbit_yaw = 2.30f;
            const char* p = std::getenv("NOIRE_PITCH");
            orbit_pitch = p != nullptr ? static_cast<float>(std::atof(p)) : 0.32f;
            orbit_distance = 38.0f;
            window.consume_cursor_delta();  // vidange, sinon elle s'accumule
            return;
        }
        const platform::CursorDelta d = window.consume_cursor_delta();
        if (camera_mode == CameraMode::Orbit) {
            orbit_yaw += static_cast<float>(d.dx) * 0.005f;
            orbit_pitch = glm::clamp(orbit_pitch - static_cast<float>(d.dy) * 0.005f, -1.30f, 1.30f);
            // Zoom déplacé sur Ctrl/Maj gauche : Espace est désormais le frein de service.
            if (window.is_key_down(Key::LeftControl)) orbit_distance += static_cast<float>(25.0 * dt);
            if (window.is_key_down(Key::LeftShift)) orbit_distance -= static_cast<float>(25.0 * dt);
            orbit_distance = glm::clamp(orbit_distance, 12.0f, 200.0f);
        } else {
            // Caméra Cabine FPS (M23) : orientation relative au poste de conduite
            cab_yaw += static_cast<float>(d.dx) * 0.003f;
            cab_pitch = glm::clamp(cab_pitch - static_cast<float>(d.dy) * 0.003f, -0.75f, 0.75f);
            cab_yaw = glm::clamp(cab_yaw, -1.50f, 1.50f);
        }
    }

    void update_physics(double dt) {
        // NOIRE_STILL : gèle la physique mais laisse courir l'horloge du vent. Avec
        // NOIRE_CREEP et NOIRE_PIN_CAM, c'est le banc qui rend une mesure REPRODUCTIBLE.
        static const bool still = std::getenv("NOIRE_STILL") != nullptr;
        if (still) {
            sim_time += dt;
            return;
        }

        // Manipulateur japonais (M30) — intégré ICI, au pas FIXE, pour rester
        // déterministe. Traction À RAPPEL : Z tenue pousse la consigne (~1,2 s de
        // course), Z lâchée la ramène à 0 % (~0,7 s). Toute traction TUE les crans
        // de frein (la poignée repasse par neutre). Le frein reste serré sur ses
        // crans tant qu'on n'accélère pas : c'est ce qui tient la rame à l'arrêt.
        if (key_throttle_up) {
            brake_notch = 0;
            throttle_handle = std::min(1.0, throttle_handle + dt / 1.2);
        } else {
            throttle_handle = std::max(0.0, throttle_handle - dt / 0.7);
        }
        // Consigne de frein = max(crans / 7, frein de service maintenu).
        const double brake_demand =
            std::max(static_cast<double>(brake_notch) / 7.0, key_brake ? 1.0 : 0.0);
        consist.set_controls(throttle_handle, brake_demand, key_emergency);
        // La météo pilote l'adhérence : sec = 1, pluie battante = 0,36 (µ ~ 0,12). C'est
        // ce qui rend le patinage — hors d'atteinte à sec avec ces chiffres — possible
        // sous la pluie au-delà de ~71 % de traction.
        consist.set_adhesion_scale(static_cast<double>(glm::mix(1.0f, 0.36f, wetness)));

        consist.update(dt);

        // --- Audio ferroviaire procédural (piloté par l'état physique) ---
        const glm::vec3 train_velocity =
            glm::vec3(wagon.front_bogie().tangent()) * static_cast<float>(wagon.speed());
        const glm::dvec3 tf = glm::normalize(wagon.front_bogie().tangent());
        const glm::dvec3 tr = glm::normalize(wagon.rear_bogie().tangent());
        const double curvature =
            std::acos(glm::clamp(glm::dot(tf, tr), -1.0, 1.0)) / wagon.config().wheelbase;

        audio::RailAudio::Input in;
        in.front_chainage = wagon.front_bogie().chainage();
        in.front_position = wagon.front_bogie().position();
        in.rear_chainage = wagon.rear_bogie().chainage();
        in.rear_position = wagon.rear_bogie().position();
        in.body_position = wagon.body_position();
        in.velocity = train_velocity;
        in.speed = wagon.speed();
        in.curvature = curvature;
        rail_audio.update(audio, dt, in);

        // --- Sifflet (M14) : posé au NEZ du train, avec sa vitesse => Doppler natif ---
        // Attaque rapide (~0,08 s), détente plus lente (~0,4 s) : le son enfle et retombe
        // au lieu de claquer.
        const float horn_target = key_horn ? 1.0f : 0.0f;
        const float horn_rate = static_cast<float>(dt) * (key_horn ? 12.0f : 2.5f);
        horn_level += glm::clamp(horn_target - horn_level, -horn_rate, horn_rate);
        const glm::dvec3 fwd = glm::normalize(wagon.front_bogie().tangent());
        // Le nez est en avant du bogie avant : ~8 m suffisent à séparer clairement la
        // source du reste du train pour la spatialisation.
        const WorldPosition nose = wagon.front_bogie().position() + fwd * 8.0;
        audio.set_horn(nose, train_velocity, horn_level * 0.8f);

        sim_time += dt;
        day_time += dt * day_cycle_speed;

        // --- Portes : animation au pas fixe (déterministe) ---
        // 2 secondes pour ouvrir ou fermer (0.5/s). Intégré ici comme le throttle :
        // l'animation reste à la même vitesse quelle que soit la fréquence d'affichage.
        const float door_speed = 0.5f;
        const float door_dir = doors_opening ? 1.0f : -1.0f;
        door_t = glm::clamp(door_t + static_cast<float>(dt) * door_speed * door_dir, 0.0f, 1.0f);

        if (++telemetry_ticks % 120 == 0) {
            std::uint32_t total = 0, visible = 0;
            for (int v = 0; v < kBuildingVariants; ++v) {
                total += building_counts[static_cast<std::size_t>(v)];
                visible += visible_building_counts[static_cast<std::size_t>(v)];
            }
            const int dh = static_cast<int>(day_time / 3600.0) % 24;
            const int dm = static_cast<int>(day_time / 60.0) % 60;
            log::info("v={:6.1f} km/h | pente={:+5.1f}% | {} | chunks={} | "
                      "pluie={:.0f}% | phares={} | {:02d}:{:02d} | "
                      "{:.0f} fps (GPU {:.1f} ms) | bâtiments={}/{} vis",
                      wagon.speed() * 3.6, wagon.grade_percent(),
                      wagon.slipping() ? "PATINE   " : "adherent ", streamer.active_chunk_count(),
                      wetness * 100.0f, headlights_on ? "ON" : "OFF", dh, dm,
                      perf_fps, perf_gpu_ms, visible, total);
        }
    }

    // --- HUD (M13) ---------------------------------------------------------------
    // Le pupitre. Le Renderer sait dessiner du texte à des pixels ; ce qu'on y écrit se
    // décide ICI, avec le reste de la logique de simulation.
    //
    // Toutes les couleurs sont LINÉAIRES : la swapchain est SRGB et l'encodage est
    // matériel. Un « blanc » à 0.8 linéaire sort donc bien plus clair que 0.8 sRGB.
    [[nodiscard]] render::Hud build_hud() const {
        // Échelle ENTIÈRE (le Renderer l'arrondit de toute façon) : à 3, un glyphe fait
        // 15x21 px et sa chasse 18 px.
        constexpr float kScale = 3.0f;
        constexpr float kAdvance = 6.0f * kScale;   // (5 + 1 texel de chasse) * échelle
        constexpr float kLine = 7.0f * kScale + 8.0f;  // hauteur du glyphe + interligne
        constexpr float kPad = 14.0f;
        constexpr float kOrigin = 24.0f;

        const glm::vec4 label{0.75f, 0.78f, 0.82f, 1.0f};
        const glm::vec4 value{1.0f, 1.0f, 1.0f, 1.0f};
        const glm::vec4 alert{1.0f, 0.15f, 0.10f, 1.0f};

        const auto& brake = wagon.air_brake();
        std::vector<std::pair<std::string, glm::vec4>> lines;
        // Heure simulée (M21)
        const int sim_hour   = static_cast<int>(day_time / 3600.0) % 24;
        const int sim_minute = static_cast<int>(day_time / 60.0) % 60;
        lines.emplace_back(std::format("HEURE    {:02d}:{:02d}", sim_hour, sim_minute), label);
        lines.emplace_back(std::format("VITESSE  {: >5.0f} KM/H", wagon.speed() * 3.6), value);
        // Aspect ATS (M30) : G/YG/Y/R déduit de la limite courante, en couleur.
        const double limit = consist.current_limit_kmh();
        const bool over_limit = wagon.speed() * 3.6 > limit;
        const char* aspect = "G";
        glm::vec4 aspect_color{0.45f, 0.85f, 0.5f, 1.0f};       // G : vert
        if (limit <= 0.0) {
            aspect = "R";
            aspect_color = glm::vec4{1.0f, 0.15f, 0.10f, 1.0f}; // rouge
        } else if (limit <= 45.0) {
            aspect = "Y";
            aspect_color = glm::vec4{0.95f, 0.80f, 0.10f, 1.0f}; // jaune
        } else if (limit <= 65.0) {
            aspect = "YG";
            aspect_color = glm::vec4{0.65f, 0.90f, 0.20f, 1.0f}; // vert-jaune
        }
        lines.emplace_back(std::format("ATS {: >2}   {: >5.0f} KM/H", aspect, limit),
                           over_limit ? alert : aspect_color);
        // Position du MANIPULATEUR (traction à rappel) et crans de frein engagés.
        lines.emplace_back(std::format("TRACTION {: >5.0f} %", throttle_handle * 100.0), value);
        lines.emplace_back(std::format("FREIN    {: >5} B{}", key_brake ? "MAX" : "",
                                       brake_notch),
                           brake_notch > 0 || key_brake ? value : label);
        // CG = conduite générale. Sa pression EST l'état du frein que lit le mécanicien :
        // 5 bar = desserré, elle chute quand on serre.
        lines.emplace_back(std::format("CG      {: >5.1f} BAR", brake.pipe_pressure()),
                           brake.emergency() ? alert : value);
        lines.emplace_back(std::format("PENTE    {: >+5.1f} %", wagon.grade_percent()), label);
        // Météo (M14 → touche R) : mot d'état + intensité. Bleuté sous la pluie.
        const bool raining = wetness > 0.5f;
        const glm::vec4 meteo_color = raining ? glm::vec4{0.45f, 0.65f, 1.0f, 1.0f} : label;
        lines.emplace_back(std::format("METEO {: >4} {: >3.0f}%", raining ? "PLUIE" : "SEC",
                                       static_cast<double>(wetness) * 100.0),
                           meteo_color);
        lines.emplace_back(std::format("{:>3.0f} FPS  GPU {:.1f} MS", perf_fps, perf_gpu_ms), label);
        // M23 : Témoin vert d'immobilisation complète (clamp zéro vitesse + frein actif)
        const glm::vec4 green_notice{0.15f, 0.90f, 0.25f, 1.0f};
        if (consist.immobilized()) {
            lines.emplace_back("IMMOBILISE", green_notice);
        }

        // ATS (M30) : témoin prioritaire.
        if (consist.ats_isolated()) {
            // Clignotement : sin(t*4) > 0 => visible 50% du temps, ~2 Hz.
            // L'alpha oscille entre 0.3 et 1.0 pour un effet voyant sans disparaître.
            const float blink = 0.65f + 0.35f * std::sin(static_cast<float>(sim_time) * 8.0f);
            const glm::vec4 warn_color{0.95f, 0.80f, 0.15f, blink};
            lines.emplace_back("ATS ISOLE", warn_color);
        } else if (consist.ats_active()) {
            lines.emplace_back("ATS URGENCE", alert);
        } else if (brake.emergency()) {
            lines.emplace_back("URGENCE", alert);
        } else if (wagon.slipping()) {
            lines.emplace_back("PATINAGE", alert);
        }

        // La plaque se dimensionne sur le contenu : une taille en dur se décalerait au
        // premier libellé rallongé.
        std::size_t widest = 0;
        for (const auto& [text, color] : lines) {
            widest = std::max(widest, text.size());
        }
        const float plate_w = static_cast<float>(widest) * kAdvance + 2.0f * kPad;
        const float plate_h = static_cast<float>(lines.size()) * kLine + 2.0f * kPad - 8.0f;

        render::Hud hud;
        hud.rects.push_back({{kOrigin, kOrigin}, {plate_w, plate_h}, {0.0f, 0.0f, 0.0f, 0.45f}});
        float y = kOrigin + kPad;
        for (const auto& [text, color] : lines) {
            hud.texts.push_back({{kOrigin + kPad, y}, kScale, color, text});
            y += kLine;
        }
        return hud;
    }

    // Sème les bâtiments autour du train. Appelé quand le train change de cellule.
    // Mêmes garanties que la végétation du M11 : semis purement fonction du hash de la
    // cellule, donc déterministe et sans état. Chaque cellule produit au plus UN
    // immeuble, rangé dans la liste de sa variante (tour / immeuble / barre).
    void reseed_buildings() {
        const WorldPosition wp = wagon.body_position();
        std::array<std::vector<render::InstanceData>, kBuildingVariants> seeded;
        const auto cells = static_cast<long>(kBuildingRange / kBuildingCell) + 1;
        const long cx0 = static_cast<long>(std::floor(wp.x / kBuildingCell));
        const long cz0 = static_cast<long>(std::floor(wp.z / kBuildingCell));

        for (long ci = cx0 - cells; ci <= cx0 + cells; ++ci) {
            for (long cj = cz0 - cells; cj <= cz0 + cells; ++cj) {
                // Hash de la cellule : 4 tirages indépendants (variante, position,
                // échelle, lacet).
                const std::uint32_t h1 = hash_u32(static_cast<std::uint32_t>(ci) * 73856093u ^
                                                  static_cast<std::uint32_t>(cj) * 19349663u);
                const std::uint32_t h2 = hash_u32(h1 ^ 0x9e3779b9u);
                const std::uint32_t h3 = hash_u32(h2 ^ 0x85ebca6bu);
                const std::uint32_t h4 = hash_u32(h3 ^ 0xc2b2ae35u);
                // Densité : ~80 % des cellules sont bâties — mégalopole dense, le vide
                // entre les tours est rare (c'est lui qui donne la profondeur).
                if ((h1 & 0xffffu) > 52428u) {
                    continue;
                }
                const double jx = static_cast<double>((h2 >> 8) & 0xffffu) / 65535.0;
                const double jz = static_cast<double>((h3 >> 8) & 0xffffu) / 65535.0;
                const double wx = (static_cast<double>(ci) + jx) * kBuildingCell;
                const double wz = (static_cast<double>(cj) + jz) * kBuildingCell;

                if (std::hypot(wx - wp.x, wz - wp.z) > kBuildingRange) {
                    continue;
                }
                // EXCLUSION DU VIADUC : aucun immeuble ne traverse la ligne. Le couloir
                // est bien plus large que l'emprise du tablier (±4,6 m) : il dégage les
                // stations et laisse respirer la rue sous le viaduc.
                const double d = terrain.distance_to_track(wx, wz);
                if (d < kBuildingCorridor || std::abs(wz - wp.z) > kBuildingHalfWidth) {
                    continue;
                }

                // Variante : 20 % de tours hautes, 40 % d'immeubles, 40 % de barres.
                const std::uint32_t pick = h4 % 10u;
                const auto variant =
                    static_cast<std::size_t>(pick < 2u ? 0u : (pick < 6u ? 1u : 2u));

                render::InstanceData inst;
                // Posé sur le terrain naturel, ENFONCÉ de 4 m : jamais de jour sous la
                // semelle quand le sol ondule (la base des modèles est à y = 0).
                const double h = terrain.height(wx, wz) - 4.0;
                inst.position_scale = glm::vec4(
                    glm::vec3(glm::dvec3(wx, h, wz) - wp),
                    0.75f + 0.65f * (static_cast<float>(h3 & 0xffu) / 255.0f));
                // Lacet : quarts de tour seulement — une ville en damier. z = 0 : AUCUN
                // vent, l'instance décide (le pipeline est partagé avec la végétation).
                inst.rotation_phase =
                    glm::vec4(static_cast<float>((h4 >> 8) & 3u) * 1.5707963f, 0.0f, 0.0f, 0.0f);
                seeded[variant].push_back(inst);
            }
        }

        for (int v = 0; v < kBuildingVariants; ++v) {
            const auto i = static_cast<std::size_t>(v);
            if (building_instances[i] != 0) {
                renderer.destroy_instances(building_instances[i]);
            }
            building_instances[i] = renderer.create_instances(seeded[i]);
            building_counts[i] = static_cast<std::uint32_t>(seeded[i].size());
        }
        building_origin = wp;
        // Conserve les listes CPU : ce sont elles qu'on retestera contre le frustum chaque
        // frame (les tampons GPU persistants servent la passe d'ombres, non cullée).
        building_lists = std::move(seeded);
    }

    // Culling CPU des bâtiments (M15, adapté M31). Reteste les listes complètes contre le
    // frustum caméra et remplit `visible_buildings`. Espace CAMÉRA-RELATIF, comme tout le
    // rendu en origine flottante. ~700 tests => quelques microsecondes.
    void cull_buildings(const glm::mat4& view, const glm::mat4& proj) {
        // Hauteur et demi-diagonale de chaque gabarit (gen_building.py), pour la sphère
        // englobante : tower 22x95x22, block 30x48x30, slab 42x26x22.
        constexpr float kHeights[kBuildingVariants] = {95.0f, 48.0f, 26.0f};
        constexpr float kHalfDiag[kBuildingVariants] = {15.6f, 21.2f, 23.7f};

        // Décalage du groupe par rapport à la caméra (float, faible) : identique à la
        // translation de camera.relative_model(building_origin).
        const glm::vec3 group = glm::vec3(building_origin - camera.position());

        // Plans du frustum par Gribb-Hartmann (inchangé depuis le M15).
        const glm::mat4 clip = proj * view;
        const glm::vec4 rx{clip[0][0], clip[1][0], clip[2][0], clip[3][0]};
        const glm::vec4 ry{clip[0][1], clip[1][1], clip[2][1], clip[3][1]};
        const glm::vec4 rz{clip[0][2], clip[1][2], clip[2][2], clip[3][2]};
        const glm::vec4 rw{clip[0][3], clip[1][3], clip[2][3], clip[3][3]};
        std::array<glm::vec4, 6> planes{rw + rx, rw - rx, rw + ry, rw - ry, rw + rz, rw - rz};
        for (glm::vec4& p : planes) {
            const float len = glm::length(glm::vec3(p));
            if (len > 1e-6f) p /= len;  // normalise => la marge du rayon est en mètres
        }

        for (int v = 0; v < kBuildingVariants; ++v) {
            const auto i = static_cast<std::size_t>(v);
            visible_buildings[i].clear();
            for (const render::InstanceData& inst : building_lists[i]) {
                const glm::vec3 c = group + glm::vec3(inst.position_scale);
                // Sphère englobante : base au sol, centre remonté à mi-hauteur, rayon
                // généreux (hauteur + demi-empreinte, dans l'échelle de l'instance).
                const float scale = inst.position_scale.w;
                const glm::vec3 center = c + glm::vec3(0.0f, 0.5f * kHeights[i] * scale, 0.0f);
                const float radius = (0.55f * kHeights[i] + kHalfDiag[i]) * scale;
                bool inside = true;
                for (const glm::vec4& p : planes) {
                    if (glm::dot(glm::vec3(p), center) + p.w < -radius) {
                        inside = false;
                        break;
                    }
                }
                if (inside) {
                    visible_buildings[i].push_back(inst);
                }
            }
            visible_building_counts[i] = static_cast<std::uint32_t>(visible_buildings[i].size());
        }
    }

    // Caténaire : ré-engendrée quand le train a franchi kCatenaryStep. Les poteaux étant
    // calés sur une grille ABSOLUE de chainage, deux fenêtres successives replacent les
    // mêmes poteaux aux mêmes endroits : rien ne saute.
    void update_catenary() {
        // LE SAS, comme TerrainClipmap : create_mesh_indexed est ASYNCHRONE et le Renderer
        // saute tout maillage non prêt. Substituer tout de suite ferait DISPARAÎTRE la
        // caténaire le temps du téléversement — exactement le bug qui faisait clignoter le
        // sol tous les 16 m.
        if (catenary_uploading != 0 && renderer.is_mesh_ready(catenary_uploading)) {
            if (catenary_mesh != 0) {
                renderer.destroy_mesh(catenary_mesh);
            }
            catenary_mesh = catenary_uploading;
            catenary_origin = catenary_uploading_origin;
            catenary_uploading = 0;
            catenary_valid = true;
        }
        const long snap = std::lround(wagon.chainage() / kCatenaryStep);
        if (catenary_uploading != 0 || (catenary_valid && snap == catenary_snap)) {
            return;
        }

        const double center = static_cast<double>(snap) * kCatenaryStep;
        glm::dvec3 pos, tangent;
        track.sample(center, pos, tangent);
        const WorldPosition origin{pos};
        const scene::CatenaryData data = scene::generate_catenary(
            track, center - kCatenaryRange, center + kCatenaryRange, origin, catenary_profile);

        const render::MeshId fresh =
            renderer.create_mesh_indexed(data.wires.vertices, data.wires.indices);
        if (fresh != 0) {
            catenary_uploading = fresh;
            catenary_uploading_origin = origin;
            catenary_snap = snap;
        }
        // Les poteaux, eux, sont un tampon HOST-VISIBLE : il est écrit et lisible tout de
        // suite, sans transfert asynchrone. Aucun sas nécessaire, et la substitution ne peut
        // rien faire disparaître.
        if (pole_instances != 0) {
            renderer.destroy_instances(pole_instances);
        }
        pole_instances = renderer.create_instances(data.poles);
        pole_count = static_cast<std::uint32_t>(data.poles.size());
        pole_origin = origin;

        // Attaches de gare (M19), même chemin que les poteaux (même origine, tampon
        // host-visible immédiat). create_instances gère un vecteur vide (aucune gare en vue).
        if (insulator_instances != 0) {
            renderer.destroy_instances(insulator_instances);
            insulator_instances = 0;
        }
        insulator_count = static_cast<std::uint32_t>(data.insulators.size());
        if (insulator_count > 0) {
            insulator_instances = renderer.create_instances(data.insulators);
        }
    }

    // Viaduc (M31) : ré-engendré quand le train a franchi kViaductStep. Même sas que la
    // caténaire (create_mesh_indexed est asynchrone : on ne substitue qu'un maillage
    // prêt), mêmes grilles ABSOLUES (piles au chainage fixe : rien ne saute).
    void update_viaduct() {
        if (viaduct_uploading != 0 && renderer.is_mesh_ready(viaduct_uploading)) {
            if (viaduct_mesh != 0) {
                renderer.destroy_mesh(viaduct_mesh);
            }
            viaduct_mesh = viaduct_uploading;
            viaduct_origin = viaduct_uploading_origin;
            viaduct_uploading = 0;
            viaduct_valid = true;
        }
        const long snap = std::lround(wagon.chainage() / kViaductStep);
        if (viaduct_uploading != 0 || (viaduct_valid && snap == viaduct_snap)) {
            return;
        }

        const double center = static_cast<double>(snap) * kViaductStep;
        glm::dvec3 pos, tangent;
        track.sample(center, pos, tangent);
        const WorldPosition origin{pos};
        scene::RailMeshData deck = scene::generate_viaduct(
            track, terrain, center - kViaductRange, center + kViaductRange, origin,
            viaduct_profile);

        const render::MeshId fresh = renderer.create_mesh_indexed(deck.vertices, deck.indices);
        if (fresh != 0) {
            viaduct_uploading = fresh;
            viaduct_uploading_origin = origin;
            viaduct_snap = snap;
        }
    }

    void render_frame() {
        // Compté ICI, tout en haut : render_frame présente AUSSI pendant l'écran de
        // chargement, dont la durée varie d'un lancement à l'autre. Compter plus bas
        // décalait le glissement d'un nombre variable de frames, et deux runs identiques
        // cadraient des scènes différentes (mesuré : 7,3/255 d'écart sur le sol).
        ++present_index;

        // M7 finalisé : signale une fois la locomotive entièrement chargée et téléversée
        // (cgltf async -> ResourceManager -> GPU), remplaçant les cubes de debug.
        // Le ciel n'est lié qu'une fois sa cubemap réellement sur le GPU : d'ici là le
        // Renderer garde son nettoyage à la couleur de fond.
        if (!sky_ready_reported && sky && sky->ready) {
            renderer.set_environment(sky->id);
            log::info("M8 étape 6a : skybox HDR liée (environnement {})", sky->id);
            sky_ready_reported = true;
        }

        // Ballast texturé : dès que les 3 cartes sont sur le GPU, on bascule le matériau.
        // Un matériau ne se réécrit JAMAIS (son set serait en vol) : on en crée un neuf et
        // on change la référence. L'ancien meurt avec l'app — il est unique et minuscule.
        if (!ballast_textured && ballast_diff && ballast_diff->id != 0 && ballast_arm &&
            ballast_arm->id != 0 && ballast_nor && ballast_nor->id != 0) {
            render::MaterialDesc desc;
            desc.base_color = ballast_diff->id;
            desc.metallic_roughness = ballast_arm->id;
            desc.normal = ballast_nor->id;
            // Facteurs à 1 : on laisse les textures décider entièrement (convention glTF,
            // facteur * texture). Le gravier de Poly Haven porte déjà sa propre rugosité.
            desc.base_color_factor = glm::vec4(1.0f);
            desc.metallic_factor = 1.0f;
            desc.roughness_factor = 1.0f;
            if (const render::MaterialId textured = renderer.create_material(desc)) {
                ballast_material = textured;
                log::info("M9 : ballast texturé (gravier Poly Haven CC0, base+ARM+normale)");
            }
            ballast_textured = true;  // une seule tentative, réussie ou non
        }

        // Terrain texturé : le matériau de splatting est créé dès que les 6 cartes sont
        // sur le GPU. Comme le ballast, on ne réécrit JAMAIS un matériau (son set serait
        // en vol) : on en crée un neuf et on change la référence.
        if (!terrain_textured) {
            bool all = true;
            for (const auto& h : terrain_maps) {
                all = all && h && h->id != 0;
            }
            if (all) {
                render::TerrainMaterialDesc td;
                td.grass_base = terrain_maps[0]->id;
                td.grass_metallic_rough = terrain_maps[1]->id;
                td.grass_normal = terrain_maps[2]->id;
                td.chalk_base = terrain_maps[3]->id;
                td.chalk_metallic_rough = terrain_maps[4]->id;
                td.chalk_normal = terrain_maps[5]->id;
                if (const render::MaterialId m = renderer.create_terrain_material(td)) {
                    terrain_material = m;
                    log::info("M11 : terrain splatté (herbe + craie Poly Haven CC0)");
                }
                terrain_textured = true;
            }
        }

        if (!model_ready_reported && train_model && train_model->ready) {
            log::info("M10 : motrice E235 procédurale chargée — {} primitive(s), cubes masqués",
                      train_model->primitives.size());
            model_ready_reported = true;
        }

        // Pipeline d'assets (thread principal, 1x/frame) : injecte le CpuReady dans le
        // GPU (budget) et recycle les ressources des handles relâchés.
        resources.pump();

        // M7 étape 5 : dès que le PCM du roulement est décodé, on le branche sur
        // l'émetteur « rumble » (spatialisation + Doppler conservés). Sinon : synthé M6.
        if (!rumble_source_applied && rumble_clip && rumble_clip->ready) {
            if (audio.set_source(audio::AudioEngine::Emitter::Rumble, rumble_clip->pcm.data(),
                                 rumble_clip->pcm.size())) {
                log::info("M7 étape 5 : roulement.wav branché sur l'émetteur audio (spatialisé + Doppler)");
            }
            rumble_source_applied = true;  // une seule fois, même si l'audio est indisponible
        }

        // Streaming (thread principal), toujours exécuté (même minimisé).
        streamer.update(wagon.chainage(), renderer);
        update_catenary();
        update_viaduct();
        clipmap.update(wagon.body_position(), renderer);

        // Bâtiments : re-semés quand le train franchit une cellule. Le semis étant
        // déterministe, les immeubles déjà présents retombent EXACTEMENT au même endroit —
        // seuls ceux qui entrent ou sortent de portée changent.
        bool buildings_ready = true;
        for (const auto& m : building_models) {
            buildings_ready = buildings_ready && m && m->ready;
        }
        if (buildings_ready) {
            const WorldPosition wp = wagon.body_position();
            const long sx = static_cast<long>(std::floor(wp.x / kBuildingCell));
            const long sz = static_cast<long>(std::floor(wp.z / kBuildingCell));
            if (!building_snap_valid || sx != building_snap_x || sz != building_snap_z) {
                reseed_buildings();
                building_snap_x = sx;
                building_snap_z = sz;
                building_snap_valid = true;
            }
        }

        const auto size = window.framebuffer_size();
        if (size.width == 0 || size.height == 0) {
            window.wait_events();
            return;
        }
        if (window.was_resized()) {
            window.reset_resized();
            renderer.notify_resized();
        }

        // M17 — DÉMARRAGE INSTANTANÉ : plus d'écran de chargement bloquant. On dessine la
        // scène dès la première frame et on interagit tout de suite. Le RENDERER gère seul
        // l'état asynchrone, sans jamais lier une ressource GPU incomplète :
        //   * un matériau n'est LIÉ qu'une fois son descriptor set écrit, ce qui n'arrive
        //     qu'après le téléversement de TOUTES ses textures (sinon le DrawItem est sauté) ;
        //   * un maillage non encore téléversé n'est pas dessiné (is_mesh_ready) ;
        //   * l'environnement retombe sur une cubemap 1x1 tant que le HDRI n'est pas prêt,
        //     et la skybox n'est pas dessinée — le fond est alors la couleur de brouillard
        //     (le « ciel basique »). Le HDRI se substitue en arrière-plan à son arrivée.
        // Donc rien ne bloque, et rien ne crashe : on voit la voie et le train se compléter
        // en quelques centaines de ms au lieu d'un écran noir.
        if (assets_ready() && !loading_done_reported) {
            log::info("M17 : assets fondateurs tous prêts (le jeu tournait déjà)");
            loading_done_reported = true;
        }

        // Delta de temps réel pour la vitesse de la caméra (Doppler du listener).
        const auto now = std::chrono::steady_clock::now();
        double dt_render = 1.0 / 60.0;
        if (prev_render_valid) {
            dt_render = std::chrono::duration<double>(now - prev_render_time).count();
        }
        prev_render_time = now;
        prev_render_valid = true;
        if (dt_render < 1e-4) dt_render = 1e-4;

        // FPS et temps GPU moyens. Comptés ICI, dans le rendu : les compter dans
        // update_physics reviendrait à mesurer le pas fixe (120 Hz par construction), ce
        // qui ne dit rien du rendu. La MOYENNE, pas un échantillon : une frame isolée
        // varie du simple au double (préemption, fréquences de l'iGPU).
        {
            ++perf_frames;
            perf_gpu_sum += renderer.last_gpu_ms();
            const double el = std::chrono::duration<double>(now - perf_t0).count();
            if (el >= 1.0) {
                perf_fps = perf_frames / el;
                perf_gpu_ms = perf_gpu_sum / perf_frames;
                perf_frames = 0;
                perf_gpu_sum = 0.0;
                perf_t0 = now;
            }
        }

        WorldPosition cam_world{0.0, 0.0, 0.0};
        glm::mat4 view_mat{1.0f};

        if (camera_mode == CameraMode::Orbit) {
            camera.near_plane = 0.1f;
            // Caméra orbitale qui suit le wagon.
            const WorldPosition target = wagon.body_position();
            const glm::vec3 dir(std::cos(orbit_yaw) * std::cos(orbit_pitch), std::sin(orbit_pitch),
                                std::sin(orbit_yaw) * std::cos(orbit_pitch));
            cam_world = target + WorldPosition(dir) * static_cast<double>(orbit_distance);
            static const char* creep = std::getenv("NOIRE_CREEP");
            if (creep != nullptr) {
                cam_world.x += std::atof(creep) * static_cast<double>(present_index);
            }
            camera.set_position(cam_world);
            camera.look_at(target);
            view_mat = camera.view_matrix();
        } else {
            // Caméra Cabine FPS (M30 — métro E235) : vue dégagée à travers le pare-brise PBR.
            // near_plane réglé à 0.10 m par défaut (surchargeable via NOIRE_CAM_NEAR ou NOIRE_CAB_ZNEAR).
            static const char* env_near1 = std::getenv("NOIRE_CAM_NEAR");
            static const char* env_near2 = std::getenv("NOIRE_CAB_ZNEAR");
            const char* env_znear = (env_near1 != nullptr) ? env_near1 : env_near2;
            camera.near_plane = (env_znear != nullptr) ? static_cast<float>(std::atof(env_znear)) : 0.10f;

            static const char* env_x = std::getenv("NOIRE_CAM_X");
            static const char* env_y = std::getenv("NOIRE_CAM_Y");
            static const char* env_z = std::getenv("NOIRE_CAM_Z");

            const float cam_x = (env_x != nullptr) ? static_cast<float>(std::atof(env_x)) : 0.0f;
            const float cam_y = (env_y != nullptr) ? static_cast<float>(std::atof(env_y)) : 0.25f;
            const float cam_z = (env_z != nullptr) ? static_cast<float>(std::atof(env_z)) : -8.55f;
            const glm::vec3 driver_head_local{cam_x, cam_y, cam_z};

            const glm::mat4 loco_ori = wagon.body_orientation();
            const glm::vec3 head_world_offset = glm::vec3(loco_ori * glm::vec4(driver_head_local, 1.0f));
            cam_world = wagon.body_position() + WorldPosition(head_world_offset);
            camera.set_position(cam_world);

            const float cos_p = std::cos(cab_pitch);
            const glm::vec3 local_dir(std::sin(cab_yaw) * cos_p, std::sin(cab_pitch), -std::cos(cab_yaw) * cos_p);
            const glm::vec3 world_dir = glm::vec3(loco_ori * glm::vec4(local_dir, 0.0f));
            const glm::vec3 world_up  = glm::vec3(loco_ori * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
            view_mat = glm::lookAt(glm::vec3(0.0f), world_dir, world_up);
            camera.look_at(cam_world + WorldPosition(world_dir));
        }

        // Listener audio = caméra ; vitesse par différence finie (pour le Doppler).
        glm::vec3 cam_velocity(0.0f);
        if (prev_cam_valid) {
            cam_velocity = glm::vec3((cam_world - prev_cam_world) / dt_render);
        }
        prev_cam_world = cam_world;
        prev_cam_valid = true;
        audio.update_listener(cam_world, cam_velocity, camera.forward(), glm::vec3(0.0f, 1.0f, 0.0f));

        // Uniforms globaux : caméra + météo.
        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
        render::FrameUniforms uniforms;
        uniforms.view = view_mat;
        // Ancre le snap des cascades sur le monde (cf. update_shadow_cascades).
        uniforms.camera_world_position = camera.position();
        uniforms.proj = camera.projection_matrix(aspect);
        // Depuis le M8 étape 6b, la couleur de ciel n'est PLUS une valeur d'auteur : le
        // brouillard échantillonne la skybox elle-même. .rgb ne sert donc plus qu'au
        // nettoyage de fond (visible seulement tant que le ciel HDR n'est pas téléversé) ;
        // seule la densité (.a) pilote encore le brouillard.
        const glm::vec3 fog_dry(0.28f, 0.45f, 0.78f);
        const glm::vec3 fog_wet(0.55f, 0.57f, 0.60f);
        const glm::vec3 fog_color = glm::mix(fog_dry, fog_wet, wetness);
        // Densité BAISSÉE au M9 (0.0006 -> 0.00008) : à 0.0006, le brouillard atteignait
        // 95 % dès 5 km — les 10 km de voie chargée étaient purement invisibles. À
        // 0.00008 l'horizon reste à ~55 % de voile à 10 km : brumeux mais lisible.
        const float fog_density = glm::mix(0.00008f, 0.010f, wetness);
        uniforms.fog_color_density = glm::vec4(fog_color, fog_density);
        // y = temps : c'est l'horloge du vent (mesh_instanced.vert).
        uniforms.params = glm::vec4(wetness, static_cast<float>(sim_time), 0.0f, 0.0f);

        // Pluie (M14) : intensité = météo, temps = horloge des gouttes. L'INCLINAISON est
        // la vitesse du train projetée dans le repère VUE : la pluie penche dans le sens où
        // le paysage file à l'écran. Caméra de face (train qui s'éloigne) => pas de biais
        // latéral, la pluie tombe droite ; caméra de profil à 300 km/h => elle raye
        // l'écran presque à l'horizontale. Le signe suit l'orbite, donc c'est cohérent.
        uniforms.rain_intensity = wetness;
        uniforms.rain_time = static_cast<float>(sim_time);
        const glm::vec3 train_vel =
            glm::vec3(wagon.front_bogie().tangent()) * static_cast<float>(wagon.speed());
        const glm::vec3 vel_view = glm::vec3(uniforms.view * glm::vec4(train_vel, 0.0f));
        uniforms.rain_tilt = glm::clamp(-vel_view.x / 30.0f, -2.5f, 2.5f);

        // --- Cycle Jour/Nuit (M21) -------------------------------------------
        // L'élévation du soleil est gouvernée par day_time : 0° à midi, -90° à minuit.
        // Azimut fixe : l'axe est calé pour que l'ombre pointe dans une direction naturelle.
        // La direction HDR n'est PAS utilisée ici : elle est statique, contrairement au
        // cycle. Les SH de l'IBL, eux, restent ceux du HDR (ambiante image-based).
        const auto day_angle = static_cast<float>(
            (day_time / 86400.0) * 2.0 * glm::pi<double>() - glm::half_pi<double>());
        const glm::vec3 sun_dir_dyn =
            glm::normalize(glm::vec3(0.3f, std::sin(day_angle), 0.4f));
        uniforms.sun_direction = glm::vec4(sun_dir_dyn, 0.0f);

        // Facteur nuit : 0 en plein jour, 1 quand le soleil est sous l'horizon.
        // Le facteur 3 donne une transition crépuscule rapide mais douce (≈ 20 min sim).
        const float night_factor =
            glm::clamp(-std::sin(day_angle) * 3.0f, 0.0f, 1.0f);
        // Couleur du soleil : blanche à midi, orange au crépuscule, éteinte la nuit.
        const float dawn_factor =
            1.0f - glm::clamp(std::abs(std::sin(day_angle)) * 2.0f, 0.0f, 1.0f);
        const glm::vec3 sun_day(1.05f, 1.00f, 0.92f);
        const glm::vec3 sun_dawn(1.40f, 0.70f, 0.30f);  // orange du coucher/lever
        glm::vec3 sun_rgb =
            glm::mix(glm::mix(sun_day, sun_dawn, dawn_factor),
                     glm::vec3(0.0f), night_factor);
        // La pluie écrase le soleil (couvert). Le ciel HDR reste ; un second HDRI
        // nuageux serait nécessaire pour un vrai ciel de pluie.
        sun_rgb *= glm::mix(1.0f, 0.30f, wetness);
        uniforms.sun_color = glm::vec4(sun_rgb, 0.0f);

        // sky_params : x = nuit, y = couverture nuageuse (pluie), z = horloge nuages.
        uniforms.sky_params = glm::vec4(night_factor, wetness * 0.8f,
                                        static_cast<float>(sim_time), 0.0f);

        // SH du HDR (ambiante IBL) — inchangé
        if (sky && sky->ready) {
            for (std::size_t i = 0; i < uniforms.sh.size(); ++i) {
                uniforms.sh[i] = glm::vec4(sky->sh[i], 0.0f);
            }
        }

        // --- Phares de la rame (M21) : 2 spotlights coniques ---
        // Positions en espace CAMÉRA-RELATIF (origin-floating) : même convention que
        // tout le rendu. Direction en espace MONDE (invariant à la translation).
        if (headlights_on && train_model && train_model->ready) {
            const glm::dvec3 fwd = glm::normalize(wagon.front_bogie().tangent());
            const glm::dvec3 world_up(0.0, 1.0, 0.0);
            const glm::dvec3 right_w = glm::normalize(glm::cross(fwd, world_up));
            // Le nez est à ~8 m devant le bogie avant, phares à 2.10 m de haut, ±0.60 m.
            const WorldPosition nose_base = wagon.front_bogie().position() + fwd * 8.0;
            const glm::dvec3 h_off = world_up * 2.10;
            const glm::dvec3 lat = right_w * 0.60;
            const WorldPosition spot_pos[2] = {
                nose_base + h_off + lat,   // phare droit
                nose_base + h_off - lat,   // phare gauche
            };
            const glm::vec3 spot_dir = -glm::vec3(fwd);  // direction monde
            // Demi-angle interne 8° (faisceau de route), externe 15° (pénombre).
            const float cos_inner = std::cos(glm::radians(8.0f));
            const float cos_outer = std::cos(glm::radians(15.0f));
            for (std::size_t i = 0; i < 2; ++i) {
                const glm::vec3 rel = glm::vec3(spot_pos[i] - camera.position());
                uniforms.spot_positions[i]  = glm::vec4(rel, cos_inner);
                uniforms.spot_directions[i] = glm::vec4(spot_dir, cos_outer);
            }
            // rgb = couleur x intensité (blanc légèrement chaud), a = 1 → allumés.
            uniforms.spot_color = glm::vec4(1.05f, 1.02f, 0.96f, 1.0f);
        } else {
            // .a = 0 : le shader ignore les spots éteints (court-circuit direct).
            uniforms.spot_color = glm::vec4(0.0f);
        }

        std::vector<render::DrawItem> items;

        // Terrain (M11) : geo-clipmap centré sur le train. Il remplace le plan plat, qui
        // ne pouvait par construction ni onduler ni suivre la voie. UN seul maillage pour
        // les 7 niveaux => un seul draw call, un seul upload par régénération.
        if (clipmap.ready()) {
            items.push_back(render::DrawItem{camera.relative_model(clipmap.origin()),
                                             clipmap.mesh(),
                                             terrain_material != 0 ? terrain_material
                                                                   : ground_material});
        }

        // Voie streamée (une origine par tuile) : 3 sous-maillages, 3 matériaux (M9).
        for (const scene::ChunkRenderInfo& chunk : streamer.renderables()) {
            const glm::mat4 model = camera.relative_model(chunk.origin);
            const std::pair<render::MeshId, render::MaterialId> parts[3] = {
                {chunk.ballast, ballast_material},
                {chunk.sleepers, sleeper_material},
                {chunk.rails, rail_material},
            };
            for (const auto& [mesh, material] : parts) {
                if (mesh != 0) {
                    items.push_back(render::DrawItem{model, mesh, material});
                }
            }
        }

        // Bâtiments (M31), dessinés APRÈS le terrain et la voie : leur fragment peut
        // faire `discard` (pipeline instancié partagé avec le feuillage), ce qui INTERDIT
        // l'early-z — en dernier, au moins le depth-test rejette ce que le sol cache.
        {
            // Frustum culling CPU : la PASSE PRINCIPALE ne dessine que le sous-ensemble
            // visible ; la passe d'ombres reçoit les tampons complets (une tour hors
            // champ projette encore). NOIRE_NOCULL : désactive le culling (banc).
            static const bool nocull = std::getenv("NOIRE_NOCULL") != nullptr;
            if (!nocull) {
                cull_buildings(uniforms.view, uniforms.proj);
            } else {
                visible_building_counts = building_counts;
            }
            const glm::mat4 group = camera.relative_model(building_origin);
            for (int v = 0; v < kBuildingVariants; ++v) {
                const auto i = static_cast<std::size_t>(v);
                if (!building_models[i] || !building_models[i]->ready ||
                    building_instances[i] == 0 || building_counts[i] == 0) {
                    continue;
                }
                for (const resource::Model::Primitive& prim : building_models[i]->primitives) {
                    render::DrawItem item;
                    item.model = group;
                    item.mesh = prim.mesh;
                    item.material = prim.material ? prim.material->id : 0;
                    item.instances = building_instances[i];   // complet => ombres
                    item.instance_count = building_counts[i];
                    item.cpu_instances = nocull ? nullptr : &visible_buildings[i];
                    items.push_back(item);
                }
            }
        }

        // Viaduc (M31) : tablier + piles, opaque, avec la voie (il la porte).
        if (viaduct_mesh != 0) {
            items.push_back(render::DrawItem{camera.relative_model(viaduct_origin),
                                             viaduct_mesh, viaduct_material});
        }

        // Poteaux caténaire (M12) : OPAQUES, donc avec le reste. Un seul draw call pour
        // toute la ligne visible — même mât, seuls sa position et son lacet changent.
        if (pole_mesh != 0 && pole_instances != 0 && pole_count > 0) {
            render::DrawItem item;
            item.model = camera.relative_model(pole_origin);
            item.mesh = pole_mesh;
            item.material = pole_material;
            item.instances = pole_instances;
            item.instance_count = pole_count;
            items.push_back(item);
        }
        // Attaches de gare (M19) : là où les poteaux ont été supprimés (0-400 m), instanciées
        // à la même origine que les poteaux.
        if (insulator_mesh != 0 && insulator_instances != 0 && insulator_count > 0) {
            render::DrawItem item;
            item.model = camera.relative_model(pole_origin);
            item.mesh = insulator_mesh;
            item.material = insulator_material;
            item.instances = insulator_instances;
            item.instance_count = insulator_count;
            items.push_back(item);
        }

        // Locomotive (M17.6) : la CAISSE suit la suspension (position + orientation de la
        // physique multi-corps), mais SES BOGIES sont dessinés SÉPARÉMENT — plaqués sur la
        // voie, sans le moindre tangage de caisse. C'est la HIÉRARCHIE ferroviaire correcte :
        // le bogie roule sur le rail, la caisse flotte au-dessus sur ses ressorts.
        // M30.5 : comme pour les voitures, les 16 DERNIÈRES primitives de la motrice sont
        // les battants de porte (même convention, même animation).
        constexpr int kDoorPairs = 8;  // 4 doubles portes par face
        const float door_t1 = glm::clamp(door_t / 0.3f, 0.0f, 1.0f);
        const float door_t2 = glm::clamp((door_t - 0.3f) / 0.7f, 0.0f, 1.0f);
        auto push_car = [&](const resource::ModelHandle& model, const glm::mat4& caisse_m) {
            const auto& prims = model->primitives;
            const int n_prims = static_cast<int>(prims.size());
            const bool has_doors = n_prims >= 2 * kDoorPairs;
            const auto body_count = static_cast<std::size_t>(
                has_doors ? n_prims - 2 * kDoorPairs : n_prims);
            for (std::size_t j = 0; j < body_count; ++j) {
                const render::MaterialId mat = prims[j].material ? prims[j].material->id : 0;
                items.push_back(render::DrawItem{caisse_m, prims[j].mesh, mat});
            }
            if (has_doors) {
                for (int d = 0; d < kDoorPairs; ++d) {
                    const float sx = (d < 4) ? +1.0f : -1.0f;  // flanc droit / gauche
                    for (int leaf = 0; leaf < 2; ++leaf) {
                        const float sz = (leaf == 0) ? -1.0f : +1.0f;  // A : -z, B : +z
                        const glm::mat4 door_local = glm::translate(
                            glm::mat4(1.0f),
                            glm::vec3(sx * 0.10f * door_t1, 0.0f, sz * 0.60f * door_t2));
                        const auto prim_idx = static_cast<std::size_t>(
                            n_prims - 2 * kDoorPairs + 2 * d + leaf);
                        const render::MaterialId mat =
                            prims[prim_idx].material ? prims[prim_idx].material->id : 0;
                        items.push_back(
                            render::DrawItem{caisse_m * door_local, prims[prim_idx].mesh, mat});
                    }
                }
            }
        };
        if (train_model && train_model->ready) {
            const glm::mat4 caisse_m = camera.relative_model(wagon.body_position()) *
                                       wagon.body_orientation() * kLocoTransform.matrix();
            push_car(train_model, caisse_m);
            // Helper pour dessiner un bogie (châssis + 2 essieux animés en rotation)
            auto draw_bogie = [&](const physics::Bogie& b) {
                const glm::mat4 m = camera.relative_model(b.position()) * b.orientation() *
                                    kBogieTransform.matrix();
                const auto& prims = jacobs_bogie_model->primitives;
                const int n_prims = static_cast<int>(prims.size());
                const auto body_count = static_cast<std::size_t>((n_prims >= 2) ? n_prims - 2 : n_prims);
                for (std::size_t j = 0; j < body_count; ++j) {
                    const render::MaterialId mat = prims[j].material ? prims[j].material->id : 0;
                    items.push_back(render::DrawItem{m, prims[j].mesh, mat});
                }
                // Essieux animés en rotation Pitch (v = ω · r)
                if (n_prims >= 2) {
                    constexpr float kAxleZ[2] = {-1.5f, 1.5f};
                    constexpr float kWheelR = 0.46f;
                    for (int axle = 0; axle < 2; ++axle) {
                        const auto prim_idx = static_cast<std::size_t>(n_prims - 2 + axle);
                        const render::MaterialId mat = prims[prim_idx].material ? prims[prim_idx].material->id : 0;
                        const glm::mat4 axle_local =
                            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, kWheelR, kAxleZ[axle])) *
                            glm::rotate(glm::mat4(1.0f), b.wheel_angle(), glm::vec3(1.0f, 0.0f, 0.0f));
                        items.push_back(render::DrawItem{m * axle_local, prims[prim_idx].mesh, mat});
                    }
                }
            };

            // Les 2 bogies moteur, à l'orientation de la VOIE (jamais celle de la caisse).
            // Même modèle que les bogies Jacobs (empattement 3 m identique).
            if (jacobs_bogie_model && jacobs_bogie_model->ready) {
                for (const physics::Bogie* b : {&wagon.front_bogie(), &wagon.rear_bogie()}) {
                    draw_bogie(*b);
                }
            }
        } else {
            // Fallback / pendant le chargement : cubes de debug M4 (2 bogies + caisse),
            // masqués définitivement une fois la locomotive prête.
            const glm::vec3 bogie_scale(2.2f, 0.7f, 2.6f);
            for (const physics::Bogie* b : {&wagon.front_bogie(), &wagon.rear_bogie()}) {
                const WorldPosition p = b->position() + WorldPosition{0.0, 0.4, 0.0};
                const glm::mat4 model = camera.relative_model(p) * b->orientation() *
                                        glm::scale(glm::mat4(1.0f), bogie_scale);
                items.push_back(render::DrawItem{model, bogie_mesh});
            }
            const glm::mat4 body_model = camera.relative_model(wagon.body_position()) *
                                         wagon.body_orientation() *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(2.8f, 2.6f, 18.0f));
            items.push_back(render::DrawItem{body_model, body_mesh});
        }

        // Voitures (M30 — métro E235) : chaque caisse est posée par la cinématique
        // inverse de ses bogies Jacobs. Même convention portes que la motrice
        // (16 dernières primitives = battants), même animation.
        if (voiture_model && voiture_model->ready) {
            for (int i = 0; i < consist.car_count(); ++i) {
                const physics::CarBody& car = consist.car(i);
                const glm::mat4 caisse_m = camera.relative_model(car.position()) *
                                           car.orientation() * kCarTransform.matrix();
                push_car(voiture_model, caisse_m);
            }
        }
        // Bogies Jacobs (M16) : l'organe PARTAGÉ, dessiné une fois à chaque articulation.
        if (jacobs_bogie_model && jacobs_bogie_model->ready) {
            auto draw_bogie = [&](const physics::Bogie& b) {
                const glm::mat4 m = camera.relative_model(b.position()) * b.orientation() *
                                    kBogieTransform.matrix();
                const auto& prims = jacobs_bogie_model->primitives;
                const int n_prims = static_cast<int>(prims.size());
                const auto body_count = static_cast<std::size_t>((n_prims >= 2) ? n_prims - 2 : n_prims);
                for (std::size_t j = 0; j < body_count; ++j) {
                    const render::MaterialId mat = prims[j].material ? prims[j].material->id : 0;
                    items.push_back(render::DrawItem{m, prims[j].mesh, mat});
                }
                if (n_prims >= 2) {
                    constexpr float kAxleZ[2] = {-1.5f, 1.5f};
                    constexpr float kWheelR = 0.46f;
                    for (int axle = 0; axle < 2; ++axle) {
                        const auto prim_idx = static_cast<std::size_t>(n_prims - 2 + axle);
                        const render::MaterialId mat = prims[prim_idx].material ? prims[prim_idx].material->id : 0;
                        const glm::mat4 axle_local =
                            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, kWheelR, kAxleZ[axle])) *
                            glm::rotate(glm::mat4(1.0f), b.wheel_angle(), glm::vec3(1.0f, 0.0f, 0.0f));
                        items.push_back(render::DrawItem{m * axle_local, prims[prim_idx].mesh, mat});
                    }
                }
            };
            for (const physics::Bogie& b : consist.jacobs_bogies()) {
                draw_bogie(b);
            }
        }

        // Signaux ATS (M30) : un panneau à chaque DÉBUT DE BLOC dans une
        // fenêtre autour du train. Le panneau marque le début d'une zone de vitesse et sa
        // couleur en donne la sévérité — le conducteur le voit venir et anticipe. Même
        // source de vérité que l'ATS (consist.speed_limits()) : ils ne peuvent pas mentir.
        if (sign_mast_mesh != 0 && sign_panel_mesh != 0) {
            const auto& limits = consist.speed_limits();
            const double x_train = wagon.chainage();
            const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
            // Un panneau par ZONE, planté au chainage EXACT de la transition (M17.5). On ne
            // dessine que ceux dans une fenêtre autour du train (visibles de loin devant,
            // gardés un peu derrière).
            for (int z = 0; z < limits.zone_count(); ++z) {
                const double xs = limits.zone_start(z);
                if (xs < x_train - 800.0 || xs > x_train + 12000.0) {
                    continue;
                }
                glm::dvec3 pos, tangent;
                track.sample(xs, pos, tangent);
                const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
                const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
                const glm::vec3 up = glm::cross(right, forward);
                // Le panneau se dresse à 8 m à DROITE de l'axe, orienté comme la voie.
                const WorldPosition sign_pos = pos + glm::dvec3(right) * 8.0;
                glm::mat4 basis(1.0f);
                basis[0] = glm::vec4(right, 0.0f);
                basis[1] = glm::vec4(up, 0.0f);
                basis[2] = glm::vec4(-forward, 0.0f);
                const glm::mat4 m = camera.relative_model(sign_pos) * basis;
                const int tier = limits.tier_for_limit(limits.zone_limit(z));
                items.push_back(render::DrawItem{m, sign_mast_mesh, sign_mast_material});
                items.push_back(
                    render::DrawItem{m, sign_panel_mesh, tier_material[static_cast<std::size_t>(tier)]});
            }
        }

        // Gares (M30 — métro de Tokyo) : une gare tous les 1 200 m, calée sur le
        // profil ATS (quai = les 300 premiers mètres du cycle, cf. speed_limits.cpp).
        // Le module est RÉPÉTÉ le long du quai, posé à chaque fois sur la spline avec
        // l'orientation de la voie — il épouse donc la courbe et la pente.
        if (station_model && station_model->ready) {
            const double x_train = wagon.chainage();
            const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
            constexpr double kStationModule = 40.0;
            constexpr int kStationModules = 8;      // 8 x 40 m = quai de 320 m
            constexpr double kStationSpacing = 1200.0;  // = profil ATS
            const int k_first =
                static_cast<int>(std::max(0.0, std::floor((x_train - 600.0) / kStationSpacing)));
            const int k_last = static_cast<int>((x_train + 4000.0) / kStationSpacing);
            for (int k = k_first; k <= k_last; ++k) {
                for (int i = 0; i < kStationModules; ++i) {
                    const double xc = k * kStationSpacing +
                                      kStationModule * (static_cast<double>(i) + 0.5);
                    glm::dvec3 pos, tangent;
                    track.sample(xc, pos, tangent);
                    const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
                    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
                    const glm::vec3 up = glm::cross(right, forward);
                    glm::mat4 basis(1.0f);
                    basis[0] = glm::vec4(right, 0.0f);
                    basis[1] = glm::vec4(up, 0.0f);
                    basis[2] = glm::vec4(-forward, 0.0f);
                    const glm::mat4 m = camera.relative_model(pos) * basis;
                    for (const resource::Model::Primitive& prim : station_model->primitives) {
                        const render::MaterialId mat = prim.material ? prim.material->id : 0;
                        items.push_back(render::DrawItem{m, prim.mesh, mat});
                    }
                }
            }
        }

        // LES CÂBLES EN DERNIER, et ce n'est pas un détail : ils sont MÉLANGÉS et n'écrivent
        // pas la profondeur. Tout ce qui est opaque doit donc déjà avoir posé la sienne,
        // sans quoi un fil se peindrait par-dessus un talus qui le cache.
        if (catenary_mesh != 0) {
            items.push_back(render::DrawItem{camera.relative_model(catenary_origin),
                                             catenary_mesh, wire_material});
        }

        render::Hud hud = build_hud();
        // Fondu d'ouverture : le voile noir tombe de 1 à 0 sur kFadeDuration secondes.
        if (fade_active) {
            fade_t += dt_render;
            const float a = 1.0f - static_cast<float>(fade_t / kFadeDuration);
            hud.fade = glm::clamp(a, 0.0f, 1.0f);
            if (a <= 0.0f) {
                fade_active = false;
            }
        }
        renderer.draw_frame(uniforms, items, hud);
    }

    void shutdown() {
        jobs.stop();          // draine les workers avant toute destruction
        train_model.reset();  // relâche le handle ; renderer.shutdown() détruit le GPU restant
        voiture_model.reset();
        jacobs_bogie_model.reset();
        station_model.reset();
        for (auto& m : building_models) {
            m.reset();
        }
        audio.shutdown();
        renderer.wait_idle();
        renderer.shutdown();
        window.shutdown();
        engine.shutdown();
    }
};

Application::Application(ApplicationConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Application::~Application() = default;

int Application::run() {
    if (!impl_->initialize()) {
        log::error("Application : échec de l'initialisation");
        impl_->shutdown();
        return 1;
    }
    impl_->engine.run();
    impl_->shutdown();
    return 0;
}

}  // namespace noire
