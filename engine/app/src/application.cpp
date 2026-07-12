#include "noire/app/application.hpp"

#include <utility>
#include <vector>

#include "noire/core/camera.hpp"
#include "noire/core/engine.hpp"
#include "noire/core/log.hpp"
#include "noire/core/math.hpp"
#include "noire/platform/window.hpp"
#include "noire/render/renderer.hpp"
#include "noire/render/vertex.hpp"

namespace noire {

namespace {

// Origine du monde volontairement TRÈS éloignée (~1 000 km sur X et Z) pour
// démontrer l'origine flottante : sans elle, un float perdrait ~0,1 m de précision
// ici => jittering. Avec le calcul double->relatif->float, tout reste net.
constexpr WorldPosition kWorldBase{1000000.0, 0.0, 1000000.0};

// Cube unité (±0.5) centré à l'origine, une couleur par face. cullMode = NONE
// => l'ordre des sommets n'a pas d'importance pour la visibilité.
std::vector<render::Vertex> make_cube_vertices() {
    const glm::vec3 c[8] = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},
    };
    struct Face {
        int a, b, c, d;
        glm::vec3 color;
    };
    const Face faces[6] = {
        {4, 5, 6, 7, {0.85f, 0.25f, 0.25f}},  // avant  (z+) rouge
        {1, 0, 3, 2, {0.25f, 0.55f, 0.85f}},  // arrière (z-) bleu
        {0, 4, 7, 3, {0.30f, 0.75f, 0.35f}},  // gauche (x-) vert
        {5, 1, 2, 6, {0.90f, 0.70f, 0.25f}},  // droite (x+) jaune
        {7, 6, 2, 3, {0.85f, 0.85f, 0.85f}},  // haut   (y+) clair
        {0, 1, 5, 4, {0.45f, 0.45f, 0.50f}},  // bas    (y-) sombre
    };
    std::vector<render::Vertex> vertices;
    vertices.reserve(36);
    for (const Face& f : faces) {
        const int order[6] = {f.a, f.b, f.c, f.a, f.c, f.d};
        for (int i : order) {
            vertices.push_back(render::Vertex{c[i], f.color});
        }
    }
    return vertices;
}

// Grille au sol (y = 0) faite de lignes ; axes X (rouge) et Z (bleu) mis en valeur.
std::vector<render::Vertex> make_grid_vertices(int half_lines, float step) {
    const glm::vec3 gray{0.25f, 0.25f, 0.28f};
    const glm::vec3 axis_x{0.70f, 0.20f, 0.20f};
    const glm::vec3 axis_z{0.20f, 0.40f, 0.80f};
    const float extent = static_cast<float>(half_lines) * step;

    std::vector<render::Vertex> vertices;
    for (int i = -half_lines; i <= half_lines; ++i) {
        const float t = static_cast<float>(i) * step;
        // Ligne parallèle à Z (x = t) ; à x = 0 c'est l'axe Z.
        const glm::vec3 col_z = (i == 0) ? axis_z : gray;
        vertices.push_back(render::Vertex{{t, 0.0f, -extent}, col_z});
        vertices.push_back(render::Vertex{{t, 0.0f, extent}, col_z});
        // Ligne parallèle à X (z = t) ; à z = 0 c'est l'axe X.
        const glm::vec3 col_x = (i == 0) ? axis_x : gray;
        vertices.push_back(render::Vertex{{-extent, 0.0f, t}, col_x});
        vertices.push_back(render::Vertex{{extent, 0.0f, t}, col_x});
    }
    return vertices;
}

}  // namespace

// Toute la logique (et donc toutes les dépendances graphiques) est confinée ici.
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

    render::MeshId grid_mesh = 0;
    render::MeshId cube_mesh = 0;
    WorldPosition grid_origin{kWorldBase};
    std::vector<WorldPosition> cube_positions;

    bool initialize() {
        if (!window.initialize()) {
            return false;
        }

        // render <- fenêtre, sans couplage à GLFW (fabrique de surface + taille).
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

        // Géométrie de la scène (définie par l'app, pas par render).
        grid_mesh = renderer.create_mesh(make_grid_vertices(50, 2.0f), render::Topology::Lines);
        cube_mesh = renderer.create_mesh(make_cube_vertices(), render::Topology::Triangles);
        cube_positions = {
            kWorldBase + WorldPosition{0.0, 0.5, 0.0},   kWorldBase + WorldPosition{3.0, 0.5, -2.0},
            kWorldBase + WorldPosition{-3.0, 0.5, -1.0}, kWorldBase + WorldPosition{0.0, 0.5, -6.0},
            kWorldBase + WorldPosition{6.0, 1.5, -4.0},  kWorldBase + WorldPosition{-6.0, 0.5, -5.0},
        };

        // Caméra placée à ~1 000 km de l'origine absolue, face aux cubes.
        camera.set_position(kWorldBase + WorldPosition{0.0, 2.0, 8.0});
        window.set_cursor_captured(true);  // mode FPS

        EngineHooks hooks;
        hooks.poll_events = [this] { window.poll_events(); };
        hooks.should_stop = [this] { return window.should_close(); };
        hooks.variable_update = [this](double dt) { update_input(dt); };
        hooks.fixed_update = [](double /*dt*/) {
            // Simulation ferroviaire à venir (M3 : bogies, adhérence, réseau de voies).
        };
        hooks.render = [this](double /*interpolation*/) { render_frame(); };
        engine.set_hooks(std::move(hooks));

        log::info("Contrôles : ZQSD/WASD = déplacement, Espace/Maj = haut/bas, souris = regard, Échap = quitter");
        return engine.initialize();
    }

    void update_input(double dt) {
        using platform::Key;

        // Souris -> orientation (fly camera).
        const platform::CursorDelta d = window.consume_cursor_delta();
        const float sensitivity = camera.mouse_sensitivity;
        camera.add_yaw_pitch(static_cast<float>(d.dx) * sensitivity,
                             static_cast<float>(-d.dy) * sensitivity);

        // Clavier -> déplacement (ZQSD + WASD gérés ensemble).
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
            window.wait_events();  // minimisé
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

        // ORIGINE FLOTTANTE : chaque Model est calculé (double - double) puis ramené
        // en float relativement à la caméra. render ne reçoit que des float.
        std::vector<render::DrawItem> items;
        items.reserve(cube_positions.size() + 1);
        items.push_back(render::DrawItem{camera.relative_model(grid_origin), grid_mesh});
        for (const WorldPosition& p : cube_positions) {
            items.push_back(render::DrawItem{camera.relative_model(p), cube_mesh});
        }

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
