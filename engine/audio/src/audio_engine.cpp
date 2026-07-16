#include "noire/audio/audio_engine.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <miniaudio.h>

#include "noire/core/log.hpp"

namespace noire::audio {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kJointPoolSize = 12;  // clacs simultanés max (recouvrement à grande vitesse)
constexpr float kPi = 3.14159265358979f;

// Générateur de bruit blanc simple (LCG) pour la synthèse.
struct Noise {
    std::uint32_t state = 0x12345u;
    float operator()() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
    }
};

// « Clac » de joint : attaque nette + résonance métallique + thump grave, ~90 ms.
std::vector<float> synth_clack() {
    const int n = kSampleRate * 90 / 1000;
    std::vector<float> s(static_cast<std::size_t>(n));
    Noise noise{0xC1ACu};
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float env_hi = std::exp(-t * 55.0f);
        const float env_lo = std::exp(-t * 22.0f);
        const float click = noise() * env_hi * 0.5f;
        const float thump = std::sin(2.0f * kPi * 130.0f * t) * env_lo * 0.6f;
        const float ring = std::sin(2.0f * kPi * 900.0f * t) * env_hi * 0.25f;
        s[static_cast<std::size_t>(i)] = click + thump + ring;
    }
    return s;
}

// Crissement métallique : tonal aigu + harmonique + léger vibrato, boucle 1 s.
std::vector<float> synth_squeal() {
    const int n = kSampleRate;  // 1 s => fréquences entières = boucle continue
    std::vector<float> s(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float vib = 1.0f + 0.015f * std::sin(2.0f * kPi * 7.0f * t);
        const float base = std::sin(2.0f * kPi * 2600.0f * vib * t) * 0.5f;
        const float harm = std::sin(2.0f * kPi * 5200.0f * t) * 0.18f;
        s[static_cast<std::size_t>(i)] = base + harm;
    }
    return s;
}

// Sifflet du TGV (M14) : deux-tons cuivré, boucle 1 s. Un avertisseur ferroviaire est un
// ACCORD de deux notes (le « deux-tons » SNCF) ; ici une tierce majeure (~349 + 440 Hz)
// enrichie de quelques harmoniques pour le mordant, avec une attaque douce sur le tout
// premier instant pour éviter le clic de démarrage de boucle.
std::vector<float> synth_horn() {
    const int n = kSampleRate;  // 1 s, fréquences entières => boucle sans couture
    std::vector<float> s(static_cast<std::size_t>(n));
    const float f_lo = 349.0f;  // fa
    const float f_hi = 440.0f;  // la (tierce majeure au-dessus)
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        auto tone = [t](float f) {
            // Fondamentale + 2e et 3e harmoniques décroissantes => timbre d'avertisseur.
            return std::sin(2.0f * kPi * f * t) * 0.6f +
                   std::sin(2.0f * kPi * 2.0f * f * t) * 0.25f +
                   std::sin(2.0f * kPi * 3.0f * f * t) * 0.12f;
        };
        s[static_cast<std::size_t>(i)] = (tone(f_lo) + tone(f_hi)) * 0.45f;
    }
    return s;
}

// Roulement : grave filtré passe-bas + tonales basses, boucle 1 s.
std::vector<float> synth_rumble() {
    const int n = kSampleRate;
    std::vector<float> s(static_cast<std::size_t>(n));
    Noise noise{0x2113u};
    float low_pass = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        low_pass = low_pass * 0.96f + noise() * 0.04f;
        const float tonal =
            std::sin(2.0f * kPi * 40.0f * t) * 0.35f + std::sin(2.0f * kPi * 70.0f * t) * 0.22f;
        s[static_cast<std::size_t>(i)] = tonal + low_pass * 1.8f;
    }
    return s;
}

}  // namespace

struct AudioEngine::Impl {
    ma_engine engine{};
    bool engine_ok = false;
    bool sounds_ok = false;

    std::vector<float> clack_pcm;
    std::vector<float> squeal_pcm;
    std::vector<float> rumble_pcm;
    std::vector<float> horn_pcm;

    std::array<ma_audio_buffer, kJointPoolSize> clack_bufs{};
    std::array<ma_sound, kJointPoolSize> joint_pool{};
    int joint_next = 0;

    ma_audio_buffer squeal_buf{};
    ma_audio_buffer rumble_buf{};
    ma_audio_buffer horn_buf{};
    ma_sound squeal_sound{};
    ma_sound rumble_sound{};
    ma_sound horn_sound{};

    WorldPosition listener_pos{0.0, 0.0, 0.0};

    [[nodiscard]] glm::vec3 relative(const WorldPosition& world) const {
        return glm::vec3(world - listener_pos);
    }

    static bool make_buffer(std::vector<float>& pcm, ma_audio_buffer& buffer) {
        ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
            ma_format_f32, 1, pcm.size(), pcm.data(), nullptr);
        cfg.sampleRate = kSampleRate;
        return ma_audio_buffer_init(&cfg, &buffer) == MA_SUCCESS;
    }

    // Remplace la source d'un son en boucle (rumble/squeal) par un PCM externe, en
    // conservant looping + Doppler + volume. `storage` devient propriétaire du PCM
    // copié ; l'ancien tampon (qui le référençait) est détruit AVANT la réassignation.
    bool rebuild_loop(ma_sound& sound, ma_audio_buffer& buffer, std::vector<float>& storage,
                      const float* pcm, std::size_t frames) {
        ma_sound_stop(&sound);
        ma_sound_uninit(&sound);
        ma_audio_buffer_uninit(&buffer);
        storage.assign(pcm, pcm + frames);
        if (!make_buffer(storage, buffer)) {
            return false;
        }
        if (ma_sound_init_from_data_source(&engine, &buffer, 0, nullptr, &sound) != MA_SUCCESS) {
            return false;
        }
        ma_sound_set_looping(&sound, MA_TRUE);
        ma_sound_set_doppler_factor(&sound, 1.0f);
        ma_sound_set_volume(&sound, 0.0f);  // le prochain set_rumble/set_squeal pilotera le volume
        ma_sound_start(&sound);
        return true;
    }

    // Idem pour le pool de « clacs » one-shot (kJointPoolSize sons partageant le PCM).
    bool rebuild_joint(const float* pcm, std::size_t frames) {
        for (ma_sound& s : joint_pool) {
            ma_sound_stop(&s);
            ma_sound_uninit(&s);
        }
        for (ma_audio_buffer& b : clack_bufs) {
            ma_audio_buffer_uninit(&b);
        }
        clack_pcm.assign(pcm, pcm + frames);
        for (int i = 0; i < kJointPoolSize; ++i) {
            const std::size_t idx = static_cast<std::size_t>(i);
            if (!make_buffer(clack_pcm, clack_bufs[idx])) {
                return false;
            }
            if (ma_sound_init_from_data_source(&engine, &clack_bufs[idx], 0, nullptr,
                                               &joint_pool[idx]) != MA_SUCCESS) {
                return false;
            }
            ma_sound_set_doppler_factor(&joint_pool[idx], 1.0f);
        }
        return true;
    }
};

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::valid() const { return impl_ && impl_->sounds_ok; }

bool AudioEngine::initialize() {
    impl_ = std::make_unique<Impl>();

    if (ma_engine_init(nullptr, &impl_->engine) != MA_SUCCESS) {
        log::warn("Audio : aucun périphérique disponible — audio désactivé (le jeu tourne)");
        impl_.reset();
        return false;
    }
    impl_->engine_ok = true;

    impl_->clack_pcm = synth_clack();
    impl_->squeal_pcm = synth_squeal();
    impl_->rumble_pcm = synth_rumble();
    impl_->horn_pcm = synth_horn();

    // Chaque clac du pool a son propre audio_buffer (curseur indépendant) mais
    // partage les mêmes données PCM (référencées, non copiées).
    bool ok = true;
    for (ma_audio_buffer& buf : impl_->clack_bufs) {
        ok = ok && Impl::make_buffer(impl_->clack_pcm, buf);
    }
    ok = ok && Impl::make_buffer(impl_->squeal_pcm, impl_->squeal_buf);
    ok = ok && Impl::make_buffer(impl_->rumble_pcm, impl_->rumble_buf);
    ok = ok && Impl::make_buffer(impl_->horn_pcm, impl_->horn_buf);
    if (!ok) {
        log::error("Audio : échec de création des buffers procéduraux");
        return false;
    }

    for (int i = 0; i < kJointPoolSize; ++i) {
        ma_sound& s = impl_->joint_pool[static_cast<std::size_t>(i)];
        ma_sound_init_from_data_source(&impl_->engine, &impl_->clack_bufs[static_cast<std::size_t>(i)],
                                       0, nullptr, &s);
        ma_sound_set_doppler_factor(&s, 1.0f);
    }

    ma_sound_init_from_data_source(&impl_->engine, &impl_->squeal_buf, 0, nullptr,
                                   &impl_->squeal_sound);
    ma_sound_set_looping(&impl_->squeal_sound, MA_TRUE);
    ma_sound_set_doppler_factor(&impl_->squeal_sound, 1.0f);
    ma_sound_set_volume(&impl_->squeal_sound, 0.0f);
    ma_sound_start(&impl_->squeal_sound);

    ma_sound_init_from_data_source(&impl_->engine, &impl_->rumble_buf, 0, nullptr,
                                   &impl_->rumble_sound);
    ma_sound_set_looping(&impl_->rumble_sound, MA_TRUE);
    ma_sound_set_doppler_factor(&impl_->rumble_sound, 1.0f);
    ma_sound_set_volume(&impl_->rumble_sound, 0.0f);
    ma_sound_start(&impl_->rumble_sound);

    // Sifflet (M14) : même schéma — boucle continue, muette au repos, Doppler activé.
    ma_sound_init_from_data_source(&impl_->engine, &impl_->horn_buf, 0, nullptr,
                                   &impl_->horn_sound);
    ma_sound_set_looping(&impl_->horn_sound, MA_TRUE);
    ma_sound_set_doppler_factor(&impl_->horn_sound, 1.0f);
    ma_sound_set_volume(&impl_->horn_sound, 0.0f);
    ma_sound_start(&impl_->horn_sound);

    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 1.0f, 0.0f);

    impl_->sounds_ok = true;
    log::info("Audio : miniaudio initialisé ({} Hz, spatialisation + Doppler)",
              ma_engine_get_sample_rate(&impl_->engine));
    return true;
}

void AudioEngine::shutdown() {
    if (!impl_) {
        return;
    }
    if (impl_->sounds_ok) {
        for (ma_sound& s : impl_->joint_pool) {
            ma_sound_uninit(&s);
        }
        ma_sound_uninit(&impl_->squeal_sound);
        ma_sound_uninit(&impl_->rumble_sound);
        ma_sound_uninit(&impl_->horn_sound);
        for (ma_audio_buffer& b : impl_->clack_bufs) {
            ma_audio_buffer_uninit(&b);
        }
        ma_audio_buffer_uninit(&impl_->squeal_buf);
        ma_audio_buffer_uninit(&impl_->rumble_buf);
        ma_audio_buffer_uninit(&impl_->horn_buf);
    }
    if (impl_->engine_ok) {
        ma_engine_uninit(&impl_->engine);
    }
    impl_.reset();
}

void AudioEngine::update_listener(const WorldPosition& position, const glm::vec3& velocity,
                                  const glm::vec3& forward, const glm::vec3& up) {
    if (!valid()) {
        return;
    }
    // Listener maintenu à l'origine : les émetteurs sont placés en relatif.
    impl_->listener_pos = position;
    ma_engine_listener_set_position(&impl_->engine, 0, 0.0f, 0.0f, 0.0f);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
    ma_engine_listener_set_velocity(&impl_->engine, 0, velocity.x, velocity.y, velocity.z);
}

void AudioEngine::set_rumble(const WorldPosition& position, const glm::vec3& velocity, float volume,
                             float pitch) {
    if (!valid()) {
        return;
    }
    const glm::vec3 rel = impl_->relative(position);
    ma_sound_set_position(&impl_->rumble_sound, rel.x, rel.y, rel.z);
    ma_sound_set_velocity(&impl_->rumble_sound, velocity.x, velocity.y, velocity.z);
    ma_sound_set_volume(&impl_->rumble_sound, volume);
    ma_sound_set_pitch(&impl_->rumble_sound, pitch);
}

void AudioEngine::set_squeal(const WorldPosition& position, const glm::vec3& velocity,
                             float volume) {
    if (!valid()) {
        return;
    }
    const glm::vec3 rel = impl_->relative(position);
    ma_sound_set_position(&impl_->squeal_sound, rel.x, rel.y, rel.z);
    ma_sound_set_velocity(&impl_->squeal_sound, velocity.x, velocity.y, velocity.z);
    ma_sound_set_volume(&impl_->squeal_sound, volume);
}

void AudioEngine::set_horn(const WorldPosition& position, const glm::vec3& velocity, float volume) {
    if (!valid()) {
        return;
    }
    const glm::vec3 rel = impl_->relative(position);
    ma_sound_set_position(&impl_->horn_sound, rel.x, rel.y, rel.z);
    ma_sound_set_velocity(&impl_->horn_sound, velocity.x, velocity.y, velocity.z);
    ma_sound_set_volume(&impl_->horn_sound, volume);
}

void AudioEngine::play_rail_joint(const WorldPosition& position, const glm::vec3& velocity,
                                  float volume) {
    if (!valid()) {
        return;
    }
    ma_sound& s = impl_->joint_pool[static_cast<std::size_t>(impl_->joint_next)];
    impl_->joint_next = (impl_->joint_next + 1) % kJointPoolSize;

    const glm::vec3 rel = impl_->relative(position);
    ma_sound_set_position(&s, rel.x, rel.y, rel.z);
    ma_sound_set_velocity(&s, velocity.x, velocity.y, velocity.z);
    ma_sound_set_volume(&s, volume);
    ma_sound_seek_to_pcm_frame(&s, 0);
    ma_sound_start(&s);
}

bool AudioEngine::set_source(Emitter emitter, const float* pcm, std::size_t frame_count) {
    if (!valid() || pcm == nullptr || frame_count == 0) {
        return false;
    }
    switch (emitter) {
        case Emitter::Rumble:
            return impl_->rebuild_loop(impl_->rumble_sound, impl_->rumble_buf, impl_->rumble_pcm,
                                       pcm, frame_count);
        case Emitter::Squeal:
            return impl_->rebuild_loop(impl_->squeal_sound, impl_->squeal_buf, impl_->squeal_pcm,
                                       pcm, frame_count);
        case Emitter::Joint:
            return impl_->rebuild_joint(pcm, frame_count);
    }
    return false;
}

}  // namespace noire::audio
