#pragma once

#include "noire/core/math.hpp"
#include "noire/core/spline.hpp"

namespace noire::physics {

// Bogie : point de contact contraint sur la voie. Depuis le M4 il est PASSIF :
// le Wagon lui impose une abscisse curviligne et il en déduit sa position (double)
// et son orientation (alignée sur la tangente). Toute la dynamique (forces, masse,
// adhérence) est portée par le Wagon.
class Bogie {
public:
    // Place le bogie à l'abscisse `distance` le long de la voie.
    void follow(const Spline& track, double distance);

    [[nodiscard]] const WorldPosition& position() const { return position_; }
    [[nodiscard]] const glm::dvec3& tangent() const { return tangent_; }
    [[nodiscard]] const glm::mat4& orientation() const { return orientation_; }
    [[nodiscard]] double distance() const { return distance_; }

private:
    double distance_ = 0.0;
    WorldPosition position_{0.0, 0.0, 0.0};
    glm::dvec3 tangent_{0.0, 0.0, 1.0};
    glm::mat4 orientation_{1.0f};
};

}  // namespace noire::physics
