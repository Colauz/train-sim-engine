#include "noire/core/camera.hpp"

#include <cmath>

namespace noire {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
}  // namespace

glm::vec3 Camera::forward() const {
    const float cos_pitch = std::cos(pitch_);
    return glm::normalize(glm::vec3(std::cos(yaw_) * cos_pitch,  //
                                    std::sin(pitch_),            //
                                    std::sin(yaw_) * cos_pitch));
}

glm::vec3 Camera::right() const {
    return glm::normalize(glm::cross(forward(), kWorldUp));
}

void Camera::move_local(double forward_amount, double right_amount, double up_amount) {
    // Les déplacements se font en double (world space) pour ne rien perdre en précision.
    const glm::dvec3 f = glm::dvec3(forward());
    const glm::dvec3 r = glm::dvec3(right());
    const glm::dvec3 u = glm::dvec3(kWorldUp);
    position_ += f * forward_amount + r * right_amount + u * up_amount;
}

void Camera::add_yaw_pitch(float delta_yaw, float delta_pitch) {
    yaw_ += delta_yaw;
    const float limit = glm::half_pi<float>() - 0.01f;  // évite le gimbal / up dégénéré
    pitch_ = glm::clamp(pitch_ + delta_pitch, -limit, limit);
}

void Camera::look_at(const WorldPosition& target) {
    const glm::vec3 dir = glm::vec3(target - position_);
    const float len = glm::length(dir);
    if (len < 1e-6f) {
        return;
    }
    const glm::vec3 d = dir / len;
    pitch_ = std::asin(glm::clamp(d.y, -0.9999f, 0.9999f));
    yaw_ = std::atan2(d.z, d.x);
}

glm::mat4 Camera::view_matrix() const {
    // Eye à l'origine : on ne conserve QUE l'orientation. Toute la translation est
    // reportée dans les matrices Model relatives (origine flottante).
    return glm::lookAt(glm::vec3(0.0f), forward(), kWorldUp);
}

glm::mat4 Camera::projection_matrix(float aspect_ratio) const {
    glm::mat4 proj = glm::perspective(fov_y_radians, aspect_ratio, near_plane, far_plane);
    proj[1][1] *= -1.0f;  // Vulkan : axe Y de l'espace clip inversé par rapport à OpenGL
    return proj;
}

glm::mat4 Camera::relative_model(const WorldPosition& world_position) const {
    // (position monde - position caméra) en double, PUIS conversion float => petit offset.
    const glm::vec3 relative = glm::vec3(world_position - position_);
    return glm::translate(glm::mat4(1.0f), relative);
}

}  // namespace noire
