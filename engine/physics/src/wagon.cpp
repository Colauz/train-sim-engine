#include "noire/physics/wagon.hpp"

#include <algorithm>
#include <cmath>

namespace noire::physics {

namespace {
constexpr double kGravity = 9.81;

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
}  // namespace

void Wagon::set_controls(double throttle, double brake) {
    throttle_input_ = clamp01(throttle);
    brake_ = clamp01(brake);
}

void Wagon::place_at(double chainage) {
    chainage_ = chainage;
    if (track_ != nullptr) {
        const double rate = track_->arc_rate(chainage_);
        const double half = (config_.wheelbase * 0.5) / rate;
        front_.follow(*track_, chainage_ + half);
        rear_.follow(*track_, chainage_ - half);
        update_body(1.0 / 120.0, 0.0);
    }
}

void Wagon::update(double dt) {
    if (track_ == nullptr) {
        return;
    }

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

    // --- Traction limitée par l'adhérence ---
    const double demand = throttle_ * config_.max_tractive_effort;
    const double adhesion_static = config_.adhesion_static * normal_force;
    const double adhesion_kinetic = config_.adhesion_kinetic * normal_force;
    slipping_ = false;
    double tractive = demand;
    if (demand > adhesion_static) {
        slipping_ = true;
        tractive = adhesion_kinetic;
    }
    tractive_effort_ = tractive;

    // --- Gravité projetée sur la voie ---
    const double gravity_force = -config_.mass * kGravity * sin_theta;

    const double v_before = velocity_;

    // Phase 1 — propulsif.
    const double propulsive = tractive + gravity_force;
    velocity_ += (propulsive / config_.mass) * dt;

    // Phase 2 — résistif (ne peut pas inverser le sens).
    double brake_force = brake_ * config_.max_brake_force;
    if (brake_force > adhesion_static) {
        brake_force = adhesion_kinetic;
        slipping_ = true;
    }
    double resistive = brake_force;
    if (std::abs(velocity_) > 0.01) {
        resistive += config_.rolling_c0 + config_.rolling_c2 * velocity_ * velocity_;
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

    update_body(dt, longitudinal_accel);
}

void Wagon::update_body(double dt, double longitudinal_accel) {
    const glm::dvec3 front_pos = front_.position();
    const glm::dvec3 rear_pos = rear_.position();
    const glm::dvec3 center = (front_pos + rear_pos) * 0.5;

    const glm::vec3 forward = glm::vec3(glm::normalize(front_pos - rear_pos));
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
    const glm::vec3 up = glm::cross(right, forward);

    const double support_y = center.y;
    if (!suspension_ready_) {
        body_y_ = support_y;
        prev_support_y_ = support_y;
        suspension_ready_ = true;
    }

    // Pilonnement : oscillateur excité par la base.
    const double base_velocity = (support_y - prev_support_y_) / dt;
    prev_support_y_ = support_y;
    const double wn = 2.0 * glm::pi<double>() * config_.heave_frequency;
    const double k = wn * wn;
    const double c = 2.0 * config_.heave_damping * wn;
    const double heave_accel = -k * (body_y_ - support_y) - c * (body_vy_ - base_velocity);
    body_vy_ += heave_accel * dt;
    body_y_ += body_vy_ * dt;
    const double heave = body_y_ - support_y;

    // Tangage : oscillateur forcé par l'accélération longitudinale.
    const double target_pitch = -config_.pitch_gain * longitudinal_accel;
    const double wp = 2.0 * glm::pi<double>() * config_.pitch_frequency;
    const double pitch_accel =
        wp * wp * (target_pitch - pitch_) - 2.0 * config_.pitch_damping * wp * pitch_vel_;
    pitch_vel_ += pitch_accel * dt;
    pitch_ += pitch_vel_ * dt;

    body_position_ = center + glm::dvec3(0.0, config_.body_height + heave, 0.0);

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(up, 0.0f);
    basis[2] = glm::vec4(-forward, 0.0f);
    body_orientation_ =
        basis * glm::rotate(glm::mat4(1.0f), static_cast<float>(pitch_), glm::vec3(1.0f, 0.0f, 0.0f));
}

}  // namespace noire::physics
