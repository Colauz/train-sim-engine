#include <noire/core/engine.hpp>

int main() {
    noire::EngineConfig config;
    config.simulation_hz = 120.0;  // pas de temps physique (120 Hz)
    config.max_ticks = 600;        // ~5 s de simulation puis arrêt propre (démo)

    noire::Engine engine(config);
    if (!engine.initialize()) {
        return 1;
    }

    engine.run();
    engine.shutdown();
    return 0;
}
