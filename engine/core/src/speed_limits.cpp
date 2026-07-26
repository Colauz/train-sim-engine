#include "noire/core/speed_limits.hpp"

namespace noire {

// PROFIL DE LIGNE JAPONAIS (M30 — pivot métro de Tokyo), généré par cycle :
// une GARE tous les 1 200 m, et un séquencement d'approche typique ATS/ATC :
//
//     cycle +   0 m : gare (quai de 300 m) — 45 km/h  => aspect Y
//     cycle + 300 m : pleine ligne         — 90 km/h  => aspect G
//     cycle + 900 m : approche réduite     — 65 km/h  => aspect YG
//     cycle +1050 m : approche courte      — 45 km/h  => aspect Y
//     ... et on recommence. Terminus à 36 km : 0 km/h => aspect R (urgence si franchi).
//
// Le chainage sert d'abscisse (arc_rate ≈ 1 sur notre tracé => chainage ≈ m réels). C'est
// l'unique source de vérité, partagée par l'ATS (qui l'applique) et par les signaux
// trackside (qui l'affichent aux points de transition EXACTS) : ils ne peuvent pas se
// contredire.

namespace {
struct Zone {
    double start;      // chainage de début (m)
    double limit_kmh;  // limite dans cette zone
};

constexpr double kStationSpacing = 1200.0;  // distance inter-gares (métro dense)
constexpr int kCycles = 30;                 // 30 gares = 36 km de ligne

// Zones générées une fois à l'init statique, ordonnées par chainage croissant.
struct Zones {
    Zone list[4 * kCycles + 1];
    int count = 0;
    Zones() {
        for (int k = 0; k < kCycles; ++k) {
            const double base = kStationSpacing * static_cast<double>(k);
            list[count++] = {base, 45.0};         // gare (quai 0-300 m)
            list[count++] = {base + 300.0, 90.0};  // pleine ligne
            list[count++] = {base + 900.0, 65.0};  // approche réduite (YG)
            list[count++] = {base + 1050.0, 45.0}; // approche courte (Y)
        }
        list[count++] = {kStationSpacing * kCycles, 0.0};  // terminus : R
    }
};
const Zones kZones;
}  // namespace

double SpeedLimits::limit_kmh(double chainage) const {
    // Dernière zone dont le début est <= chainage. Les zones étant triées, on avance tant
    // qu'on n'a pas dépassé le train.
    double limit = kZones.list[0].limit_kmh;
    for (int i = 0; i < kZones.count; ++i) {
        if (chainage >= kZones.list[i].start) {
            limit = kZones.list[i].limit_kmh;
        } else {
            break;
        }
    }
    return limit;
}

int SpeedLimits::zone_count() const { return kZones.count; }

double SpeedLimits::zone_start(int zone) const { return kZones.list[zone].start; }

double SpeedLimits::zone_limit(int zone) const { return kZones.list[zone].limit_kmh; }

int SpeedLimits::tier_for_limit(double limit_kmh) const {
    // Aspect ATS => palier de couleur du signal.
    if (limit_kmh <= 0.0) return 0;   // R  : rouge (arrêt)
    if (limit_kmh <= 45.0) return 1;  // Y  : jaune (caution, 45)
    if (limit_kmh <= 65.0) return 2;  // YG : vert-jaune (reduced, 65)
    return 4;                         // G  : vert (proceed, limite de ligne)
}

}  // namespace noire
