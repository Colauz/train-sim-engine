#include "noire/physics/consist.hpp"

#include <algorithm>

namespace noire::physics {

Consist::Consist(WagonConfig loco_config, ConsistConfig config)
    : loco_(loco_config), config_(config) {
    const int n = std::max(0, config_.car_count);
    // N voitures => N+1 bogies Jacobs. Un bogie de plus que de voitures, car les extrémités
    // de deux voitures voisines partagent le bogie du milieu.
    jacobs_.resize(static_cast<std::size_t>(n + 1));
    cars_.assign(static_cast<std::size_t>(n), CarBody(config_.car_body));
}

void Consist::attach(const TrackSource* track) {
    track_ = track;
    loco_.attach(track);
}

double Consist::chainage_behind(double x_ref, double arc_dist) const {
    if (track_ == nullptr) {
        return x_ref - arc_dist;
    }
    // arc_dist ≈ arc_rate · (x_ref - x) sur l'intervalle => x = x_ref - arc_dist/arc_rate.
    // On itère en évaluant arc_rate au MILIEU de l'intervalle courant : point fixe qui
    // converge en 2-3 pas et rend la distance d'arc inter-bogies rigoureuse même en courbe.
    double x = x_ref - arc_dist;
    for (int i = 0; i < 4; ++i) {
        const double rate = track_->arc_rate(0.5 * (x_ref + x));
        x = x_ref - arc_dist / (rate > 1e-6 ? rate : 1.0);
    }
    return x;
}

void Consist::update_running_gear(double dt, double longitudinal_accel) {
    if (track_ == nullptr || jacobs_.empty()) {
        return;
    }
    // Tête de référence = centre de caisse de la motrice (son chainage). Le 1er bogie
    // Jacobs est une demi-motrice + un jeu d'articulation derrière.
    const double x_head = loco_.chainage();
    double x = chainage_behind(x_head, config_.loco_half_length + config_.head_to_first_jacobs);
    jacobs_[0].follow(*track_, x);
    // Chaque bogie suivant est placé EXACTEMENT à `car_spacing` d'arc du précédent : c'est
    // ce placement séquentiel qui garantit une distance inter-bogies constante.
    for (std::size_t k = 1; k < jacobs_.size(); ++k) {
        x = chainage_behind(x, config_.car_spacing);
        jacobs_[k].follow(*track_, x);
    }

    // M22 : mise à jour de la cinématique de rotation des roues (v = ω · r)
    for (Bogie& b : jacobs_) {
        b.update_wheels(loco_.speed(), dt);
    }

    // Chaque caisse = cinématique inverse entre ses deux bogies porteurs. jacobs_[k] est
    // vers l'AVANT (chainage plus élevé, côté motrice), jacobs_[k+1] vers l'arrière.
    for (std::size_t k = 0; k < cars_.size(); ++k) {
        const Bogie& front = jacobs_[k];
        const Bogie& rear = jacobs_[k + 1];
        const double lateral = signed_lateral_accel(front.tangent(), rear.tangent(),
                                                    loco_.speed(), config_.car_spacing);
        cars_[k].update(front.position(), rear.position(), dt, longitudinal_accel, lateral);
    }
}

void Consist::place_at(double chainage) {
    loco_.place_at(chainage);
    prev_velocity_ = loco_.speed();
    current_limit_kmh_ = limits_.limit_kmh(chainage);  // HUD sensé dès la 1re frame
    update_running_gear(1.0 / 120.0, 0.0);
}

void Consist::update(double dt) {
    // --- KVB (M17 + M21.5) : appliqué AVANT la physique de tête ----------------
    // La limite courante est celle du bloc où se trouve le train (= le dernier panneau
    // franchi). Hystérésis : on ARME l'urgence dès qu'on dépasse la limite de plus de la
    // marge, et on ne la RELÂCHE qu'une fois revenu AU niveau de la limite — le conducteur
    // doit donc réellement ralentir sous la limite pour récupérer la main.
    current_limit_kmh_ = limits_.limit_kmh(loco_.chainage());
    const double speed_kmh = loco_.speed() * 3.6;
    if (speed_kmh > current_limit_kmh_ + config_.kvb_margin_kmh) {
        kvb_active_ = true;
    } else if (speed_kmh <= current_limit_kmh_) {
        kvb_active_ = false;
    }
    // KVB actif => traction coupée + freinage d'urgence, quelle que soit la consigne.
    // M21.5 : si le KVB est ISOLÉ (mode Arcade), la surveillance reste active (kvb_active_
    // se lève normalement, le HUD l'affiche) mais l'enforcement est désactivé — la
    // consigne du conducteur passe telle quelle.
    const bool kvb_enforcing = kvb_active_ && !kvb_isolated_;
    const double throttle = kvb_enforcing ? 0.0 : throttle_cmd_;
    const bool emergency = emergency_cmd_ || kvb_enforcing;
    loco_.set_controls(throttle, brake_cmd_, emergency);

    loco_.update(dt);
    // Même accélération longitudinale pour toute la rame (elle est rigide) => le tangage
    // des voitures suit celui de la motrice, sans décalage.
    const double longitudinal_accel = (loco_.speed() - prev_velocity_) / dt;
    prev_velocity_ = loco_.speed();
    update_running_gear(dt, longitudinal_accel);
}

}  // namespace noire::physics
