#include "noire/physics/car_body.hpp"

#include <algorithm>
#include <cmath>

namespace noire::physics {

double signed_lateral_accel(const glm::dvec3& front_tangent, const glm::dvec3& rear_tangent,
                            double speed, double span) {
    if (span < 1e-6) {
        return 0.0;
    }
    // Roll = virage EN PLAN : on projette les tangentes à l'horizontale.
    glm::dvec3 a(front_tangent.x, 0.0, front_tangent.z);
    glm::dvec3 b(rear_tangent.x, 0.0, rear_tangent.z);
    const double la = glm::length(a);
    const double lb = glm::length(b);
    if (la < 1e-9 || lb < 1e-9) {
        return 0.0;
    }
    a /= la;
    b /= lb;
    const double angle = std::acos(glm::clamp(glm::dot(a, b), -1.0, 1.0));
    // Signe du virage : composante verticale du produit vectoriel (arrière -> avant).
    const double cross_y = b.z * a.x - b.x * a.z;
    const double signed_curvature = std::copysign(angle, cross_y) / span;
    return speed * speed * signed_curvature;
}

void CarBody::update(const glm::dvec3& front_pos, const glm::dvec3& rear_pos, double dt,
                     double longitudinal_accel, double lateral_accel) {
    const glm::dvec3 center = (front_pos + rear_pos) * 0.5;

    // Base orientée : l'axe long de la caisse EST le segment entre les deux bogies. Yaw et
    // pitch géométriques en découlent sans un seul angle calculé à la main.
    const glm::vec3 forward = glm::vec3(glm::normalize(front_pos - rear_pos));
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
    const glm::vec3 up = glm::cross(right, forward);

    const double support_y = center.y;
    if (!ready_) {
        body_y_ = support_y;
        prev_support_y_ = support_y;
        ready_ = true;
    }

    // Pilonnement : oscillateur excité par la base (le sol qui monte/descend sous la caisse).
    const double base_velocity = (support_y - prev_support_y_) / dt;
    prev_support_y_ = support_y;
    const double wn = 2.0 * glm::pi<double>() * config_.heave_frequency;
    const double k = wn * wn;
    const double c = 2.0 * config_.heave_damping * wn;
    const double heave_accel = -k * (body_y_ - support_y) - c * (body_vy_ - base_velocity);
    body_vy_ += heave_accel * dt;
    body_y_ += body_vy_ * dt;
    const double heave = body_y_ - support_y;

    // Tangage : oscillateur forcé par l'accélération longitudinale (plonge au freinage).
    const double target_pitch = -config_.pitch_gain * longitudinal_accel;
    const double wp = 2.0 * glm::pi<double>() * config_.pitch_frequency;
    const double pitch_accel =
        wp * wp * (target_pitch - pitch_) - 2.0 * config_.pitch_damping * wp * pitch_vel_;
    pitch_vel_ += pitch_accel * dt;
    pitch_ += pitch_vel_ * dt;
    // BUTOIR DUR (M17.5) : la caisse ne cabre JAMAIS au-delà de max_pitch, quelle que soit
    // l'accélération. On annule aussi la vitesse au contact du butoir : sinon l'intégrateur
    // « pousse » contre la borne et la caisse repart d'un coup en la quittant (wind-up).
    if (pitch_ > config_.max_pitch) {
        pitch_ = config_.max_pitch;
        if (pitch_vel_ > 0.0) pitch_vel_ = 0.0;
    } else if (pitch_ < -config_.max_pitch) {
        pitch_ = -config_.max_pitch;
        if (pitch_vel_ < 0.0) pitch_vel_ = 0.0;
    }

    // Roulis (M16) : oscillateur forcé par l'accélération latérale. Même forme et même
    // butoir que le tangage.
    const double target_roll = -config_.roll_gain * lateral_accel;
    const double wr = 2.0 * glm::pi<double>() * config_.roll_frequency;
    const double roll_accel =
        wr * wr * (target_roll - roll_) - 2.0 * config_.roll_damping * wr * roll_vel_;
    roll_vel_ += roll_accel * dt;
    roll_ += roll_vel_ * dt;
    if (roll_ > config_.max_roll) {
        roll_ = config_.max_roll;
        if (roll_vel_ > 0.0) roll_vel_ = 0.0;
    } else if (roll_ < -config_.max_roll) {
        roll_ = -config_.max_roll;
        if (roll_vel_ < 0.0) roll_vel_ = 0.0;
    }

    position_ = center + glm::dvec3(0.0, config_.body_height + heave, 0.0);

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(up, 0.0f);
    basis[2] = glm::vec4(-forward, 0.0f);
    // Tangage autour de l'axe transversal (X local), roulis autour de l'axe long (Z local).
    glm::mat4 lean = glm::rotate(glm::mat4(1.0f), static_cast<float>(pitch_),
                                 glm::vec3(1.0f, 0.0f, 0.0f));
    lean = glm::rotate(lean, static_cast<float>(roll_), glm::vec3(0.0f, 0.0f, 1.0f));
    orientation_ = basis * lean;
}

}  // namespace noire::physics
