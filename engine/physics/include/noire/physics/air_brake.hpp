#pragma once

#include <cstddef>
#include <vector>

namespace noire::physics {

// FREIN PNEUMATIQUE À CONDUITE GÉNÉRALE (M13).
//
// LE FAIT QUI COMMANDE TOUT : sur un train, le frein n'est pas une commande, c'est une
// ONDE. Le mécanicien ne serre pas les sabots — il ouvre une vanne qui fait CHUTER la
// pression dans un tuyau de 200 m, et chaque véhicule freine quand la dépression lui
// parvient. D'où un enchaînement que rien ne court-circuite :
//
//   consigne -> TEMPS MORT (propagation) -> pression CG (chute à débit limité)
//            -> cylindres (remplissage retardé) -> effort
//
// C'est cette chaîne, et pas un coefficient de « mollesse », qui donne au freinage
// ferroviaire sa signature : on tire le robinet, et il ne se passe RIEN pendant un temps,
// puis l'effort monte tout seul et met plusieurs secondes à s'établir.
struct AirBrakeConfig {
    double nominal_pressure = 5.0;   // bar — CG chargée, freins desserrés
    double full_service_drop = 1.5;  // bar — dépression au freinage de service maximal

    // Débits de la CG. La recharge est plus LENTE que la vidange : vider un tuyau à
    // l'atmosphère est facile, le remplir demande au compresseur de suivre. C'est
    // pourquoi un mécanicien ne « pompe » pas ses freins — il ne les récupérerait pas.
    double apply_rate = 0.35;    // bar/s — vidange (serrage)
    double release_rate = 0.20;  // bar/s — recharge (desserrage)

    double cylinder_tau = 0.8;  // s — constante de temps de remplissage des cylindres

    // Propagation de l'onde de dépression le long de la rame.
    double propagation_speed = 250.0;  // m/s — célérité dans la conduite
    double train_length = 200.0;       // m — longueur de la RAME (pas l'empattement !)

    // Urgence : la CG est vidée intégralement, et par des valves réparties le long de la
    // rame — d'où un débit bien supérieur et un temps mort presque nul.
    double emergency_rate = 2.5;   // bar/s
    double emergency_delay = 0.15; // s
    // Effort d'urgence rapporté au service maximal (= 1.0). 1,6 => 480 kN sur 400 t,
    // soit ~1,2 m/s² : l'ordre de grandeur d'un freinage d'urgence réel.
    double emergency_ratio = 1.6;

    // Pas de la simulation, en secondes. Il DOIT valoir le pas fixe de l'Engine : c'est
    // lui qui dimensionne la ligne à retard. Le donner ici plutôt que de le déduire du
    // premier update() évite qu'un changement de simulation_hz ne fausse le temps mort en
    // silence — AirBrake préviendra si un update() ne le respecte pas.
    double fixed_dt = 1.0 / 120.0;
};

class AirBrake {
public:
    explicit AirBrake(AirBrakeConfig config = {});

    // `demand` = position du robinet, 0..1 (1 = service maximal). `emergency` court-circuite
    // le service : la CG tombe à zéro.
    void set_handle(double demand, bool emergency);
    void update(double dt);  // fixed_update — déterministe

    // Pression de la conduite générale, en bar. C'est CE que lit le mécanicien.
    [[nodiscard]] double pipe_pressure() const { return pipe_pressure_; }
    // Fraction de l'effort de freinage maximal (de service) à appliquer. Dépasse 1.0 en
    // urgence, jusqu'à emergency_ratio.
    [[nodiscard]] double force_fraction() const { return cylinder_; }
    [[nodiscard]] bool emergency() const { return emergency_; }
    [[nodiscard]] const AirBrakeConfig& config() const { return config_; }

private:
    AirBrakeConfig config_;

    // Ligne à retard de la PROPAGATION. Elle mémorise la DÉPRESSION commandée, jamais la
    // pression absolue : initialisée à zéro, une ligne de dépressions vaut « rien de
    // demandé » (l'état de repos), alors qu'une ligne de pressions vaudrait « 0 bar dans
    // la CG », c'est-à-dire un freinage d'urgence à chaque démarrage.
    std::vector<double> delay_line_;
    std::size_t head_ = 0;
    std::size_t tap_service_ = 0;    // prises de lecture, en nombre d'échantillons
    std::size_t tap_emergency_ = 0;

    double demand_ = 0.0;
    bool emergency_ = false;

    double pipe_pressure_ = 0.0;  // bar — initialisée à nominal_pressure au constructeur
    double cylinder_ = 0.0;       // 0..emergency_ratio

    bool dt_warned_ = false;
};

}  // namespace noire::physics
