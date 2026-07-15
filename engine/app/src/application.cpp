#include "noire/app/application.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
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
#include "noire/physics/wagon.hpp"
#include "noire/platform/window.hpp"
#include "noire/render/renderer.hpp"
#include "noire/render/vertex.hpp"
#include "noire/resource/asset_paths.hpp"
#include "noire/resource/resource_manager.hpp"
#include "noire/scene/track_mesh.hpp"
#include "noire/scene/terrain_clipmap.hpp"
#include "noire/scene/world_streamer.hpp"

namespace noire {

namespace {

constexpr WorldPosition kTrackOrigin{1000000.0, 0.0, 1000000.0};

physics::WagonConfig make_loco_config() {
    physics::WagonConfig c;
    c.mass = 80000.0;
    c.wheelbase = 14.0;
    c.max_tractive_effort = 300000.0;
    c.max_brake_force = 350000.0;
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

// --- Sol (M8 étape 2) -------------------------------------------------------
// Grand plan solide texturé, qui remplace la grille filaire du M2 : c'est lui qui
// reçoit l'ombre portée du train.

// Doit couvrir le plan lointain de la caméra (10 km depuis le M9) dans toutes les
// directions, sinon le vide apparaît au-delà.
constexpr float kGroundHalfExtent = 12000.0f;
// Altitude ABSOLUE du plan de sol. Calée sous le point le plus BAS de la spline
// (amplitude verticale 6 m) moins la profondeur du ballast : la voie est donc toujours
// en remblai au-dessus du sol, jamais en tranchée — un simple quad ne saurait pas se
// percer d'un trou. AVANT le M9, le sol suivait le train (body_y - 3.0) : il divergeait
// de la voie jusqu'à 12 m au loin, d'où les rails qui flottaient ou s'enterraient.
constexpr double kGroundLevel = -6.8;
// Période de répétition de la texture, en mètres. C'est aussi le pas de recentrage du
// sol, pour que les UV ne glissent pas sous le train. (Sa valeur était grande faute de
// mipmaps ; le M9 les a activées, elle pourrait donc être réduite.)
constexpr float kGroundUvPeriod = 20.0f;
constexpr std::uint32_t kGroundTextureSize = 256;

// Couleurs de la voie, portées par le base_color_factor de leur matériau (M8 étape 3).
// Valeurs LINÉAIRES (la conversion sRGB est matérielle).
constexpr glm::vec3 kRailColor{0.55f, 0.57f, 0.62f};      // acier poli par les roues
constexpr glm::vec3 kSleeperColor{0.28f, 0.27f, 0.25f};   // béton gris, un peu sale
constexpr glm::vec3 kBallastColor{0.20f, 0.19f, 0.17f};   // gravier gris sombre

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
    float rotation_y = 0.0f;  // radians, autour de la verticale : oriente l'avant du modèle
    // Ordre VOULU : rotation d'abord, translation ensuite. L'inverse ferait décrire un
    // arc à l'offset au lieu de rester vertical.
    [[nodiscard]] glm::mat4 matrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, offset_y, 0.0f));
        m = glm::rotate(m, rotation_y, glm::vec3(0.0f, 1.0f, 0.0f));
        return glm::scale(m, glm::vec3(scale));
    }
};

// Calibrage de la motrice (tools/gen_tgv_procedural.py). Le modèle est généré DANS le
// repère caisse et en mètres : échelle 1, rotation nulle.
// offset_y = 0 : le script pose le bas des roues exactement sur y = -2.20 = -body_height,
// c'est-à-dire sur le plan de roulement (vérifié : bbox acier y[-2.20, -1.15]). Un modèle
// importé, lui, aura presque toujours besoin des trois champs.
constexpr ModelTransform kLocoTransform{1.0f, 0.0f, 0.0f};

void make_ground_plane(std::vector<render::MeshVertex>& vertices,
                       std::vector<std::uint32_t>& indices) {
    const float e = kGroundHalfExtent;
    const float t = e / kGroundUvPeriod;
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    // u croît selon +x, v selon +z. La bitangente reconstruite par cross(N,T)*w doit
    // donc pointer vers +z : cross(up, +x) = -z, d'où w = -1.
    const glm::vec4 tangent{1.0f, 0.0f, 0.0f, -1.0f};
    vertices = {
        render::MeshVertex{{-e, 0.0f, -e}, up, {-t, -t}, tangent},
        render::MeshVertex{{e, 0.0f, -e}, up, {t, -t}, tangent},
        render::MeshVertex{{e, 0.0f, e}, up, {t, t}, tangent},
        render::MeshVertex{{-e, 0.0f, e}, up, {-t, t}, tangent},
    };
    indices = {0, 1, 2, 2, 3, 0};
}

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

// Sol herbeux/terreux : 2 octaves de bruit lissé, généré à la volée (aucun asset).
// La texture est RGBA8 SRGB => les octets écrits ici sont des valeurs sRGB, que le
// matériel reconvertit en linéaire à l'échantillonnage.
std::vector<unsigned char> make_ground_pixels(std::uint32_t size) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4u);
    const glm::vec3 dark{0.42f, 0.46f, 0.30f};
    const glm::vec3 light{0.60f, 0.64f, 0.44f};
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
          wagon(make_loco_config()),
          streamer(track, jobs, make_streamer_config()),
          clipmap(terrain, jobs, make_clipmap_config()),
          resources(renderer, jobs, asset_paths) {}

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
    physics::Wagon wagon;
    scene::WorldStreamer streamer;
    scene::TerrainClipmap clipmap;

    audio::AudioEngine audio;
    audio::RailAudio rail_audio;

    // Pipeline d'assets (M7 étapes 4-5) : racine assets/ + cache/loaders asynchrones.
    resource::AssetPaths asset_paths;
    resource::ResourceManager resources;
    resource::ModelHandle train_model;
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
    bool rumble_source_applied = false;

    // Sol : plan solide texturé (M8 étape 2), il reçoit l'ombre portée du train.
    render::MeshId ground_mesh = 0;
    render::TextureId ground_texture = 0;
    render::MaterialId ground_material = 0;
    // Voie : trois matériaux partagés par toutes les tuiles (M9). Sans texture pour
    // l'instant — les UV sont générés et à l'échelle physique, prêts à en recevoir.
    render::MaterialId rail_material = 0;
    render::MaterialId sleeper_material = 0;
    render::MaterialId ballast_material = 0;
    // Terrain : secours = le sol procédural, remplacé par le splatting dès qu'il est prêt.
    render::MaterialId terrain_material = 0;

    // Cubes de debug M4, dessinés uniquement en fallback / pendant le chargement.
    render::MeshId bogie_mesh = 0;
    render::MeshId body_mesh = 0;

    bool model_ready_reported = false;
    bool sky_ready_reported = false;
    bool loading_done_reported = false;

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
               ballast_textured &&                      // les 3 cartes du ballast
               terrain_textured &&                      // les 6 cartes du splatting
               streamer.active_chunk_count() > 0 &&     // au moins une tuile de voie
               clipmap.ready();                         // et le relief sous le train
    }

    float orbit_yaw = 3.14159f;
    float orbit_pitch = 0.30f;
    float orbit_distance = 42.0f;

    float wetness = 0.0f;
    float wetness_target = 0.0f;
    bool prev_m_down = false;

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
        std::vector<render::MeshVertex> ground_vertices;
        std::vector<std::uint32_t> ground_indices;
        make_ground_plane(ground_vertices, ground_indices);
        ground_mesh = renderer.create_mesh_indexed(ground_vertices, ground_indices);
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

        bogie_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.85f, 0.15f, 0.15f)),
                                          render::Topology::Triangles);
        body_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.20f, 0.38f, 0.58f)),
                                         render::Topology::Triangles);

        // M7 : découverte du dossier assets/ puis chargement ASYNCHRONE (JobSystem) de la
        // locomotive et du roulement. Fallbacks automatiques : cubes de debug si le .glb
        // est absent, synthèse M6 si le .wav est absent — le moteur ne crashe jamais.
        asset_paths = resource::AssetPaths::discover();
        resources.set_upload_budget(2);
        // Motrice TGV procédurale (M10) : carrosserie loftée (superellipse + Béziers),
        // nez plongeant, vraies roues cylindriques. Remplace le gabarit-boîtes du M9.
        train_model = resources.load_model("models/tgv_procedural.glb");
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

        wagon.attach(&track);
        wagon.place_at(0.0);
        wagon.set_speed(25.0);
        window.set_cursor_captured(true);

        EngineHooks hooks;
        hooks.poll_events = [this] { window.poll_events(); };
        hooks.should_stop = [this] { return window.should_close(); };
        hooks.variable_update = [this](double dt) { update_input(dt); };
        hooks.fixed_update = [this](double dt) { update_physics(dt); };
        hooks.render = [this](double /*interpolation*/) { render_frame(); };
        engine.set_hooks(std::move(hooks));

        log::info("Commandes : W/Z=traction, S=frein, M=pluie | souris=orbite, Espace/Maj=zoom, Échap=quitter");
        return engine.initialize();
    }

    void update_input(double dt) {
        using platform::Key;

        wagon.set_controls((window.is_key_down(Key::W) || window.is_key_down(Key::Z)) ? 1.0 : 0.0,
                           window.is_key_down(Key::S) ? 1.0 : 0.0);
        if (window.is_key_down(Key::Escape)) {
            window.request_close();
        }

        // Pluie : bascule sur front montant de M, puis transition douce du wetness.
        const bool m_down = window.is_key_down(Key::M);
        if (m_down && !prev_m_down) {
            wetness_target = (wetness_target > 0.5f) ? 0.0f : 1.0f;
            log::info("Météo : {}", wetness_target > 0.5f ? "PLUIE" : "temps sec");
        }
        prev_m_down = m_down;
        const float rate = static_cast<float>(dt) * 0.7f;
        wetness += glm::clamp(wetness_target - wetness, -rate, rate);

        const platform::CursorDelta d = window.consume_cursor_delta();
        orbit_yaw += static_cast<float>(d.dx) * 0.005f;
        orbit_pitch = glm::clamp(orbit_pitch - static_cast<float>(d.dy) * 0.005f, -1.30f, 1.30f);
        if (window.is_key_down(Key::Space)) orbit_distance += static_cast<float>(25.0 * dt);
        if (window.is_key_down(Key::LeftShift)) orbit_distance -= static_cast<float>(25.0 * dt);
        orbit_distance = glm::clamp(orbit_distance, 12.0f, 200.0f);
    }

    void update_physics(double dt) {
        wagon.update(dt);

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

        if (++telemetry_ticks % 120 == 0) {
            log::info("v={:6.1f} km/h | pente={:+5.1f}% | {} | chunks={} | pluie={:.0f}%",
                      wagon.speed() * 3.6, wagon.grade_percent(),
                      wagon.slipping() ? "PATINE   " : "adherent ", streamer.active_chunk_count(),
                      wetness * 100.0f);
        }
    }

    void render_frame() {
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
            log::info("M10 : motrice TGV procédurale chargée — {} primitive(s), cubes masqués",
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
        clipmap.update(wagon.body_position(), renderer);

        const auto size = window.framebuffer_size();
        if (size.width == 0 || size.height == 0) {
            window.wait_events();
            return;
        }
        if (window.was_resized()) {
            window.reset_resized();
            renderer.notify_resized();
        }

        // Écran de chargement : noir tant que les assets fondateurs ne sont pas prêts.
        // Placé APRÈS resources.pump() et streamer.update() — sinon rien ne progresserait
        // et on n'en sortirait jamais. Le premier pixel affiché est donc déjà l'image
        // finale : plus de « pop » de lumière.
        if (!assets_ready()) {
            render::FrameUniforms black;
            black.view = camera.view_matrix();
            black.proj = camera.projection_matrix(static_cast<float>(size.width) /
                                                  static_cast<float>(size.height));
            black.fog_color_density = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);  // fond noir
            renderer.draw_frame(black, {});
            return;
        }
        if (!loading_done_reported) {
            log::info("M9 : assets fondateurs prêts — première frame complète");
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

        // Caméra orbitale qui suit le wagon.
        const WorldPosition target = wagon.body_position();
        const glm::vec3 dir(std::cos(orbit_yaw) * std::cos(orbit_pitch), std::sin(orbit_pitch),
                            std::sin(orbit_yaw) * std::cos(orbit_pitch));
        const WorldPosition cam_world = target + WorldPosition(dir) * static_cast<double>(orbit_distance);
        camera.set_position(cam_world);
        camera.look_at(target);

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
        uniforms.view = camera.view_matrix();
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
        uniforms.params = glm::vec4(wetness, 0.0f, 0.0f, 0.0f);

        // Soleil : direction VERS l'astre, source de vérité unique (elle éclaire les
        // modèles ET cadre les cascades d'ombre). Depuis l'étape 6b elle est EXTRAITE du
        // ciel HDR : c'est ce qui garantit que l'ombre portée pointe dans le sens du
        // soleil qu'on voit réellement dans le ciel. Valeur en dur = secours tant que le
        // HDR n'est pas chargé (~2,6 s au démarrage).
        uniforms.sun_direction = glm::vec4(glm::normalize(glm::vec3(-0.18f, 0.77f, 0.61f)), 0.0f);
        glm::vec3 sun_rgb(1.05f, 1.00f, 0.92f);
        if (sky && sky->ready) {
            uniforms.sun_direction = glm::vec4(sky->sun_direction, 0.0f);
            sun_rgb = sky->sun_color;
            // Irradiance du ciel (soleil déjà retiré côté loader : le laisser dans les SH
            // le compterait deux fois, une fois ici et une fois en directionnel).
            for (std::size_t i = 0; i < uniforms.sh.size(); ++i) {
                uniforms.sh[i] = glm::vec4(sky->sh[i], 0.0f);
            }
        }
        // La pluie écrase le soleil (couvert). Le ciel, lui, reste celui du HDR : un vrai
        // ciel de pluie demandera un second HDRI en fondu.
        uniforms.sun_color = glm::vec4(sun_rgb * glm::mix(1.0f, 0.30f, wetness), 0.0f);

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

        // Locomotive : dessinée avec le transform EXACT de la caisse — position,
        // orientation, pitch et heave issus de la physique multi-corps (M4) — suivi du
        // CALIBRAGE du modèle (M9). Ce dernier facteur est le point d'insertion pour un
        // vrai tgv.glb : régler kLocoTransform suffit à le mettre à l'échelle, le
        // retourner et poser ses roues sur le rail, sans toucher ni la physique ni l'asset.
        if (train_model && train_model->ready) {
            const glm::mat4 loco = camera.relative_model(wagon.body_position()) *
                                   wagon.body_orientation() * kLocoTransform.matrix();
            for (const resource::Model::Primitive& prim : train_model->primitives) {
                const render::MaterialId mat = prim.material ? prim.material->id : 0;
                items.push_back(render::DrawItem{loco, prim.mesh, mat});
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

        renderer.draw_frame(uniforms, items);
    }

    void shutdown() {
        jobs.stop();          // draine les workers avant toute destruction
        train_model.reset();  // relâche le handle ; renderer.shutdown() détruit le GPU restant
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
