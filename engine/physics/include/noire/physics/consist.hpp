#pragma once

#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/speed_limits.hpp"
#include "noire/core/track_source.hpp"
#include "noire/physics/bogie.hpp"
#include "noire/physics/car_body.hpp"
#include "noire/physics/wagon.hpp"

namespace noire::physics {

// RAME ARTICULÉE (M16) : motrice de tête + N voitures voyageurs sur bogies Jacobs.
//
// HIÉRARCHIE (cf. le fil du jalon) :
//   * la PHYSIQUE LONGITUDINALE est UNIQUE — c'est le Wagon de tête (loco_), dont la masse
//     représente déjà toute la rame. Il porte chainage + vitesse + traction + frein.
//   * les BOGIES sont placés à des distances d'ARC FIXES derrière la tête, chaque tick :
//     rien n'est intégré par voiture, donc AUCUN décalage temporel entre les caisses.
//   * un bogie Jacobs jacobs_[k] est PARTAGÉ : arrière de car[k-1], avant de car[k]. Il
//     est possédé une seule fois (vecteur plat) et référencé par INDEX — pas de pointeur
//     croisé entre voitures. Pour N voitures : N+1 bogies Jacobs.
//   * chaque caisse (CarBody) est la cinématique inverse entre ses deux bogies porteurs.
struct ConsistConfig {
    int car_count = 2;              // N voitures voyageurs

    double loco_half_length = 11.075;  // demi-longueur de la motrice (22.15 m)
    double car_spacing = 24.0;      // distance d'arc entre bogies Jacobs consécutifs (= longueur voiture)
    double head_to_first_jacobs = 1.0;  // jeu d'arc entre l'arrière motrice et le 1er Jacobs

    CarBodyConfig car_body;         // suspension des voitures (défauts CarBodyConfig)

    // KVB (M17) : marge de tolérance avant déclenchement de l'urgence. Dépasser la limite
    // de plus de kvb_margin_kmh arme le freinage d'urgence automatique.
    double kvb_margin_kmh = 10.0;
};

class Consist {
public:
    explicit Consist(WagonConfig loco_config = {}, ConsistConfig config = {});

    void attach(const TrackSource* track);
    void place_at(double chainage);
    void set_speed(double meters_per_second) { loco_.set_speed(meters_per_second); }
    // MÉMORISE la consigne du conducteur. Elle n'est appliquée qu'en update(), APRÈS le KVB
    // — c'est ce qui permet au KVB de la court-circuiter (couper la traction, forcer
    // l'urgence) sans que l'app ait à le savoir.
    void set_controls(double throttle, double brake, bool emergency = false) {
        throttle_cmd_ = throttle;
        brake_cmd_ = brake;
        emergency_cmd_ = emergency;
    }
    void set_adhesion_scale(double scale) { loco_.set_adhesion_scale(scale); }

    void update(double dt);  // fixed_update — déterministe

    // --- Accès pour le rendu / le reste de l'app ------------------------------
    [[nodiscard]] Wagon& loco() { return loco_; }
    [[nodiscard]] const Wagon& loco() const { return loco_; }
    [[nodiscard]] int car_count() const { return config_.car_count; }
    // Caisse d'une voiture voyageurs (0..N-1).
    [[nodiscard]] const CarBody& car(int i) const { return cars_[static_cast<std::size_t>(i)]; }
    // Bogies Jacobs (0..N), pour les dessiner.
    [[nodiscard]] const std::vector<Bogie>& jacobs_bogies() const { return jacobs_; }

    // --- KVB (M17) ------------------------------------------------------------
    [[nodiscard]] const SpeedLimits& speed_limits() const { return limits_; }
    // Limite applicable à la position actuelle du train (km/h).
    [[nodiscard]] double current_limit_kmh() const { return current_limit_kmh_; }
    // true tant que le KVB force le freinage d'urgence (dépassement > marge, non résorbé).
    [[nodiscard]] bool kvb_active() const { return kvb_active_; }

private:
    // Trouve l'abscisse x telle que la distance d'ARC de x_ref à x (vers l'arrière) vaut
    // `arc_dist`. Point fixe sur arc_rate : garantit une distance inter-bogies RIGOUREUSE
    // même quand la voie n'est pas paramétrée en longueur d'arc.
    [[nodiscard]] double chainage_behind(double x_ref, double arc_dist) const;
    // Place les bogies Jacobs (distances d'arc fixes) puis calcule chaque caisse voyageurs
    // depuis ses deux bogies. Appelé après que la physique de tête a avancé.
    void update_running_gear(double dt, double longitudinal_accel);

    Wagon loco_;
    ConsistConfig config_;
    const TrackSource* track_ = nullptr;

    std::vector<Bogie> jacobs_;   // N+1
    std::vector<CarBody> cars_;   // N
    double prev_velocity_ = 0.0;  // pour l'accélération longitudinale (tangage des voitures)

    // --- KVB (M17) ---
    SpeedLimits limits_;
    // Consigne du conducteur, en attente d'application (cf. set_controls).
    double throttle_cmd_ = 0.0;
    double brake_cmd_ = 0.0;
    bool emergency_cmd_ = false;
    // État courant du contrôle de vitesse.
    double current_limit_kmh_ = 320.0;
    bool kvb_active_ = false;
};

}  // namespace noire::physics
