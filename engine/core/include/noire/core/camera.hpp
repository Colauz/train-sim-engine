#pragma once

#include "noire/core/math.hpp"

namespace noire {

// Caméra libre (fly camera) conçue pour l'ORIGINE FLOTTANTE :
//   * la position est stockée en double (world space) => précision à grande échelle ;
//   * la matrice de vue ne contient AUCUNE translation (la caméra est à l'origine
//     du repère GPU) ;
//   * chaque objet fournit sa position monde en double ; on en dérive une matrice
//     Model relative à la caméra, ramenée en float => translations faibles, pas de
//     jittering, et le GPU ne manipule que des float32.
class Camera {
public:
    // --- Manipulation ---------------------------------------------------------
    void move_local(double forward_amount, double right_amount, double up_amount);
    void add_yaw_pitch(float delta_yaw, float delta_pitch);  // radians (souris)

    void set_position(const WorldPosition& p) { position_ = p; }
    void set_orientation(float yaw, float pitch) {
        yaw_ = yaw;
        pitch_ = pitch;
    }
    // Oriente la caméra pour viser un point monde (utile pour une caméra qui suit).
    void look_at(const WorldPosition& target);
    [[nodiscard]] const WorldPosition& position() const { return position_; }
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 right() const;

    // --- Matrices (float, conventions Vulkan) --------------------------------
    // Vue SANS translation (eye à l'origine) : c'est le cœur de l'origine flottante.
    [[nodiscard]] glm::mat4 view_matrix() const;
    // Projection perspective (profondeur 0..1, axe Y inversé pour Vulkan).
    [[nodiscard]] glm::mat4 projection_matrix(float aspect_ratio) const;

    // Matrice Model d'un objet situé en `world_position` (double), exprimée
    // relativement à la caméra et convertie en float.
    [[nodiscard]] glm::mat4 relative_model(const WorldPosition& world_position) const;

    // --- Paramètres publics ---------------------------------------------------
    float fov_y_radians = glm::radians(60.0f);
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float move_speed = 12.0f;          // m/s
    float mouse_sensitivity = 0.0025f;  // rad/pixel

private:
    WorldPosition position_{0.0, 0.0, 0.0};
    float yaw_ = -glm::half_pi<float>();  // regarde vers -Z au démarrage
    float pitch_ = 0.0f;
};

}  // namespace noire
