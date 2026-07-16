#pragma once

#include "noire/core/math.hpp"

namespace noire::physics {

// Réglages de la caisse (suspension). Repris tels quels de WagonConfig (le comportement
// de la motrice ne change pas), plus le roll ajouté au M16.
struct CarBodyConfig {
    double body_height = 2.2;      // hauteur du centre de caisse au-dessus du plan de roulement

    double heave_frequency = 1.2;  // pilonnement (Hz)
    double heave_damping = 0.30;
    double pitch_frequency = 1.4;  // tangage (Hz)
    double pitch_damping = 0.35;
    // rad de tangage par m/s² d'accélération longitudinale. VOLONTAIREMENT FAIBLE (M17.5) :
    // le tangage d'un TGV est infime (~1°). À 0,02, un freinage d'urgence (~1,2 m/s²) vise
    // ~1,4° ; l'oscillateur sous-amorti dépasse un peu, le clamp (max_pitch) coupe le reste.
    double pitch_gain = 0.02;
    // BUTOIR DUR sur le tangage : la caisse ne peut PAS cabrer au-delà, quelle que soit
    // l'accélération. 2° = 0,035 rad. C'est ce qui interdit le « wheelie » (M17.5).
    double max_pitch = 0.035;

    // Roll (M16) : la caisse s'incline en courbe. La voie n'a PAS de dévers modélisé, donc
    // ce roll est une souplesse de suspension ∝ accélération latérale (v²·courbure) —
    // volontairement subtile. Filtré comme le tangage.
    double roll_frequency = 1.1;
    double roll_damping = 0.35;
    double roll_gain = 0.015;      // rad de roll par m/s² d'accélération latérale
    double max_roll = 0.087;       // butoir dur : 5°
};

// CAISSE RIGIDE tendue entre DEUX bogies — le cœur de la cinématique inverse ferroviaire.
//
// Elle ne connaît QUE deux positions de bogie. Le YAW (courbe) et le PITCH géométrique
// (pente) en sortent directement : la caisse est le segment reliant les deux centres de
// bogie, son axe long est front - rear. Par-dessus, trois oscillateurs de suspension
// (pilonnement, tangage dynamique, roulis) donnent la vie.
//
// RÉUTILISÉE par la motrice (Wagon) ET par chaque voiture voyageurs (Consist) : c'est la
// même physique de caisse, qu'elle repose sur deux bogies classiques ou sur deux bogies
// Jacobs partagés. C'est ce qui rend l'articulation « saine » — un seul modèle de caisse.
class CarBody {
public:
    explicit CarBody(CarBodyConfig config = {}) : config_(config) {}

    // `front_pos`/`rear_pos` : centres des deux bogies porteurs (monde, double).
    // `longitudinal_accel` : pour le tangage (plonge au freinage, cabre à l'accélération).
    // `lateral_accel` : pour le roll (v²·courbure). 0 => pas de roulis.
    void update(const glm::dvec3& front_pos, const glm::dvec3& rear_pos, double dt,
                double longitudinal_accel, double lateral_accel);

    [[nodiscard]] const WorldPosition& position() const { return position_; }
    [[nodiscard]] const glm::mat4& orientation() const { return orientation_; }
    [[nodiscard]] const CarBodyConfig& config() const { return config_; }

private:
    CarBodyConfig config_;

    bool ready_ = false;
    double body_y_ = 0.0;
    double body_vy_ = 0.0;
    double prev_support_y_ = 0.0;
    double pitch_ = 0.0;
    double pitch_vel_ = 0.0;
    double roll_ = 0.0;
    double roll_vel_ = 0.0;

    WorldPosition position_{0.0, 0.0, 0.0};
    glm::mat4 orientation_{1.0f};
};

// Accélération latérale SIGNÉE (m/s²) d'une caisse, depuis les tangentes de ses deux
// bogies, la vitesse et leur écartement d'arc. C'est v²·courbure, avec un signe donné par
// le sens du virage. Sert le roll (CarBody::update). Partagée par la motrice et les
// voitures pour que toute la rame s'incline de façon cohérente dans une même courbe.
[[nodiscard]] double signed_lateral_accel(const glm::dvec3& front_tangent,
                                          const glm::dvec3& rear_tangent, double speed,
                                          double span);

}  // namespace noire::physics
