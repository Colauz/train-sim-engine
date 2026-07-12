#include "noire/physics/wagon.hpp"

#include <algorithm>
#include <cmath>

namespace noire::physics {

namespace {
constexpr double kGravity = 9.81;  // m/s^2

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
}  // namespace

void Wagon::set_controls(double throttle, double brake) {
    throttle_input_ = clamp01(throttle);
    brake_ = clamp01(brake);
}

void Wagon::place_at(double distance) {
    distance_ = distance;
    velocity_ = 0.0;
    if (track_ != nullptr && !track_->empty()) {
        const double half = config_.wheelbase * 0.5;
        front_.follow(*track_, distance_ + half);
        rear_.follow(*track_, distance_ - half);
        update_body(1.0 / 120.0, 0.0);
    }
}

void Wagon::update(double dt) {
    if (track_ == nullptr || track_->empty()) {
        return;
    }
    const double length = track_->length();
    const double half_wb = config_.wheelbase * 0.5;

    // Manette de traction lissée (notch) : ~2 s pour monter à fond, redescend plus vite.
    if (throttle_input_ > 0.5) {
        throttle_ = std::min(1.0, throttle_ + dt / 2.0);
    } else {
        throttle_ = std::max(0.0, throttle_ - dt / 1.0);
    }

    // --- Pente à l'abscisse courante ---
    glm::dvec3 pos_center;
    glm::dvec3 tangent_center;
    track_->sample(distance_, pos_center, tangent_center);
    const double sin_theta = tangent_center.y;  // tangente normalisée => composante verticale = sin(pente)
    const double cos_theta = std::sqrt(std::max(0.0, 1.0 - sin_theta * sin_theta));
    grade_percent_ = (cos_theta > 1e-6) ? (sin_theta / cos_theta) * 100.0 : 0.0;

    const double normal_force = config_.mass * kGravity * cos_theta;  // charge sur les rails

    // --- Traction limitée par l'adhérence acier/acier ---
    const double demand = throttle_ * config_.max_tractive_effort;
    const double adhesion_static = config_.adhesion_static * normal_force;
    const double adhesion_kinetic = config_.adhesion_kinetic * normal_force;
    slipping_ = false;
    double tractive = demand;
    if (demand > adhesion_static) {
        // Patinage : l'effort transmissible s'effondre à l'adhérence cinétique.
        slipping_ = true;
        tractive = adhesion_kinetic;
    }
    tractive_effort_ = tractive;

    // --- Gravité projetée sur la voie ---
    const double gravity_force = -config_.mass * kGravity * sin_theta;

    const double v_before = velocity_;

    // Phase 1 — forces propulsives (peuvent accélérer dans les deux sens).
    const double propulsive = tractive + gravity_force;
    velocity_ += (propulsive / config_.mass) * dt;

    // Phase 2 — forces résistives (freinage + résistance) : ne peuvent PAS inverser le sens.
    double brake_force = brake_ * config_.max_brake_force;
    if (brake_force > adhesion_static) {
        brake_force = adhesion_kinetic;  // blocage de roue (enrayage)
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

    // --- Intégration de la position (Euler semi-implicite) ---
    distance_ += velocity_ * dt;
    if (distance_ < half_wb) {
        distance_ = half_wb;
        velocity_ = 0.0;
    } else if (distance_ > length - half_wb) {
        distance_ = length - half_wb;
        velocity_ = 0.0;
    }

    const double longitudinal_accel = (velocity_ - v_before) / dt;

    // --- Placement des deux bogies sur la voie ---
    front_.follow(*track_, distance_ + half_wb);
    rear_.follow(*track_, distance_ - half_wb);

    update_body(dt, longitudinal_accel);
}

void Wagon::update_body(double dt, double longitudinal_accel) {
    // Pose géométrique de la caisse : dérivée de la position des DEUX bogies.
    const glm::dvec3 front_pos = front_.position();
    const glm::dvec3 rear_pos = rear_.position();
    const glm::dvec3 center = (front_pos + rear_pos) * 0.5;

    // L'axe avant contient le lacet (courbe) ET le tangage géométrique (dénivelé).
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

    // --- Pilonnement (heave) : oscillateur ressort/amortisseur excité par la base ---
    const double base_velocity = (support_y - prev_support_y_) / dt;
    prev_support_y_ = support_y;
    const double wn = 2.0 * glm::pi<double>() * config_.heave_frequency;
    const double k = wn * wn;
    const double c = 2.0 * config_.heave_damping * wn;
    const double heave_accel = -k * (body_y_ - support_y) - c * (body_vy_ - base_velocity);
    body_vy_ += heave_accel * dt;
    body_y_ += body_vy_ * dt;
    const double heave = body_y_ - support_y;

    // --- Tangage (pitch) : oscillateur forcé par l'accélération longitudinale ---
    const double target_pitch = -config_.pitch_gain * longitudinal_accel;  // nez cabré en accélération
    const double wp = 2.0 * glm::pi<double>() * config_.pitch_frequency;
    const double pitch_accel =
        wp * wp * (target_pitch - pitch_) - 2.0 * config_.pitch_damping * wp * pitch_vel_;
    pitch_vel_ += pitch_accel * dt;
    pitch_ += pitch_vel_ * dt;

    // --- Pose finale de la caisse ---
    body_position_ = center + glm::dvec3(0.0, config_.body_height + heave, 0.0);

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(up, 0.0f);
    basis[2] = glm::vec4(-forward, 0.0f);
    // Tangage de suspension appliqué autour de l'axe droit (X local).
    body_orientation_ =
        basis * glm::rotate(glm::mat4(1.0f), static_cast<float>(pitch_), glm::vec3(1.0f, 0.0f, 0.0f));
}

}  // namespace noire::physics
