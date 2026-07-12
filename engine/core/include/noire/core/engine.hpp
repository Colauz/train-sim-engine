#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace noire {

// Paramètres de création du moteur (style « CreateInfo »). Défini au niveau du
// namespace pour pouvoir servir d'argument par défaut ({}) du constructeur.
struct EngineConfig {
    double simulation_hz = 120.0;  // fréquence de la simulation (Hz)
    std::uint32_t max_ticks = 0;   // 0 = illimité ; sinon arrêt auto (démo/tests)
};

// Points d'ancrage injectés par la couche supérieure (app). Le module core reste
// ainsi TOTALEMENT indépendant de la fenêtre et du rendu : il n'orchestre que le
// temps. C'est ce qui empêche la logique graphique de « fuiter » vers le bas.
struct EngineHooks {
    std::function<void()> poll_events;                    // pomper les événements OS
    std::function<bool()> should_stop;                    // true => quitter la boucle
    std::function<void(double frame_dt)> variable_update;  // caméra, inputs (par frame)
    std::function<void(double dt)> fixed_update;          // un pas de simulation (fixe)
    std::function<void(double interpolation)> render;     // rendu interpolé [0,1]
};

// Boucle principale à PAS DE TEMPS FIXE (cf. Glenn Fiedler « Fix Your Timestep! »).
class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void set_hooks(EngineHooks hooks) { hooks_ = std::move(hooks); }

    bool initialize();
    void run();
    void shutdown();

    void request_stop() { running_ = false; }
    [[nodiscard]] std::uint64_t tick_count() const { return tick_count_; }

private:
    EngineConfig config_;
    EngineHooks hooks_;
    bool running_ = false;
    bool initialized_ = false;
    std::uint64_t tick_count_ = 0;
};

}  // namespace noire
