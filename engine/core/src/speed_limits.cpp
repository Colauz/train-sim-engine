#include "noire/core/speed_limits.hpp"

#include <algorithm>
#include <cmath>

namespace noire {

namespace {
// Cinq paliers de vitesse (km/h), du plus lent au plus rapide. Le KVB et les panneaux les
// partagent ; l'indice du palier sert aussi à colorer les panneaux.
constexpr double kTierLimits[5] = {110.0, 160.0, 220.0, 270.0, 320.0};
}  // namespace

int SpeedLimits::tier_for_block(long block) const {
    // Palier issu d'une somme de deux sinusoïdes lentes : varie continûment, donc au plus
    // d'un cran d'un bloc au suivant (la dérivée par bloc reste < 1,1 palier). Déterministe,
    // sans état, et sans à-coup infranchissable — c'est ce qui rend le KVB JOUABLE.
    const double phase = static_cast<double>(block) * 0.5;
    const double v = 2.0 + 1.7 * std::sin(phase) + 1.0 * std::sin(phase * 0.37 + 1.3);
    return std::clamp(static_cast<int>(std::lround(v)), 0, 4);
}

double SpeedLimits::limit_for_block(long block) const {
    return kTierLimits[tier_for_block(block)];
}

long SpeedLimits::block_index(double chainage) const {
    return static_cast<long>(std::floor(chainage / block_length_));
}

double SpeedLimits::block_start(long block) const {
    return static_cast<double>(block) * block_length_;
}

double SpeedLimits::limit_kmh(double chainage) const {
    return limit_for_block(block_index(chainage));
}

}  // namespace noire
