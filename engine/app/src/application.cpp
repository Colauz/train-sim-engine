#include "noire/app/application.hpp"

#include <chrono>
#include <cmath>
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
#include "noire/physics/wagon.hpp"
#include "noire/platform/window.hpp"
#include "noire/render/renderer.hpp"
#include "noire/render/vertex.hpp"
#include "noire/scene/track_mesh.hpp"
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

// Cube unité indexé (M7, étape 3) : 24 sommets (4 par face) avec normales et UV
// [0,1] correctes, pour montrer le placage de texture face par face. Sert de
// démonstrateur du pipeline texturé en attendant le chargement de train.glb (étape 6).
void make_unit_cube(std::vector<render::MeshVertex>& vertices,
                    std::vector<std::uint32_t>& indices) {
    struct Face {
        glm::vec3 normal;
        glm::vec3 origin;  // coin (u=0, v=0)
        glm::vec3 du;      // arête u (longueur 1)
        glm::vec3 dv;      // arête v (longueur 1)
    };
    const Face faces[6] = {
        {{0.0f, 0.0f, 1.0f}, {-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},    // +Z
        {{0.0f, 0.0f, -1.0f}, {0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},  // -Z
        {{1.0f, 0.0f, 0.0f}, {0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},    // +X
        {{-1.0f, 0.0f, 0.0f}, {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},  // -X
        {{0.0f, 1.0f, 0.0f}, {-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},    // +Y
        {{0.0f, -1.0f, 0.0f}, {-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},  // -Y
    };
    const glm::vec2 uv[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    vertices.clear();
    indices.clear();
    vertices.reserve(24);
    indices.reserve(36);
    std::uint32_t base = 0;
    for (const Face& f : faces) {
        const glm::vec3 corners[4] = {
            f.origin, f.origin + f.du, f.origin + f.du + f.dv, f.origin + f.dv,
        };
        for (int i = 0; i < 4; ++i) {
            vertices.push_back(render::MeshVertex{corners[i], f.normal, uv[i]});
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
        base += 4;
    }
}

// Texture damier RGBA8 générée en code (démonstration du placage). Deux teintes
// « signalisation » orangé / gris-bleu foncé.
std::vector<unsigned char> make_checker_rgba(int size, int cells) {
    std::vector<unsigned char> px(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) *
                                  4u);
    const unsigned char on_rgb[3] = {235, 120, 40};
    const unsigned char off_rgb[3] = {50, 55, 70};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool on = ((((x * cells) / size) + ((y * cells) / size)) % 2) == 0;
            const unsigned char* c = on ? on_rgb : off_rgb;
            const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                                     static_cast<std::size_t>(x)) *
                                    4u;
            px[idx + 0] = c[0];
            px[idx + 1] = c[1];
            px[idx + 2] = c[2];
            px[idx + 3] = 255;
        }
    }
    return px;
}

std::vector<render::Vertex> make_grid_vertices(int half_lines, float step) {
    const glm::vec3 gray{0.18f, 0.18f, 0.21f};
    const float extent = static_cast<float>(half_lines) * step;
    std::vector<render::Vertex> vertices;
    for (int i = -half_lines; i <= half_lines; ++i) {
        const float t = static_cast<float>(i) * step;
        vertices.push_back(render::Vertex{{t, 0.0f, -extent}, gray});
        vertices.push_back(render::Vertex{{t, 0.0f, extent}, gray});
        vertices.push_back(render::Vertex{{-extent, 0.0f, t}, gray});
        vertices.push_back(render::Vertex{{extent, 0.0f, t}, gray});
    }
    return vertices;
}

}  // namespace

struct Application::Impl {
    explicit Impl(ApplicationConfig cfg)
        : config(std::move(cfg)),
          window(platform::WindowConfig{config.width, config.height, config.title}),
          engine(EngineConfig{config.simulation_hz, 0}),
          track(kTrackOrigin),
          wagon(make_loco_config()),
          streamer(track, jobs, make_streamer_config()) {}

    static scene::StreamerConfig make_streamer_config() {
        scene::StreamerConfig sc;
        sc.chunk_length = 2000.0;
        sc.chunks_ahead = 2;
        sc.chunks_behind = 1;
        sc.max_uploads_per_update = 1;
        sc.rail_profile.sample_step = 2.0;
        return sc;
    }

    ApplicationConfig config;
    platform::Window window;
    render::Renderer renderer;
    Engine engine;
    Camera camera;

    ProceduralTrack track;
    JobSystem jobs;
    physics::Wagon wagon;
    scene::WorldStreamer streamer;

    audio::AudioEngine audio;
    audio::RailAudio rail_audio;

    render::MeshId grid_mesh = 0;
    render::MeshId bogie_mesh = 0;
    render::MeshId body_mesh = 0;

    // Démonstrateur M7 étape 3 : maillage indexé + texture damier, dessiné par le
    // pipeline texturé (cube rotatif au-dessus du train).
    render::MeshId test_indexed_mesh = 0;
    render::TextureId cube_texture = 0;
    double demo_time = 0.0;
    bool test_upload_reported = false;

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

        grid_mesh = renderer.create_mesh(make_grid_vertices(80, 5.0f), render::Topology::Lines);
        bogie_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.85f, 0.15f, 0.15f)),
                                          render::Topology::Triangles);
        body_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.20f, 0.38f, 0.58f)),
                                         render::Topology::Triangles);

        // M7 étape 3 : maillage indexé device-local + texture damier procédurale,
        // tous deux téléversés de façon asynchrone (staging + TransferManager).
        std::vector<render::MeshVertex> cube_v;
        std::vector<std::uint32_t> cube_i;
        make_unit_cube(cube_v, cube_i);
        test_indexed_mesh = renderer.create_mesh_indexed(cube_v, cube_i);
        const std::vector<unsigned char> checker = make_checker_rgba(64, 8);
        cube_texture = renderer.create_texture(64, 64, checker.data());

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
        // Smoke-test M7 étape 3 : signale une fois que le maillage indexé ET sa texture
        // sont prêts (transferts GPU asynchrones terminés, pipeline texturé actif).
        if (!test_upload_reported && test_indexed_mesh != 0 &&
            renderer.is_mesh_ready(test_indexed_mesh) && renderer.is_texture_ready(cube_texture)) {
            log::info("M7 étape 3 : maillage indexé + texture prêts — pipeline texturé actif");
            test_upload_reported = true;
        }

        // Streaming (thread principal), toujours exécuté (même minimisé).
        streamer.update(wagon.chainage(), renderer);

        const auto size = window.framebuffer_size();
        if (size.width == 0 || size.height == 0) {
            window.wait_events();
            return;
        }
        if (window.was_resized()) {
            window.reset_resized();
            renderer.notify_resized();
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
        const glm::vec3 fog_dry(0.02f, 0.03f, 0.06f);
        const glm::vec3 fog_wet(0.55f, 0.57f, 0.60f);
        const glm::vec3 fog_color = glm::mix(fog_dry, fog_wet, wetness);
        const float fog_density = glm::mix(0.0006f, 0.010f, wetness);
        uniforms.fog_color_density = glm::vec4(fog_color, fog_density);
        uniforms.params = glm::vec4(wetness, 0.0f, 0.0f, 0.0f);

        std::vector<render::DrawItem> items;

        // Sol infini (recentré sous le train).
        const WorldPosition wp = wagon.body_position();
        const double gs = 5.0;
        const WorldPosition grid_center(std::floor(wp.x / gs) * gs, wp.y - 3.0,
                                        std::floor(wp.z / gs) * gs);
        items.push_back(render::DrawItem{camera.relative_model(grid_center), grid_mesh});

        // Rails streamés (une origine par tuile).
        for (const scene::ChunkRenderInfo& chunk : streamer.renderables()) {
            items.push_back(render::DrawItem{camera.relative_model(chunk.origin), chunk.mesh});
        }

        // Deux bogies (cubes rouges).
        const glm::vec3 bogie_scale(2.2f, 0.7f, 2.6f);
        for (const physics::Bogie* b : {&wagon.front_bogie(), &wagon.rear_bogie()}) {
            const WorldPosition p = b->position() + WorldPosition{0.0, 0.4, 0.0};
            const glm::mat4 model = camera.relative_model(p) * b->orientation() *
                                    glm::scale(glm::mat4(1.0f), bogie_scale);
            items.push_back(render::DrawItem{model, bogie_mesh});
        }

        // Caisse.
        const glm::mat4 body_model = camera.relative_model(wagon.body_position()) *
                                     wagon.body_orientation() *
                                     glm::scale(glm::mat4(1.0f), glm::vec3(2.8f, 2.6f, 18.0f));
        items.push_back(render::DrawItem{body_model, body_mesh});

        // Démonstrateur M7 étape 3 : cube texturé (damier) tournant au-dessus du train.
        // Tombe sur la texture blanche de secours tant que `cube_texture` n'est pas prête.
        if (test_indexed_mesh != 0) {
            demo_time += dt_render;
            const WorldPosition cube_pos = wagon.body_position() + WorldPosition{0.0, 7.0, 0.0};
            const glm::mat4 cube_model =
                camera.relative_model(cube_pos) *
                glm::rotate(glm::mat4(1.0f), static_cast<float>(demo_time),
                            glm::vec3(0.25f, 1.0f, 0.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
            items.push_back(render::DrawItem{cube_model, test_indexed_mesh, cube_texture});
        }

        renderer.draw_frame(uniforms, items);
    }

    void shutdown() {
        jobs.stop();      // draine les workers avant toute destruction
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
