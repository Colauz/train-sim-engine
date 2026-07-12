#pragma once

#include "noire/core/math.hpp"
#include "noire/core/spline.hpp"
#include "noire/physics/bogie.hpp"

namespace noire::physics {

// Paramètres du wagon (au niveau namespace : cf. convention EngineConfig).
struct WagonConfig {
    double mass = 40000.0;                // kg (~40 t)
    double wheelbase = 12.0;              // m (empattement entre bogies)

    double max_tractive_effort = 150000.0;  // N à pleine puissance
    double max_brake_force = 180000.0;       // N au freinage max

    // Adhérence acier/acier : statique (avant patinage) vs cinétique (une fois qu'on patine).
    double adhesion_static = 0.33;
    double adhesion_kinetic = 0.18;

    // Résistance à l'avancement (Davis simplifié) : constante + aérodynamique.
    double rolling_c0 = 800.0;   // N
    double rolling_c2 = 8.0;     // N/(m/s)^2

    double body_height = 2.2;    // hauteur du centre de la caisse au-dessus des bogies (m)

    // Suspension (ressort/amortisseur 1D) : pilonnement vertical + tangage.
    double heave_frequency = 1.2;   // Hz
    double heave_damping = 0.30;    // ratio d'amortissement
    double pitch_frequency = 1.4;   // Hz
    double pitch_damping = 0.35;
    double pitch_gain = 0.03;       // rad de tangage par (m/s^2) d'accélération
};

// Wagon = système MULTI-CORPS : 2 bogies (avant/arrière) + 1 caisse.
//   * l'état longitudinal (distance s, vitesse v) évolue par F = m·a ;
//   * les bogies sont placés à s ± empattement/2 sur la voie ;
//   * la caisse dérive sa pose des deux bogies, filtrée par la suspension.
class Wagon {
public:
    explicit Wagon(WagonConfig config = {}) : config_(config) {}

    void attach(const Spline* track) { track_ = track; }
    void place_at(double distance);                     // initialise l'abscisse + les bogies
    void set_controls(double throttle, double brake);   // manettes dans [0, 1]

    void update(double dt);  // dynamique complète — à appeler dans fixed_update

    [[nodiscard]] const Bogie& front_bogie() const { return front_; }
    [[nodiscard]] const Bogie& rear_bogie() const { return rear_; }
    [[nodiscard]] const WorldPosition& body_position() const { return body_position_; }
    [[nodiscard]] const glm::mat4& body_orientation() const { return body_orientation_; }

    [[nodiscard]] double speed() const { return velocity_; }        // m/s
    [[nodiscard]] double distance() const { return distance_; }
    [[nodiscard]] double tractive_effort() const { return tractive_effort_; }  // N réellement transmis
    [[nodiscard]] double grade_percent() const { return grade_percent_; }
    [[nodiscard]] bool slipping() const { return slipping_; }
    [[nodiscard]] const WagonConfig& config() const { return config_; }

private:
    void update_body(double dt, double longitudinal_accel);

    WagonConfig config_;
    const Spline* track_ = nullptr;

    // État longitudinal
    double distance_ = 0.0;   // abscisse du centre du wagon (m)
    double velocity_ = 0.0;   // m/s
    double throttle_ = 0.0;   // manette lissée [0,1]
    double throttle_input_ = 0.0;
    double brake_ = 0.0;

    // Sorties / diagnostics
    bool slipping_ = false;
    double tractive_effort_ = 0.0;
    double grade_percent_ = 0.0;

    Bogie front_;
    Bogie rear_;
    WorldPosition body_position_{0.0, 0.0, 0.0};
    glm::mat4 body_orientation_{1.0f};

    // État de la suspension
    bool suspension_ready_ = false;
    double body_y_ = 0.0;        // hauteur inertielle de la caisse (heave)
    double body_vy_ = 0.0;
    double prev_support_y_ = 0.0;
    double pitch_ = 0.0;         // tangage (rad)
    double pitch_vel_ = 0.0;
};

}  // namespace noire::physics
