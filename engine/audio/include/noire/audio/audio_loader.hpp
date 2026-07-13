#pragma once

#include <string>
#include <vector>

namespace noire::audio {

// Échantillonnage cible : mono float32 à 48 kHz (aligné sur l'AudioEngine, donc
// directement injectable dans le chemin ma_audio_buffer sans conversion).
constexpr int kAudioSampleRate = 48000;

// Décode un fichier audio (.wav / .mp3 / .flac / ...) en PCM mono float32 à 48 kHz.
// Rééchantillonnage + downmix gérés par miniaudio (ma_decoder). AUCUN périphérique
// audio requis : c'est du pur décodage CPU, exécutable sur un worker du JobSystem.
// false si le fichier est introuvable ou illisible.
bool decode_audio_file(const std::string& path, std::vector<float>& out_pcm);

}  // namespace noire::audio
