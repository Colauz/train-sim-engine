#pragma once

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"

namespace noire::physics {

// Bogie passif contraint sur la voie : le Wagon lui impose un chainage x et il en
// déduit sa position (double) + orientation (alignée sur la tangente). Fonctionne
// sur une voie infinie (TrackSource) comme sur une voie finie.
class Bogie {
public:
    void follow(const TrackSource& track, double x);
    // M22 : mise à jour de la rotation des roues en fonction de la vitesse linéaire (v = ω · r)
    void update_wheels(double linear_speed, double dt);

    [[nodiscard]] const WorldPosition& position() const { return position_; }
    [[nodiscard]] const glm::dvec3& tangent() const { return tangent_; }
    [[nodiscard]] const glm::mat4& orientation() const { return orientation_; }
    [[nodiscard]] double chainage() const { return chainage_; }
    [[nodiscard]] float wheel_angle() const { return wheel_angle_; }

private:
    static constexpr double kWheelRadius = 0.46;  // Ø 920 mm (norme SNCF)
    double chainage_ = 0.0;
    float wheel_angle_ = 0.0f;  // angle de rotation cumulé (radians)
    WorldPosition position_{0.0, 0.0, 0.0};
    glm::dvec3 tangent_{0.0, 0.0, 1.0};
    glm::mat4 orientation_{1.0f};
};

}  // namespace noire::physics
