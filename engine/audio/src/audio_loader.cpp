#include "noire/audio/audio_loader.hpp"

#include <cstddef>
#include <vector>

#include <miniaudio.h>

#include "noire/core/log.hpp"

namespace noire::audio {

bool decode_audio_file(const std::string& path, std::vector<float>& out_pcm) {
    // Force mono float32 à 48 kHz : miniaudio downmixe et rééchantillonne au besoin.
    ma_decoder_config config =
        ma_decoder_config_init(ma_format_f32, 1, static_cast<ma_uint32>(kAudioSampleRate));

    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        log::warn("Audio : décodage de '{}' échoué (introuvable ou format non supporté)", path);
        return false;
    }

    out_pcm.clear();
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0) {
        // Longueur connue (WAV/FLAC/...) : une seule lecture.
        out_pcm.resize(static_cast<std::size_t>(length));  // mono => 1 échantillon par frame
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&decoder, out_pcm.data(), length, &read);
        out_pcm.resize(static_cast<std::size_t>(read));
    } else {
        // Longueur inconnue (certains flux MP3) : lecture par blocs d'une seconde.
        constexpr ma_uint64 kChunk = static_cast<ma_uint64>(kAudioSampleRate);
        std::vector<float> chunk(static_cast<std::size_t>(kChunk));
        for (;;) {
            ma_uint64 read = 0;
            const ma_result result =
                ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunk, &read);
            out_pcm.insert(out_pcm.end(), chunk.begin(),
                           chunk.begin() + static_cast<std::ptrdiff_t>(read));
            if (result != MA_SUCCESS || read < kChunk) {
                break;
            }
        }
    }

    ma_decoder_uninit(&decoder);

    if (out_pcm.empty()) {
        log::warn("Audio : '{}' vide après décodage", path);
        return false;
    }
    log::info("Audio : '{}' décodé — {} frames ({:.2f} s, mono 48 kHz)", path, out_pcm.size(),
              static_cast<double>(out_pcm.size()) / static_cast<double>(kAudioSampleRate));
    return true;
}

}  // namespace noire::audio
