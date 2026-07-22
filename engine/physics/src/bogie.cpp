#include "noire/physics/bogie.hpp"

namespace noire::physics {

void Bogie::follow(const TrackSource& track, double x) {
    chainage_ = x;

    glm::dvec3 position;
    glm::dvec3 tangent;
    track.sample(x, position, tangent);
    position_ = position;
    tangent_ = tangent;

    // Base orthonormée alignée sur la tangente : -Z local = sens de la voie.
    const glm::vec3 forward = glm::vec3(glm::normalize(tangent));
    const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
    const glm::vec3 up = glm::cross(right, forward);

    glm::mat4 rotation(1.0f);
    rotation[0] = glm::vec4(right, 0.0f);
    rotation[1] = glm::vec4(up, 0.0f);
    rotation[2] = glm::vec4(-forward, 0.0f);
    orientation_ = rotation;
}

void Bogie::update_wheels(double linear_speed, double dt) {
    // Kinematics: v = ω * r => ω = v / r
    const double omega = linear_speed / kWheelRadius;
    wheel_angle_ += static_cast<float>(omega * dt);
    constexpr double kTwoPi = 6.28318530717958647692;
    if (wheel_angle_ > static_cast<float>(kTwoPi) || wheel_angle_ < static_cast<float>(-kTwoPi)) {
        wheel_angle_ = static_cast<float>(std::fmod(static_cast<double>(wheel_angle_), kTwoPi));
    }
}

}  // namespace noire::physics
