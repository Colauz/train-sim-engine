#pragma once

#include "noire/core/math.hpp"

namespace noire::physics {

// Réglages de la caisse (suspension). Repris tels quels de WagonConfig (le comportement
// de la motrice ne change pas), plus le roll ajouté au M16.
struct CarBodyConfig {
    double body_height = 2.2;      // hauteur du centre de caisse au-dessus du plan de roulement

    double heave_frequency = 1.2;  // pilonnement (Hz)
    double heave_damping = 0.30;

    // TANGAGE PAR TRANSFERT DE CHARGE (M17.6) — plus une rotation libre autour du centre
    // (qui arrachait la caisse de ses bogies), mais une DIFFÉRENCE DE HAUTEUR entre l'appui
    // avant et l'appui arrière. La caisse est tendue entre ses deux appuis : elle ne peut
    // donc jamais les quitter, et le tangage est borné par la COURSE de suspension.
    double pitch_frequency = 1.4;  // dynamique du transfert (Hz)
    double pitch_damping = 0.35;
    // Course d'appui (m) par m/s² d'accélération : à un freinage d'urgence (~1,2 m/s²) l'appui
    // se déplace de ~7 cm, soit ~0,6° de tangage sur l'empattement moteur — subtil et réel.
    double pitch_transfer = 0.06;
    // BUTOIR DUR sur la course d'appui (m) : la suspension ne débat jamais au-delà, donc la
    // caisse reste toujours à ~body_height de ses bogies. C'est ce qui interdit le « wheelie ».
    double max_pitch_travel = 0.10;

    // Roll (M16) : petite rotation autour de l'axe LONG, gardée telle quelle. Les flancs ne
    // sont qu'à ~1,5 m de l'axe (les bogies sont sur la ligne médiane) : 5° ne lève un flanc
    // que de ~13 cm et n'arrache rien. C'est une souplesse ∝ accélération latérale.
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
    double body_y_ = 0.0;         // hauteur filtrée du centre de caisse (pilonnement)
    double body_vy_ = 0.0;
    double prev_support_y_ = 0.0;
    // pitch_ n'est PLUS un angle (M17.6) : c'est la demi-course d'appui, en MÈTRES. L'appui
    // avant monte de pitch_, l'arrière descend de pitch_ (ou l'inverse). Le tangage en
    // découle géométriquement, borné par la course => la caisse ne quitte jamais ses bogies.
    double pitch_ = 0.0;
    double pitch_vel_ = 0.0;
    double roll_ = 0.0;           // roll : reste un angle (rad)
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
