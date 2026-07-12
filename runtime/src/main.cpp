#include <noire/app/application.hpp>

// Le runtime ne connaît NI Vulkan NI GLFW : il configure et lance l'Application.
int main() {
    noire::ApplicationConfig config;
    config.title = "Noire Engine — M1 : Premier Triangle Vulkan";
    config.width = 1280;
    config.height = 720;
    config.simulation_hz = 120.0;

    noire::Application app{config};
    return app.run();
}
