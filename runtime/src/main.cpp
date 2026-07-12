#include <noire/app/application.hpp>

// Le runtime ne connaît NI Vulkan NI GLFW NI l'audio : il lance l'Application.
int main() {
    noire::ApplicationConfig config;
    config.title = "Noire Engine — M6 : Audio spatialisé & météo dynamique";
    config.width = 1280;
    config.height = 720;
    config.simulation_hz = 120.0;

    noire::Application app{config};
    return app.run();
}
