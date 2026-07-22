#include "noire/physics/wagon.hpp"

#include <algorithm>
#include <cmath>

namespace noire::physics {

namespace {
constexpr double kGravity = 9.81;

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
}  // namespace

void Wagon::set_controls(double throttle, double brake, bool emergency) {
    throttle_input_ = clamp01(throttle);
    air_brake_.set_handle(brake, emergency);
}

void Wagon::place_at(double chainage) {
    chainage_ = chainage;
    if (track_ != nullptr) {
        const double rate = track_->arc_rate(chainage_);
        const double half = (config_.wheelbase * 0.5) / rate;
        front_.follow(*track_, chainage_ + half);
        rear_.follow(*track_, chainage_ - half);
        caisse_.update(front_.position(), rear_.position(), 1.0 / 120.0, 0.0, 0.0);
    }
}

void Wagon::update(double dt) {
    if (track_ == nullptr) {
        return;
    }

    // Vitesse d'ENTRÉE du pas. Relevée ici, avant toute modification : l'hyperbole de
    // puissance comme l'accélération longitudinale (tangage) s'y réfèrent, et les faire
    // travailler sur une vitesse déjà à moitié intégrée mélangerait deux instants.
    const double v_before = velocity_;

    // Rampe de traction vers la consigne (notch) : montée ~2 s, coupure plus rapide.
    if (throttle_ < throttle_input_) {
        throttle_ = std::min(throttle_input_, throttle_ + dt / 2.0);
    } else {
        throttle_ = std::max(throttle_input_, throttle_ - dt / 1.0);
    }

    // --- Pente à l'abscisse courante ---
    glm::dvec3 pos_center;
    glm::dvec3 tangent_center;
    track_->sample(chainage_, pos_center, tangent_center);
    const double sin_theta = tangent_center.y;  // tangente normalisée => sin(pente)
    const double cos_theta = std::sqrt(std::max(0.0, 1.0 - sin_theta * sin_theta));
    grade_percent_ = (cos_theta > 1e-6) ? (sin_theta / cos_theta) * 100.0 : 0.0;

    const double normal_force = config_.mass * kGravity * cos_theta;
    // Charge sur les essieux MOTEURS. Elle seule tient le rail en traction : le reste de
    // la rame est remorqué. Les deux limites diffèrent donc, et pas d'un peu — sur un
    // TGV, la masse adhérente vaut un tiers de la masse totale.
    const double adhesive_mass =
        (config_.adhesive_mass > 0.0) ? config_.adhesive_mass : config_.mass;
    const double traction_normal_force = adhesive_mass * kGravity * cos_theta;

    // --- Effort disponible : plafond d'effort, puis hyperbole de puissance ---
    // Le plafond s'applique AVANT le manipulateur, jamais après : prendre
    // min(throttle * effort_max, P/v) donnerait la PLEINE PUISSANCE à mi-manipulateur,
    // puisque le minimum retomberait sur P/v dès que la vitesse est un peu élevée.
    double available = config_.max_tractive_effort;
    if (config_.max_power > 0.0) {
        // v_before : la vitesse de CE pas, avant que la phase 1 ne la modifie.
        // Le plancher évite la division par zéro à l'arrêt (et le 0/0 = NaN si jamais
        // max_power venait à valoir 0 malgré la garde).
        const double v = std::max(std::abs(v_before), 1e-3);
        available = std::min(available, config_.max_power / v);
    }
    const double demand = throttle_ * available;

    // --- Traction limitée par l'adhérence (des seuls essieux moteurs) ---
    // adhesion_scale_ : la météo. Sous la pluie il tombe sous 1, l'adhérence avec, et le
    // patinage — impossible à sec avec ces chiffres — redevient atteignable.
    const double mu_static = config_.adhesion_static * adhesion_scale_;
    const double mu_kinetic = config_.adhesion_kinetic * adhesion_scale_;
    const double traction_adhesion_static = mu_static * traction_normal_force;
    const double traction_adhesion_kinetic = mu_kinetic * traction_normal_force;
    const double adhesion_static = mu_static * normal_force;
    const double adhesion_kinetic = mu_kinetic * normal_force;
    slipping_ = false;
    double tractive = demand;
    if (demand > traction_adhesion_static) {
        slipping_ = true;
        tractive = traction_adhesion_kinetic;
    }
    tractive_effort_ = tractive;

    // --- Gravité projetée sur la voie ---
    const double gravity_force = -config_.mass * kGravity * sin_theta;

    // Phase 1 — propulsif.
    const double propulsive = tractive + gravity_force;
    velocity_ += (propulsive / config_.mass) * dt;

    // Phase 2 — résistif (ne peut pas inverser le sens).
    // Frein pneumatique : sa fraction d'effort sort de toute la chaîne CG -> cylindres.
    // Serrer le robinet ne fait donc PAS chuter brake_force tout de suite — l'onde met un
    // temps mort à partir, puis l'effort monte de lui-même sur plusieurs secondes.
    air_brake_.update(dt);
    double brake_force = air_brake_.force_fraction() * config_.max_brake_force;
    // Enrayage : au-delà de l'adhérence (des essieux freinés, donc masse TOTALE), la roue
    // se bloque et glisse. À sec c'est hors d'atteinte (1,3 MN contre 300 kN), mais un
    // freinage d'urgence (480 kN) sous la pluie franchit le seuil — sans cas particulier.
    if (brake_force > adhesion_static) {
        brake_force = adhesion_kinetic;
        slipping_ = true;
    }
    double resistive = brake_force;
    if (std::abs(velocity_) > 0.01) {
        // Davis : a + b*|v| + c*v². Le terme en v² est la traînée aérodynamique — à
        // 320 km/h elle pèse à elle seule 85 % de la résistance totale d'un TGV.
        const double v = std::abs(velocity_);
        resistive += config_.davis_a + config_.davis_b * v + config_.davis_c * v * v;
    }
    const double dv = (resistive / config_.mass) * dt;
    if (velocity_ > 0.0) {
        velocity_ = std::max(0.0, velocity_ - dv);
    } else if (velocity_ < 0.0) {
        velocity_ = std::min(0.0, velocity_ + dv);
    }

    // --- Avance du chainage : dx = (v · dt) / (ds/dx). Aucune borne : voie infinie. ---
    const double rate = track_->arc_rate(chainage_);
    chainage_ += (velocity_ * dt) / rate;

    const double longitudinal_accel = (velocity_ - v_before) / dt;

    // --- Bogies placés à ± empattement/2 (converti en x via le taux d'arc) ---
    const double half = (config_.wheelbase * 0.5) / rate;
    front_.follow(*track_, chainage_ + half);
    rear_.follow(*track_, chainage_ - half);
    front_.update_wheels(velocity_, dt);
    rear_.update_wheels(velocity_, dt);

    // Caisse : cinématique inverse entre les deux bogies + suspension (M16 : logique
    // extraite dans CarBody, partagée avec les voitures). Le roll vient de l'accélération
    // latérale déduite des tangentes des deux bogies.
    const double lateral_accel =
        signed_lateral_accel(front_.tangent(), rear_.tangent(), velocity_, config_.wheelbase);
    caisse_.update(front_.position(), rear_.position(), dt, longitudinal_accel, lateral_accel);
}

}  // namespace noire::physics
