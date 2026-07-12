#include "noire/app/application.hpp"

#include <cmath>
#include <utility>
#include <vector>

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

// Origine du monde à ~1000 km : l'origine flottante reste indispensable, y compris
// au fil des chunks (chaque tuile a sa propre origine locale).
constexpr WorldPosition kTrackOrigin{1000000.0, 0.0, 1000000.0};

physics::WagonConfig make_loco_config() {
    physics::WagonConfig c;
    c.mass = 80000.0;                 // 80 t (locomotive)
    c.wheelbase = 14.0;
    c.max_tractive_effort = 300000.0;  // 300 kN
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
        sc.chunk_length = 2000.0;  // tuiles de 2 km
        sc.chunks_ahead = 2;
        sc.chunks_behind = 1;
        sc.max_uploads_per_update = 1;  // budget anti-pic
        sc.rail_profile.sample_step = 2.0;
        return sc;
    }

    ApplicationConfig config;
    platform::Window window;
    render::Renderer renderer;
    Engine engine;
    Camera camera;

    ProceduralTrack track;       // voie INFINIE analytique
    JobSystem jobs;              // pool de threads (génération async)
    physics::Wagon wagon;        // dépend de `track`
    scene::WorldStreamer streamer;  // dépend de `track` et `jobs`

    render::MeshId grid_mesh = 0;
    render::MeshId bogie_mesh = 0;
    render::MeshId body_mesh = 0;

    float orbit_yaw = 3.14159f;
    float orbit_pitch = 0.30f;
    float orbit_distance = 42.0f;
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

        // Workers pour la génération asynchrone du décor.
        jobs.start(2);

        grid_mesh = renderer.create_mesh(make_grid_vertices(80, 5.0f), render::Topology::Lines);
        bogie_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.85f, 0.15f, 0.15f)),
                                          render::Topology::Triangles);
        body_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.20f, 0.38f, 0.58f)),
                                         render::Topology::Triangles);

        wagon.attach(&track);
        wagon.place_at(0.0);
        wagon.set_speed(25.0);  // départ lancé (~90 km/h) : le streaming s'exerce d'emblée
        window.set_cursor_captured(true);

        EngineHooks hooks;
        hooks.poll_events = [this] { window.poll_events(); };
        hooks.should_stop = [this] { return window.should_close(); };
        hooks.variable_update = [this](double dt) { update_input(dt); };
        hooks.fixed_update = [this](double dt) { update_physics(dt); };
        hooks.render = [this](double /*interpolation*/) { render_frame(); };
        engine.set_hooks(std::move(hooks));

        log::info("Commandes : W/Z = traction, S = frein | souris = orbite, Espace/Maj = zoom | Échap = quitter");
        return engine.initialize();
    }

    void update_input(double dt) {
        using platform::Key;

        const double throttle = (window.is_key_down(Key::W) || window.is_key_down(Key::Z)) ? 1.0 : 0.0;
        const double brake = window.is_key_down(Key::S) ? 1.0 : 0.0;
        wagon.set_controls(throttle, brake);

        if (window.is_key_down(Key::Escape)) {
            window.request_close();
        }

        const platform::CursorDelta d = window.consume_cursor_delta();
        orbit_yaw += static_cast<float>(d.dx) * 0.005f;
        orbit_pitch = glm::clamp(orbit_pitch - static_cast<float>(d.dy) * 0.005f, -1.30f, 1.30f);
        if (window.is_key_down(Key::Space)) orbit_distance += static_cast<float>(25.0 * dt);
        if (window.is_key_down(Key::LeftShift)) orbit_distance -= static_cast<float>(25.0 * dt);
        orbit_distance = glm::clamp(orbit_distance, 12.0f, 200.0f);
    }

    void update_physics(double dt) {
        wagon.update(dt);  // dynamique déterministe (jamais bloquée par le streaming)

        if (++telemetry_ticks % 120 == 0) {  // ~1x/s
            log::info("v={:6.1f} km/h | pente={:+5.1f}% | {} | chunks actifs={} | jobs={}",
                      wagon.speed() * 3.6, wagon.grade_percent(),
                      wagon.slipping() ? "PATINE   " : "adherent ", streamer.active_chunk_count(),
                      jobs.in_flight());
        }
    }

    void render_frame() {
        // STREAMING (thread principal) : (dé)charge les tuiles selon la position du train.
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

        // Caméra orbitale qui suit le wagon.
        const WorldPosition target = wagon.body_position();
        const glm::vec3 dir(std::cos(orbit_yaw) * std::cos(orbit_pitch), std::sin(orbit_pitch),
                            std::sin(orbit_yaw) * std::cos(orbit_pitch));
        camera.set_position(target + WorldPosition(dir) * static_cast<double>(orbit_distance));
        camera.look_at(target);

        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
        render::FrameUniforms uniforms;
        uniforms.view = camera.view_matrix();
        uniforms.proj = camera.projection_matrix(aspect);

        std::vector<render::DrawItem> items;

        // Sol « infini » : la grille se recentre sous le train (accrochée au pas de grille).
        const WorldPosition wp = wagon.body_position();
        const double gs = 5.0;
        const WorldPosition grid_center(std::floor(wp.x / gs) * gs, wp.y - 3.0,
                                        std::floor(wp.z / gs) * gs);
        items.push_back(render::DrawItem{camera.relative_model(grid_center), grid_mesh});

        // Rails : une entrée par tuile chargée, chacune avec SA propre origine (origine flottante).
        for (const scene::ChunkRenderInfo& chunk : streamer.renderables()) {
            items.push_back(render::DrawItem{camera.relative_model(chunk.origin), chunk.mesh});
        }

        // Les deux bogies (cubes rouges).
        const glm::vec3 bogie_scale(2.2f, 0.7f, 2.6f);
        for (const physics::Bogie* b : {&wagon.front_bogie(), &wagon.rear_bogie()}) {
            const WorldPosition p = b->position() + WorldPosition{0.0, 0.4, 0.0};
            const glm::mat4 model = camera.relative_model(p) * b->orientation() *
                                    glm::scale(glm::mat4(1.0f), bogie_scale);
            items.push_back(render::DrawItem{model, bogie_mesh});
        }

        // La caisse (parallélépipède) au-dessus.
        const glm::mat4 body_model = camera.relative_model(wagon.body_position()) *
                                     wagon.body_orientation() *
                                     glm::scale(glm::mat4(1.0f), glm::vec3(2.8f, 2.6f, 18.0f));
        items.push_back(render::DrawItem{body_model, body_mesh});

        renderer.draw_frame(uniforms, items);
    }

    void shutdown() {
        // IMPORTANT : arrêter les workers AVANT tout le reste. stop() draine les jobs
        // en cours (ils écrivent dans les chunks) => plus aucun accès concurrent ensuite.
        jobs.stop();
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
