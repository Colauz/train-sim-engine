#pragma once

#include <cstddef>
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
    // Sifflet du TGV (M14) : deux-tons continu, posé au NEZ du train. Comme les autres
    // émetteurs, il porte sa `velocity` => le Doppler est calculé par miniaudio (rien à
    // faire de plus). `volume` = 0 quand la touche H est relâchée (le son reste en boucle,
    // simplement muet), ce qui donne l'attaque/détente sans réinitialiser la source.
    void set_horn(const WorldPosition& position, const glm::vec3& velocity, float volume);

    // Joint de rail : « clac » one-shot spatialisé (pool interne round-robin).
    void play_rail_joint(const WorldPosition& position, const glm::vec3& velocity, float volume);

    // Émetteurs pilotables par un PCM EXTERNE (M7 étape 5). Remplace la synthèse M6
    // d'un émetteur par le PCM fourni (mono float32 48 kHz, cf. decode_audio_file),
    // en conservant tout le chemin de spatialisation + Doppler. Le PCM est COPIÉ
    // (l'AudioEngine reste propriétaire de ses données). Si non appelé (ou fichier
    // manquant côté appelant), l'émetteur garde le son de synthèse — fallback M6.
    // Renvoie false si l'audio est indisponible ou le PCM invalide.
    enum class Emitter { Rumble, Squeal, Joint };
    bool set_source(Emitter emitter, const float* pcm, std::size_t frame_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace noire::audio
