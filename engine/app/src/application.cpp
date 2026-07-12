#include "noire/app/application.hpp"

#include <utility>

#include "noire/core/engine.hpp"
#include "noire/core/log.hpp"
#include "noire/platform/window.hpp"
#include "noire/render/renderer.hpp"

namespace noire {

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

    bool initialize() {
        if (!window.initialize()) {
            return false;
        }

        // Câblage render -> fenêtre SANS que render ne connaisse GLFW :
        // on lui passe les extensions + une fabrique de surface + un fournisseur de taille.
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

        // Câblage de la boucle à pas fixe (core) vers fenêtre + rendu.
        EngineHooks hooks;
        hooks.poll_events = [this] { window.poll_events(); };
        hooks.should_stop = [this] { return window.should_close(); };
        hooks.fixed_update = [](double /*dt*/) {
            // Simulation ferroviaire à venir (M3 : bogies, adhérence, réseau de voies).
        };
        hooks.render = [this](double /*interpolation*/) {
            const auto size = window.framebuffer_size();
            if (size.width == 0 || size.height == 0) {
                window.wait_events();  // fenêtre minimisée : on attend un événement
                return;
            }
            if (window.was_resized()) {
                window.reset_resized();
                renderer.notify_resized();
            }
            renderer.draw_frame();
        };
        engine.set_hooks(std::move(hooks));

        return engine.initialize();
    }

    void shutdown() {
        renderer.wait_idle();  // vidange GPU avant destruction des ressources
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
