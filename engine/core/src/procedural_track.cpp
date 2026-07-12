#include "noire/core/procedural_track.hpp"

#include <cmath>

namespace noire {

void ProceduralTrack::sample(double x, glm::dvec3& position, glm::dvec3& tangent) const {
    const double z = lateral_amp_ * std::sin(x / lateral_wave_) +
                     lateral_amp2_ * std::sin(x / lateral_wave2_);
    const double y = vertical_amp_ * std::sin(x / vertical_wave_);

    // Dérivées par rapport à x (pour la tangente).
    const double dz = (lateral_amp_ / lateral_wave_) * std::cos(x / lateral_wave_) +
                      (lateral_amp2_ / lateral_wave2_) * std::cos(x / lateral_wave2_);
    const double dy = (vertical_amp_ / vertical_wave_) * std::cos(x / vertical_wave_);

    position = origin_ + glm::dvec3(x, y, z);
    tangent = glm::normalize(glm::dvec3(1.0, dy, dz));
}

double ProceduralTrack::arc_rate(double x) const {
    const double dz = (lateral_amp_ / lateral_wave_) * std::cos(x / lateral_wave_) +
                      (lateral_amp2_ / lateral_wave2_) * std::cos(x / lateral_wave2_);
    const double dy = (vertical_amp_ / vertical_wave_) * std::cos(x / vertical_wave_);
    return std::sqrt(1.0 + dy * dy + dz * dz);
}

}  // namespace noire
