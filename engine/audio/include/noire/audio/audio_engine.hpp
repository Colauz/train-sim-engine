#pragma once

#include <memory>

#include "noire/core/math.hpp"

namespace noire::audio {

// Moteur audio spatialisé (au-dessus de miniaudio). PIMPL : cet en-tête n'inclut
// PAS miniaudio.h — le runtime et l'app restent vierges de tout détail audio.
//
// Non-blocage : miniaudio possède son propre thread audio temps réel ; nos appels
// ne font que pousser des paramètres (position/vitesse/volume) => jamais bloquants.
//
// Origine flottante : les positions monde (double) sont converties en coordonnées
// RELATIVES au listener (float) avant d'être passées à miniaudio (le listener est
// maintenu à l'origine), ce qui préserve la précision de la spatialisation.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool initialize();  // false si aucun périphérique (l'audio devient alors no-op)
    void shutdown();
    [[nodiscard]] bool valid() const;

    // Listener attaché à la caméra.
    void update_listener(const WorldPosition& position, const glm::vec3& velocity,
                         const glm::vec3& forward, const glm::vec3& up);

    // Émetteurs continus : bruit de roulement et crissement (volume/pitch modulés).
    void set_rumble(const WorldPosition& position, const glm::vec3& velocity, float volume,
                    float pitch);
    void set_squeal(const WorldPosition& position, const glm::vec3& velocity, float volume);

    // Joint de rail : « clac » one-shot spatialisé (pool interne round-robin).
    void play_rail_joint(const WorldPosition& position, const glm::vec3& velocity, float volume);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace noire::audio
