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
    const glm::dvec3 world_up(0.0, 1.0, 0.0);

    // Hauteur NOMINALE du centre de caisse : moyenne des deux bogies + hauteur de caisse.
    const double support_y = (front_pos.y + rear_pos.y) * 0.5 + config_.body_height;
    if (!ready_) {
        body_y_ = support_y;
        prev_support_y_ = support_y;
        ready_ = true;
    }

    // --- Pilonnement UNIFORME : oscillateur excité par la base (le sol qui monte/descend) ---
    const double base_velocity = (support_y - prev_support_y_) / dt;
    prev_support_y_ = support_y;
    const double wn = 2.0 * glm::pi<double>() * config_.heave_frequency;
    const double heave_accel = -(wn * wn) * (body_y_ - support_y) -
                               2.0 * config_.heave_damping * wn * (body_vy_ - base_velocity);
    body_vy_ += heave_accel * dt;
    body_y_ += body_vy_ * dt;
    const double heave = body_y_ - support_y;

    // --- TANGAGE = TRANSFERT DE CHARGE (M17.6) ---
    // pitch_ est la DEMI-COURSE d'appui (m) : au freinage la charge passe sur l'avant, donc
    // l'appui avant s'enfonce et l'arrière se lève (nez qui plonge). C'est une conséquence de
    // la course de suspension, BORNÉE — la caisse ne peut jamais s'arracher des bogies.
    double target_pitch = config_.pitch_transfer * longitudinal_accel;
    target_pitch = std::clamp(target_pitch, -config_.max_pitch_travel, config_.max_pitch_travel);
    const double wp = 2.0 * glm::pi<double>() * config_.pitch_frequency;
    const double pitch_accel =
        wp * wp * (target_pitch - pitch_) - 2.0 * config_.pitch_damping * wp * pitch_vel_;
    pitch_vel_ += pitch_accel * dt;
    pitch_ += pitch_vel_ * dt;
    if (pitch_ > config_.max_pitch_travel) {
        pitch_ = config_.max_pitch_travel;
        if (pitch_vel_ > 0.0) pitch_vel_ = 0.0;
    } else if (pitch_ < -config_.max_pitch_travel) {
        pitch_ = -config_.max_pitch_travel;
        if (pitch_vel_ < 0.0) pitch_vel_ = 0.0;
    }

    // --- Roulis : petite rotation autour de l'axe long (inchangé, borné) ---
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

    // --- LES DEUX APPUIS : la caisse est tendue entre eux -----------------------------
    // Chaque appui est à `body_height` au-dessus de SON bogie, plus le pilonnement, plus/moins
    // le transfert de charge. La caisse relie ces deux points : son orientation (yaw + pitch)
    // en découle GÉOMÉTRIQUEMENT, et comme les appuis restent à ~body_height de leurs bogies,
    // la caisse ne peut PAS décoller. Fini le « wheelie ».
    const double h = config_.body_height + heave;
    const glm::dvec3 front_support = front_pos + world_up * (h + pitch_);
    const glm::dvec3 rear_support = rear_pos + world_up * (h - pitch_);
    const glm::dvec3 center = (front_support + rear_support) * 0.5;

    const glm::vec3 forward = glm::vec3(glm::normalize(front_support - rear_support));
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(world_up)));
    const glm::vec3 up = glm::cross(right, forward);

    position_ = center;
    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(up, 0.0f);
    basis[2] = glm::vec4(-forward, 0.0f);
    // Le tangage est DÉJÀ dans la géométrie (via forward). Il ne reste qu'à appliquer le
    // roulis, autour de l'axe long.
    orientation_ = basis * glm::rotate(glm::mat4(1.0f), static_cast<float>(roll_),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
}

}  // namespace noire::physics
