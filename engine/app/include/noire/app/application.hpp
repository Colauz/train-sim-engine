#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace noire {

struct ApplicationConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::string title = "Noire Engine — M1";
    double simulation_hz = 120.0;
    bool enable_validation = true;
};

// Point d'entrée du moteur pour l'application. Assemble fenêtre + rendu + boucle,
// mais via PIMPL : cet en-tête n'inclut NI Vulkan NI GLFW. Le runtime reste vierge
// de toute logique graphique — il ne manipule que Application.
class Application {
public:
    explicit Application(ApplicationConfig config = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();  // 0 = succès

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace noire
