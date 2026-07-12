#include "noire/physics/bogie.hpp"

#include <cmath>

namespace noire::physics {

void Bogie::update(double dt) {
    if (track_ == nullptr || track_->empty()) {
        return;
    }

    const double len = track_->length();
    distance_ += speed_ * dt;

    // Aller-retour aux extrémités (ping-pong) pour une démonstration continue.
    if (distance_ >= len) {
        distance_ = len;
        speed_ = -std::abs(speed_);
    } else if (distance_ <= 0.0) {
        distance_ = 0.0;
        speed_ = std::abs(speed_);
    }

    glm::dvec3 pos;
    glm::dvec3 tangent;
    track_->sample(distance_, pos, tangent);
    position_ = pos;

    // Base orthonormée alignée sur la marche : le -Z local (avant) suit la tangente.
    const glm::dvec3 dir = (speed_ >= 0.0) ? tangent : -tangent;
    const glm::vec3 forward = glm::vec3(glm::normalize(dir));
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
    const glm::vec3 up = glm::cross(right, forward);

    glm::mat4 rotation(1.0f);
    rotation[0] = glm::vec4(right, 0.0f);      // X local = droite
    rotation[1] = glm::vec4(up, 0.0f);         // Y local = haut
    rotation[2] = glm::vec4(-forward, 0.0f);   // Z local = arrière (=> -Z = avant)
    orientation_ = rotation;
}

}  // namespace noire::physics
