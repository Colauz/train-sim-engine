#include "check.hpp"
#include "grade_track.hpp"
#include "noire/core/speed_limits.hpp"
#include "noire/physics/consist.hpp"

using noire::kStationSpacing;
using noire::physics::Consist;
using noire::physics::ConsistConfig;
using noire::physics::WagonConfig;
using noire::test::GradeTrack;

namespace {

constexpr double kDt = 1.0 / 120.0;

WagonConfig metro_config() {
    WagonConfig c;
    c.mass = 96000.0;
    c.wheelbase = 14.0;
    c.max_tractive_effort = 90000.0;
    c.max_power = 1900000.0;
    c.max_brake_force = 100000.0;
    c.davis_a = 600.0;
    c.davis_b = 20.0;
    c.davis_c = 2.5;
    return c;
}

ConsistConfig consist_config() {
    ConsistConfig cc;
    cc.car_count = 2;
    cc.ats_margin_kmh = 5.0;
    return cc;
}

// Maintient la rame à vitesse CONSTANTE : on ré-impose la vitesse à chaque pas, de sorte
// que le seul comportement observé soit celui de l'ATS. Sans ça, l'urgence qu'il déclenche
// ralentirait la rame et masquerait l'état qu'on cherche à lire.
void hold_at(Consist& consist, double kmh, double seconds) {
    const int steps = static_cast<int>(seconds / kDt);
    for (int i = 0; i < steps; ++i) {
        consist.set_speed(kmh / 3.6);
        consist.update(kDt);
    }
}

// Chainage en PLEINE LIGNE (zone à 90 km/h) : 1 000 m après une gare.
constexpr double kPleineLigne = 1000.0;

// LE DÉFAUT CORRIGÉ AU M57. `ats_margin_kmh` était réglée à 5 km/h dans l'app, documentée
// dans l'en-tête de ConsistConfig... et lue par PERSONNE : consist.cpp comparait la
// vitesse à la limite nue. Résultat, l'avertissement se levait pour un demi-km/h de
// dépassement, et l'aide à la conduite — qui vise « limite − 2 km/h » en SUPPOSANT la
// marge — conseillait une consigne à 2 km/h d'un déclenchement au lieu des 7 attendus.
NOIRE_TEST(ats_respecte_sa_marge) {
    const GradeTrack track{0.0};
    Consist consist{metro_config(), consist_config()};
    consist.attach(&track);
    consist.place_at(kPleineLigne);

    // 92 km/h : au-dessus de la limite (90) mais DANS la marge (95). Aucun avertissement,
    // même après bien plus que les 10 s du délai de grâce.
    hold_at(consist, 92.0, 20.0);
    CHECK(!consist.ats_warning());
    CHECK(!consist.ats_active());
}

NOIRE_TEST(ats_declenche_au_dela_de_la_marge) {
    const GradeTrack track{0.0};
    Consist consist{metro_config(), consist_config()};
    consist.attach(&track);
    consist.place_at(kPleineLigne);

    // 99 km/h : au-delà de 90 + 5. L'avertissement se lève tout de suite...
    hold_at(consist, 99.0, 1.0);
    CHECK(consist.ats_warning());
    CHECK(!consist.ats_active());

    // ... et l'urgence tombe au bout des 10 s de grâce (M34).
    hold_at(consist, 99.0, 10.0);
    CHECK(consist.ats_active());
}

// Un signal d'arrêt (aspect R, limite 0) ne se tolère pas « à 5 km/h près » : c'est le
// seul cas où la marge ne s'applique pas.
NOIRE_TEST(aspect_r_sans_marge) {
    const GradeTrack track{0.0};
    Consist consist{metro_config(), consist_config()};
    consist.attach(&track);
    consist.place_at(kStationSpacing * 30.0 + 50.0);  // au-delà du terminus

    hold_at(consist, 3.0, 11.0);
    CHECK(consist.ats_active());
}

// LE TROU DE SURVEILLANCE corrigé au M57 : l'ATS comparait une vitesse SIGNÉE à une
// limite positive. Une rame lancée en marche arrière n'était donc JAMAIS en survitesse,
// à n'importe quelle allure. L'anti-recul rend le cas improbable — il ne doit pas pour
// autant rester invisible au système qui a précisément pour rôle de le voir.
NOIRE_TEST(ats_voit_la_survitesse_en_marche_arriere) {
    const GradeTrack track{0.0};
    Consist consist{metro_config(), consist_config()};
    consist.attach(&track);
    consist.place_at(kPleineLigne);

    hold_at(consist, -99.0, 11.0);
    CHECK(consist.ats_active());
}

// Réarmement (M33) : l'urgence reste verrouillée jusqu'à l'acquittement du conducteur.
NOIRE_TEST(ats_reste_verrouille_jusqu_au_rearmement) {
    const GradeTrack track{0.0};
    Consist consist{metro_config(), consist_config()};
    consist.attach(&track);
    consist.place_at(kPleineLigne);

    hold_at(consist, 99.0, 11.0);
    CHECK(consist.ats_active());

    // Revenir sous la limite ne suffit PAS : le latch tient.
    hold_at(consist, 50.0, 5.0);
    CHECK(consist.ats_active());

    consist.reset_ats();
    CHECK(!consist.ats_active());
}

// Mode arcade (touche K) : l'ATS continue de SURVEILLER (le pupitre affiche la limite et
// l'état) mais ne court-circuite plus la conduite. Isoler, ce n'est pas aveugler.
NOIRE_TEST(ats_isole_surveille_toujours) {
    const GradeTrack track{0.0};
    Consist consist{metro_config(), consist_config()};
    consist.attach(&track);
    consist.place_at(kPleineLigne);
    consist.set_ats_isolated(true);

    hold_at(consist, 120.0, 15.0);
    CHECK(consist.ats_isolated());
    CHECK_NEAR(consist.current_limit_kmh(), 90.0, 1e-9);
    // Isolé, l'ATS ne serre pas : la rame n'est ni immobilisée ni freinée d'urgence.
    CHECK(!consist.loco().air_brake().emergency());
}

}  // namespace
