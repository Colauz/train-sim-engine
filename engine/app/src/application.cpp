#include "noire/app/application.hpp"

#include <utility>
#include <vector>

#include "noire/core/camera.hpp"
#include "noire/core/engine.hpp"
#include "noire/core/log.hpp"
#include "noire/core/math.hpp"
#include "noire/core/spline.hpp"
#include "noire/physics/bogie.hpp"
#include "noire/platform/window.hpp"
#include "noire/render/renderer.hpp"
#include "noire/render/vertex.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire {

namespace {

// Origine de la voie, volontairement à ~1000 km de l'origine absolue : démontre
// que l'origine flottante tient (mesh rails et bogie restent nets, sans jittering).
constexpr WorldPosition kTrackOrigin{1000000.0, 0.0, 1000000.0};

// Cube unité (±0.5) coloré uniformément, avec un léger ombrage par face pour la 3D.
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

// Grille au sol (y = 0) en lignes ; axes X (rouge) et Z (bleu) mis en valeur.
std::vector<render::Vertex> make_grid_vertices(int half_lines, float step) {
    const glm::vec3 gray{0.22f, 0.22f, 0.25f};
    const glm::vec3 axis_x{0.55f, 0.20f, 0.20f};
    const glm::vec3 axis_z{0.20f, 0.35f, 0.60f};
    const float extent = static_cast<float>(half_lines) * step;

    std::vector<render::Vertex> vertices;
    for (int i = -half_lines; i <= half_lines; ++i) {
        const float t = static_cast<float>(i) * step;
        const glm::vec3 col_z = (i == 0) ? axis_z : gray;
        vertices.push_back(render::Vertex{{t, 0.0f, -extent}, col_z});
        vertices.push_back(render::Vertex{{t, 0.0f, extent}, col_z});
        const glm::vec3 col_x = (i == 0) ? axis_x : gray;
        vertices.push_back(render::Vertex{{-extent, 0.0f, t}, col_x});
        vertices.push_back(render::Vertex{{extent, 0.0f, t}, col_x});
    }
    return vertices;
}

}  // namespace

struct Application::Impl {
    explicit Impl(ApplicationConfig cfg)
        : config(std::move(cfg)),
          window(platform::WindowConfig{config.width, config.height, config.title}),
          engine(EngineConfig{config.simulation_hz, 0}) {}

    ApplicationConfig config;
    platform::Window window;
    render::Renderer renderer;
    Engine engine;
    Camera camera;

    Spline track;              // la voie ferrée (double)
    physics::Bogie bogie;      // le bogie cinématique contraint dessus

    render::MeshId grid_mesh = 0;
    render::MeshId rails_mesh = 0;
    render::MeshId bogie_mesh = 0;
    WorldPosition grid_center{kTrackOrigin};

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

        // --- La voie : une courbe en S d'environ 500 m (points de contrôle en double) ---
        track.set_control_points({
            kTrackOrigin + WorldPosition{0.0, 0.0, 0.0},
            kTrackOrigin + WorldPosition{100.0, 0.0, 0.0},
            kTrackOrigin + WorldPosition{200.0, 0.0, 35.0},
            kTrackOrigin + WorldPosition{300.0, 0.0, -35.0},
            kTrackOrigin + WorldPosition{400.0, 0.0, 0.0},
            kTrackOrigin + WorldPosition{500.0, 0.0, 0.0},
        });
        log::info("Voie générée : longueur ~{:.1f} m", track.length());

        // --- Maillages créés UNE SEULE FOIS (jamais recalculés par frame) ---
        rails_mesh = renderer.create_mesh(scene::generate_rail_mesh(track, kTrackOrigin, {}),
                                          render::Topology::Triangles);
        grid_mesh = renderer.create_mesh(make_grid_vertices(60, 5.0f), render::Topology::Lines);
        bogie_mesh = renderer.create_mesh(make_box_vertices(glm::vec3(0.85f, 0.15f, 0.15f)),
                                          render::Topology::Triangles);
        grid_center = kTrackOrigin + WorldPosition{250.0, 0.0, 0.0};

        // --- Le bogie posé sur la voie, à vitesse constante ---
        bogie.attach(&track);
        bogie.set_distance(0.0);
        bogie.set_speed(18.0);  // ~65 km/h

        // Caméra derrière le début de la voie, regardant le long des rails (+X).
        camera.set_position(kTrackOrigin + WorldPosition{-25.0, 18.0, 14.0});
        camera.set_orientation(0.08f, -0.25f);
        window.set_cursor_captured(true);

        EngineHooks hooks;
        hooks.poll_events = [this] { window.poll_events(); };
        hooks.should_stop = [this] { return window.should_close(); };
        hooks.variable_update = [this](double dt) { update_input(dt); };
        hooks.fixed_update = [this](double dt) {
            bogie.update(dt);  // avancement déterministe le long de la spline
        };
        hooks.render = [this](double /*interpolation*/) { render_frame(); };
        engine.set_hooks(std::move(hooks));

        log::info("Contrôles : ZQSD/WASD = déplacement, Espace/Maj = haut/bas, souris = regard, Échap = quitter");
        return engine.initialize();
    }

    void update_input(double dt) {
        using platform::Key;

        const platform::CursorDelta d = window.consume_cursor_delta();
        const float sensitivity = camera.mouse_sensitivity;
        camera.add_yaw_pitch(static_cast<float>(d.dx) * sensitivity,
                             static_cast<float>(-d.dy) * sensitivity);

        const double distance = static_cast<double>(camera.move_speed) * dt;
        double forward = 0.0;
        double strafe = 0.0;
        double vertical = 0.0;
        if (window.is_key_down(Key::W) || window.is_key_down(Key::Z)) forward += 1.0;
        if (window.is_key_down(Key::S)) forward -= 1.0;
        if (window.is_key_down(Key::D)) strafe += 1.0;
        if (window.is_key_down(Key::A) || window.is_key_down(Key::Q)) strafe -= 1.0;
        if (window.is_key_down(Key::Space)) vertical += 1.0;
        if (window.is_key_down(Key::LeftShift) || window.is_key_down(Key::LeftControl))
            vertical -= 1.0;
        camera.move_local(forward * distance, strafe * distance, vertical * distance);

        if (window.is_key_down(Key::Escape)) {
            window.request_close();
        }
    }

    void render_frame() {
        const auto size = window.framebuffer_size();
        if (size.width == 0 || size.height == 0) {
            window.wait_events();
            return;
        }
        if (window.was_resized()) {
            window.reset_resized();
            renderer.notify_resized();
        }

        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);

        render::FrameUniforms uniforms;
        uniforms.view = camera.view_matrix();
        uniforms.proj = camera.projection_matrix(aspect);

        // ORIGINE FLOTTANTE : chaque Model = (position monde double - caméra double) -> float.
        std::vector<render::DrawItem> items;
        items.reserve(3);

        // Grille (sommets locaux centrés) + rails (sommets locaux à kTrackOrigin).
        items.push_back(render::DrawItem{camera.relative_model(grid_center), grid_mesh});
        items.push_back(render::DrawItem{camera.relative_model(kTrackOrigin), rails_mesh});

        // Bogie : position (double) relevée de 0.6 m, orientée sur la tangente, mise à l'échelle.
        const WorldPosition bogie_pos = bogie.position() + WorldPosition{0.0, 0.6, 0.0};
        const glm::mat4 bogie_model = camera.relative_model(bogie_pos) * bogie.orientation() *
                                      glm::scale(glm::mat4(1.0f), glm::vec3(1.4f, 0.9f, 2.6f));
        items.push_back(render::DrawItem{bogie_model, bogie_mesh});

        renderer.draw_frame(uniforms, items);
    }

    void shutdown() {
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
