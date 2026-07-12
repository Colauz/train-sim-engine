#include "noire/core/engine.hpp"

#include <algorithm>
#include <chrono>

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
        if (hooks_.poll_events) {
            hooks_.poll_events();
        }
        if (hooks_.should_stop && hooks_.should_stop()) {
            break;
        }

        const auto now = Clock::now();
        double frame_time = std::chrono::duration<double>(now - previous).count();
        previous = now;
        frame_time = std::min(frame_time, 0.25);  // garde-fou anti « spirale de la mort »

        // Mise à jour à débit variable (caméra libre, inputs) : hors simulation déterministe.
        if (hooks_.variable_update) {
            hooks_.variable_update(frame_time);
        }

        accumulator += frame_time;

        // Simulation : consommée par pas fixes => déterminisme.
        while (accumulator >= dt) {
            if (hooks_.fixed_update) {
                hooks_.fixed_update(dt);
            }
            ++tick_count_;
            accumulator -= dt;
            if (config_.max_ticks != 0 && tick_count_ >= config_.max_ticks) {
                running_ = false;
                break;
            }
        }

        // Rendu : état interpolé entre les deux derniers pas fixes.
        const double interpolation = accumulator / dt;
        if (hooks_.render) {
            hooks_.render(interpolation);
        }
    }
    running_ = false;
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
