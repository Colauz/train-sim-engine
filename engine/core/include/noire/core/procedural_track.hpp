#pragma once

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"

namespace noire {

// Voie infinie procédurale : globalement droite le long de X, avec de légères
// ondulations latérales (Z) et de dénivelé (Y) => « légèrement sinueuse ».
// Entièrement analytique et déterministe (donc reproductible et thread-safe).
class ProceduralTrack : public TrackSource {
public:
    explicit ProceduralTrack(WorldPosition origin = {}) : origin_(origin) {}

    void sample(double x, glm::dvec3& position, glm::dvec3& tangent) const override;
    [[nodiscard]] double arc_rate(double x) const override;

private:
    WorldPosition origin_{};

    // Ondulations latérales (deux harmoniques) et dénivelé (une harmonique).
    double lateral_amp_ = 40.0;
    double lateral_wave_ = 620.0;
    double lateral_amp2_ = 14.0;
    double lateral_wave2_ = 175.0;
    double vertical_amp_ = 6.0;
    double vertical_wave_ = 520.0;
};

}  // namespace noire
