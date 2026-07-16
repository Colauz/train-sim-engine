#include "noire/core/speed_limits.hpp"

namespace noire {

namespace {
struct Zone {
    double start;      // chainage de début (m)
    double limit_kmh;  // limite dans cette zone
};

// PROFIL DE LIGNE FRANÇAIS (M17.5), en dur. Un vrai départ : on quitte la gare au pas, on
// prend la ligne classique, on se raccorde à la LGV, puis pleine ligne à 320. Les zones
// sont ordonnées par chainage croissant.
constexpr Zone kZones[] = {
    {0.0, 30.0},       // 0 – 2 km : gare / dépôt
    {2000.0, 160.0},   // 2 – 15 km : ligne classique
    {15000.0, 220.0},  // 15 – 20 km : raccordement LGV
    {20000.0, 320.0},  // 20 km et + : LGV pleine ligne
};
constexpr int kZoneCount = static_cast<int>(sizeof(kZones) / sizeof(kZones[0]));
}  // namespace

double SpeedLimits::limit_kmh(double chainage) const {
    // Dernière zone dont le début est <= chainage. Les zones étant triées, on avance tant
    // qu'on n'a pas dépassé le train.
    double limit = kZones[0].limit_kmh;
    for (int i = 0; i < kZoneCount; ++i) {
        if (chainage >= kZones[i].start) {
            limit = kZones[i].limit_kmh;
        } else {
            break;
        }
    }
    return limit;
}

int SpeedLimits::zone_count() const { return kZoneCount; }

double SpeedLimits::zone_start(int zone) const { return kZones[zone].start; }

double SpeedLimits::zone_limit(int zone) const { return kZones[zone].limit_kmh; }

int SpeedLimits::tier_for_limit(double limit_kmh) const {
    // Couleur du panneau par gravité de la limite (rouge = très restrictif, vert = libre).
    if (limit_kmh <= 60.0) return 0;   // rouge (30)
    if (limit_kmh <= 160.0) return 1;  // orange (160)
    if (limit_kmh <= 220.0) return 2;  // ambre (220)
    if (limit_kmh <= 280.0) return 3;  // vert-jaune
    return 4;                          // vert (320)
}

}  // namespace noire
