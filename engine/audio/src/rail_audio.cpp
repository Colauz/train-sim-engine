#include "noire/audio/rail_audio.hpp"

#include <cmath>

#include "noire/audio/audio_engine.hpp"

namespace noire::audio {

void RailAudio::update(AudioEngine& audio, double dt, const Input& in) {
    const long front_joint = static_cast<long>(std::floor(in.front_chainage / rail_length_));
    const long rear_joint = static_cast<long>(std::floor(in.rear_chainage / rail_length_));
    if (!initialized_) {
        last_front_joint_ = front_joint;
        last_rear_joint_ = rear_joint;
        initialized_ = true;
    }

    const float speed_kmh = static_cast<float>(in.speed * 3.6);
    const float joint_volume = glm::clamp(speed_kmh / 60.0f, 0.15f, 1.0f);

    // Un « clac » chaque fois qu'un bogie franchit un joint (les deux essieux
    // donnent le caractéristique « ta-dam »).
    if (in.speed > 0.5) {
        if (front_joint != last_front_joint_) {
            audio.play_rail_joint(in.front_position, in.velocity, joint_volume);
        }
        if (rear_joint != last_rear_joint_) {
            audio.play_rail_joint(in.rear_position, in.velocity, joint_volume * 0.9f);
        }
    }
    last_front_joint_ = front_joint;
    last_rear_joint_ = rear_joint;

    // Crissement : accélération latérale = v² · courbure (= v²/R). Seuil puis montée.
    const float lateral_accel = static_cast<float>(std::abs(in.curvature) * in.speed * in.speed);
    const float target = glm::clamp((lateral_accel - 0.4f) / 3.0f, 0.0f, 1.0f);
    const float smoothing = glm::clamp(static_cast<float>(dt) * 4.0f, 0.0f, 1.0f);
    squeal_level_ += (target - squeal_level_) * smoothing;
    audio.set_squeal(in.body_position, in.velocity, squeal_level_ * 0.6f);

    // Roulement : présent dès qu'on roule, plus fort et plus aigu avec la vitesse.
    const float rumble_volume = glm::clamp(speed_kmh / 120.0f, 0.0f, 0.7f);
    const float rumble_pitch = 0.7f + glm::clamp(speed_kmh / 300.0f, 0.0f, 1.0f) * 0.8f;
    audio.set_rumble(in.body_position, in.velocity, rumble_volume, rumble_pitch);
}

}  // namespace noire::audio
