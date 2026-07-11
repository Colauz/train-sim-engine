#include "noire/core/engine.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "noire/core/log.hpp"
#include "noire/core/version.hpp"

namespace noire {

using Clock = std::chrono::steady_clock;

Engine::Engine(EngineConfig config) : config_(config) {}

Engine::~Engine() { shutdown(); }

bool Engine::initialize() {
    log::init();
    log::info("Noire Engine v{} — démarrage", noire::version::string);
    log::info("Boucle de simulation à pas fixe : {} Hz", config_.simulation_hz);
    initialized_ = true;
    running_ = true;
    return true;
}

void Engine::run() {
    const double dt = 1.0 / config_.simulation_hz;  // pas de temps fixe (s)
    double accumulator = 0.0;
    auto previous = Clock::now();

    while (running_) {
        const auto now = Clock::now();
        double frame_time = std::chrono::duration<double>(now - previous).count();
        previous = now;

        // Garde-fou anti « spirale de la mort » : on borne le retard rattrapable.
        frame_time = std::min(frame_time, 0.25);
        accumulator += frame_time;

        // On consomme le temps accumulé par pas fixes : simulation déterministe.
        while (accumulator >= dt) {
            fixed_update(dt);
            accumulator -= dt;
            if (config_.max_ticks != 0 && tick_count_ >= config_.max_ticks) {
                request_stop();
                break;
            }
        }

        // Fraction restante [0,1] : facteur d'interpolation pour un rendu fluide.
        const double interpolation = accumulator / dt;
        render(interpolation);
    }
}

void Engine::fixed_update(double /*dt*/) {
    ++tick_count_;
    // À venir : intégration physique (bogies, suspensions, adhérence acier/acier),
    // avancement des trains sur le réseau de voies, freinage pneumatique, IA.
}

void Engine::render(double /*interpolation*/) {
    // À venir : soumission des command buffers Vulkan à partir de l'état interpolé.
    // Tant qu'aucune fenêtre / VSync ne cadence la boucle, on cède le CPU un instant.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Engine::shutdown() {
    if (!initialized_) {
        return;
    }
    initialized_ = false;
    running_ = false;
    log::info("Arrêt du moteur après {} pas de simulation", tick_count_);
    log::shutdown();
}

}  // namespace noire
