#pragma once

#include <cstdint>

namespace noire {

// Paramètres de création du moteur (style « CreateInfo »). Défini au niveau du
// namespace pour pouvoir servir d'argument par défaut ({}) du constructeur.
struct EngineConfig {
    double simulation_hz = 120.0;  // fréquence de la simulation (Hz)
    std::uint32_t max_ticks = 0;   // 0 = illimité ; sinon arrêt auto (utile en démo/tests)
};

// Cœur du moteur : boucle principale à PAS DE TEMPS FIXE.
//
// La simulation (physique lourde, dynamique des bogies, réseau de voies, IA)
// avance par incréments fixes et déterministes, découplés du framerate de rendu.
// Le rendu, lui, interpole entre les deux derniers états simulés pour rester
// fluide quel que soit le taux d'images. Référence : Glenn Fiedler,
// « Fix Your Timestep! ». C'est LE pilier d'une simulation reproductible.
class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool initialize();
    void run();
    void shutdown();

    void request_stop() { running_ = false; }
    [[nodiscard]] std::uint64_t tick_count() const { return tick_count_; }

private:
    void fixed_update(double dt);       // un pas de simulation
    void render(double interpolation);  // rendu de l'état interpolé [0,1]

    EngineConfig config_;
    bool running_ = false;
    bool initialized_ = false;
    std::uint64_t tick_count_ = 0;
};

}  // namespace noire
