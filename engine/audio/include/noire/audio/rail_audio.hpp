#pragma once

#include "noire/core/math.hpp"

namespace noire::audio {

class AudioEngine;

// Audio ferroviaire PROCÉDURAL. Ne dépend pas de physics : reçoit des valeurs
// simples (chainages, positions, vitesse, courbure) et pilote l'AudioEngine.
//   * Joints de rail : « clac » déclenché à chaque multiple de la longueur de rail
//     (18 m) franchi => la cadence s'accélère avec la vitesse.
//   * Crissement : proportionnel à l'accélération latérale (v²·courbure) en courbe.
//   * Roulement : volume et hauteur modulés par la vitesse.
class RailAudio {
public:
    struct Input {
        double front_chainage = 0.0;
        WorldPosition front_position{};
        double rear_chainage = 0.0;
        WorldPosition rear_position{};
        WorldPosition body_position{};
        glm::vec3 velocity{0.0f};  // vitesse monde du train (m/s), pour le Doppler
        double speed = 0.0;        // m/s
        double curvature = 0.0;    // 1/R (rad/m)
    };

    void update(AudioEngine& audio, double dt, const Input& in);

    void set_rail_length(double meters) { rail_length_ = meters; }

private:
    double rail_length_ = 18.0;  // longueur standard d'un rail (m)
    long last_front_joint_ = 0;
    long last_rear_joint_ = 0;
    bool initialized_ = false;
    float squeal_level_ = 0.0f;
};

}  // namespace noire::audio
