#pragma once

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/physics/bogie.hpp"

namespace noire::physics {

struct WagonConfig {
    double mass = 40000.0;
    double wheelbase = 12.0;

    double max_tractive_effort = 150000.0;
    double max_brake_force = 180000.0;

    double adhesion_static = 0.33;
    double adhesion_kinetic = 0.18;

    double rolling_c0 = 800.0;
    double rolling_c2 = 8.0;

    double body_height = 2.2;

    double heave_frequency = 1.2;
    double heave_damping = 0.30;
    double pitch_frequency = 1.4;
    double pitch_damping = 0.35;
    double pitch_gain = 0.03;
};

// Wagon multi-corps (2 bogies + caisse) posé sur une voie INFINIE (TrackSource).
// L'état longitudinal est un « chainage » x (paramètre le long de la voie) et une
// vitesse v (le long de l'arc). La dynamique reste identique au M4 ; seule la
// conversion vitesse<->avance passe par le taux d'arc (voie non paramétrée en
// longueur d'arc). Plus aucune borne : la voie ne finit jamais.
class Wagon {
public:
    explicit Wagon(WagonConfig config = {}) : config_(config) {}

    void attach(const TrackSource* track) { track_ = track; }
    void place_at(double chainage);
    void set_speed(double meters_per_second) { velocity_ = meters_per_second; }
    void set_controls(double throttle, double brake);

    void update(double dt);  // fixed_update — déterministe

    [[nodiscard]] const Bogie& front_bogie() const { return front_; }
    [[nodiscard]] const Bogie& rear_bogie() const { return rear_; }
    [[nodiscard]] const WorldPosition& body_position() const { return body_position_; }
    [[nodiscard]] const glm::mat4& body_orientation() const { return body_orientation_; }

    [[nodiscard]] double speed() const { return velocity_; }
    [[nodiscard]] double chainage() const { return chainage_; }
    [[nodiscard]] double tractive_effort() const { return tractive_effort_; }
    [[nodiscard]] double grade_percent() const { return grade_percent_; }
    [[nodiscard]] bool slipping() const { return slipping_; }
    [[nodiscard]] const WagonConfig& config() const { return config_; }

private:
    void update_body(double dt, double longitudinal_accel);

    WagonConfig config_;
    const TrackSource* track_ = nullptr;

    double chainage_ = 0.0;
    double velocity_ = 0.0;
    double throttle_ = 0.0;
    double throttle_input_ = 0.0;
    double brake_ = 0.0;

    bool slipping_ = false;
    double tractive_effort_ = 0.0;
    double grade_percent_ = 0.0;

    Bogie front_;
    Bogie rear_;
    WorldPosition body_position_{0.0, 0.0, 0.0};
    glm::mat4 body_orientation_{1.0f};

    bool suspension_ready_ = false;
    double body_y_ = 0.0;
    double body_vy_ = 0.0;
    double prev_support_y_ = 0.0;
    double pitch_ = 0.0;
    double pitch_vel_ = 0.0;
};

}  // namespace noire::physics
