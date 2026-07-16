#pragma once

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/physics/air_brake.hpp"
#include "noire/physics/bogie.hpp"
#include "noire/physics/car_body.hpp"

namespace noire::physics {

struct WagonConfig {
    double mass = 40000.0;
    // Masse portée par les essieux MOTEURS — c'est elle, et non la masse totale, qui
    // décide de l'adhérence EN TRACTION. Sur une rame TGV, seules les deux motrices sont
    // motrices (~136 t sur 400 t) : compter les 400 t rendrait le patinage impossible.
    // Le FREINAGE, lui, reste sur la masse totale (tous les essieux sont freinés).
    // 0 => masse totale (engin dont tous les essieux sont moteurs).
    double adhesive_mass = 0.0;
    double wheelbase = 12.0;

    double max_tractive_effort = 150000.0;
    // Puissance maximale à la jante (W). Au-delà de la vitesse de base
    // v_base = max_power / max_tractive_effort, l'effort disponible n'est plus l'effort
    // maximal mais P/v : c'est l'HYPERBOLE DE PUISSANCE, et c'est elle qui donne son
    // inertie à un train lancé. Sans elle, un engin garderait son effort de démarrage à
    // 300 km/h et accélérerait comme au départ.
    // 0 => effort constant à toute vitesse (comportement d'avant le M13).
    double max_power = 0.0;
    double max_brake_force = 180000.0;

    double adhesion_static = 0.33;
    double adhesion_kinetic = 0.18;

    // Résistance à l'avancement de DAVIS : R(v) = a + b*|v| + c*v² (N, v en m/s).
    //   a : roulement et frottements secs, indépendants de la vitesse
    //   b : résistance mécanique/rail, proportionnelle à la vitesse
    //   c : TRAÎNÉE AÉRODYNAMIQUE — le terme qui domine tout à grande vitesse
    // Le terme linéaire manquait avant le M13 (le modèle n'avait que a et c).
    double davis_a = 800.0;
    double davis_b = 0.0;
    double davis_c = 8.0;

    double body_height = 2.2;

    double heave_frequency = 1.2;
    double heave_damping = 0.30;
    double pitch_frequency = 1.4;
    double pitch_damping = 0.35;
    double pitch_gain = 0.02;  // faible : le tangage TGV est ~1° (cf. M17.5, + butoir CarBody)
};

// Dérive les réglages de caisse depuis la config du wagon. Le roll (M16) prend ses
// défauts : la motrice s'incline donc comme les voitures, d'un même mouvement.
[[nodiscard]] inline CarBodyConfig to_car_body_config(const WagonConfig& c) {
    CarBodyConfig b;
    b.body_height = c.body_height;
    b.heave_frequency = c.heave_frequency;
    b.heave_damping = c.heave_damping;
    b.pitch_frequency = c.pitch_frequency;
    b.pitch_damping = c.pitch_damping;
    b.pitch_gain = c.pitch_gain;
    return b;
}

// Wagon multi-corps (2 bogies + caisse) posé sur une voie INFINIE (TrackSource).
// L'état longitudinal est un « chainage » x (paramètre le long de la voie) et une
// vitesse v (le long de l'arc). La dynamique reste identique au M4 ; seule la
// conversion vitesse<->avance passe par le taux d'arc (voie non paramétrée en
// longueur d'arc). Plus aucune borne : la voie ne finit jamais.
class Wagon {
public:
    explicit Wagon(WagonConfig config = {})
        : config_(config), caisse_(to_car_body_config(config)) {}

    void attach(const TrackSource* track) { track_ = track; }
    void place_at(double chainage);
    void set_speed(double meters_per_second) { velocity_ = meters_per_second; }
    // `brake` = position du robinet 0..1 ; `emergency` court-circuite le service.
    void set_controls(double throttle, double brake, bool emergency = false);
    // Échelle d'adhérence (1 = sec). Couple la météo à la tenue du rail : sous la pluie,
    // l'adhérence chute et le patinage devient possible. Multiplie les deux coefficients.
    void set_adhesion_scale(double scale) { adhesion_scale_ = scale; }

    void update(double dt);  // fixed_update — déterministe

    [[nodiscard]] const Bogie& front_bogie() const { return front_; }
    [[nodiscard]] const Bogie& rear_bogie() const { return rear_; }
    [[nodiscard]] const WorldPosition& body_position() const { return caisse_.position(); }
    [[nodiscard]] const glm::mat4& body_orientation() const { return caisse_.orientation(); }

    [[nodiscard]] double speed() const { return velocity_; }
    [[nodiscard]] double chainage() const { return chainage_; }
    // Traction RÉELLEMENT appliquée (0..1), après la rampe de la chaîne de traction —
    // à ne pas confondre avec la position du manipulateur, qui est la consigne.
    [[nodiscard]] double throttle() const { return throttle_; }
    [[nodiscard]] double tractive_effort() const { return tractive_effort_; }
    [[nodiscard]] double grade_percent() const { return grade_percent_; }
    [[nodiscard]] bool slipping() const { return slipping_; }
    [[nodiscard]] const WagonConfig& config() const { return config_; }
    [[nodiscard]] const AirBrake& air_brake() const { return air_brake_; }

private:
    WagonConfig config_;
    const TrackSource* track_ = nullptr;

    double chainage_ = 0.0;
    double velocity_ = 0.0;
    double throttle_ = 0.0;
    double throttle_input_ = 0.0;
    double adhesion_scale_ = 1.0;
    AirBrake air_brake_;

    bool slipping_ = false;
    double tractive_effort_ = 0.0;
    double grade_percent_ = 0.0;

    Bogie front_;
    Bogie rear_;
    CarBody caisse_;  // caisse de la motrice — même modèle que les voitures (M16)
};

}  // namespace noire::physics
