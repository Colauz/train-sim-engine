#include "noire/physics/bogie.hpp"

namespace noire::physics {

void Bogie::follow(const Spline& track, double distance) {
    distance_ = distance;
    if (track.empty()) {
        return;
    }

    glm::dvec3 position;
    glm::dvec3 tangent;
    track.sample(distance_, position, tangent);
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

}  // namespace noire::physics
