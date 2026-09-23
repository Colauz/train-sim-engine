#include "check.hpp"
#include "noire/core/speed_limits.hpp"

using noire::SpeedLimits;
using noire::kStationSpacing;

namespace {

// Le défaut que ce cas verrouille : jusqu'au M55, le profil de vitesse était engendré
// sur un entraxe de 1 200 m sans rapport avec les 2 000 m de la géométrie. Les paliers
// tombaient donc n'importe où — 90 km/h à l'entrée d'un quai, 45 en pleine ligne. Ce
// n'était pas visible tant qu'aucune aide n'affichait la limite à venir.
NOIRE_TEST(profil_de_ligne_cale_sur_les_gares) {
    const SpeedLimits limits;
    for (int station = 0; station < 5; ++station) {
        const double gare = kStationSpacing * static_cast<double>(station);
        // L'emprise du quai est à 45 km/h, du repère d'arrêt jusqu'à 300 m après.
        CHECK_NEAR(limits.limit_kmh(gare + 1.0), 45.0, 1e-9);
        CHECK_NEAR(limits.limit_kmh(gare + 299.0), 45.0, 1e-9);
        // Puis pleine ligne, jusqu'à 300 m de la gare SUIVANTE.
        CHECK_NEAR(limits.limit_kmh(gare + 301.0), 90.0, 1e-9);
        CHECK_NEAR(limits.limit_kmh(gare + kStationSpacing - 301.0), 90.0, 1e-9);
        // Approche : 65 puis 45, et l'on arrive donc au quai déjà à 45.
        CHECK_NEAR(limits.limit_kmh(gare + kStationSpacing - 299.0), 65.0, 1e-9);
        CHECK_NEAR(limits.limit_kmh(gare + kStationSpacing - 149.0), 45.0, 1e-9);
        CHECK_NEAR(limits.limit_kmh(gare + kStationSpacing - 1.0), 45.0, 1e-9);
    }
}

// Les zones sont lues en séquence par DrivingAdvisor, qui suppose l'ordre croissant :
// il s'arrête à la première zone au-delà de sa fenêtre d'anticipation.
NOIRE_TEST(zones_triees_par_chainage_croissant) {
    const SpeedLimits limits;
    const int n = limits.zone_count();
    CHECK(n > 0);
    for (int z = 1; z < n; ++z) {
        CHECK(limits.zone_start(z) > limits.zone_start(z - 1));
    }
}

// L'aspect affiché par les signaux trackside et celui que lit le pupitre sortent de
// cette seule fonction : un désaccord y serait un signal qui ment.
NOIRE_TEST(aspects_ats_par_palier) {
    const SpeedLimits limits;
    CHECK(limits.tier_for_limit(0.0) == 0);    // R
    CHECK(limits.tier_for_limit(45.0) == 1);   // Y
    CHECK(limits.tier_for_limit(65.0) == 2);   // YG
    CHECK(limits.tier_for_limit(90.0) == 4);   // G
}

// Le terminus est un arrêt ABSOLU : au-delà, plus aucune vitesse n'est autorisée.
NOIRE_TEST(terminus_a_zero) {
    const SpeedLimits limits;
    const double fin = kStationSpacing * 30.0;
    CHECK(limits.limit_kmh(fin - 1.0) > 0.0);
    CHECK_NEAR(limits.limit_kmh(fin + 1.0), 0.0, 1e-9);
}

}  // namespace
