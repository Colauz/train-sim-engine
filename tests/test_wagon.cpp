#include "check.hpp"
#include "grade_track.hpp"
#include "noire/physics/wagon.hpp"

using noire::physics::Wagon;
using noire::physics::WagonConfig;
using noire::test::GradeTrack;

namespace {

constexpr double kDt = 1.0 / 120.0;

// La rame du jeu (make_metro_config, application.cpp) : 3 voitures E235 en charge.
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

void run(Wagon& w, double seconds) {
    const int steps = static_cast<int>(seconds / kDt);
    for (int i = 0; i < steps; ++i) {
        w.update(kDt);
    }
}

// La pente que rend le wagon doit être celle de la voie, au signe près : tout le reste
// en dépend (l'aide à la conduite retranche la pente du cran conseillé, et le pupitre
// l'affiche).
NOIRE_TEST(pente_rendue_a_l_identique) {
    const GradeTrack track{1.1};
    Wagon w{metro_config()};
    w.attach(&track);
    w.place_at(0.0);
    w.update(kDt);
    CHECK_NEAR(w.grade_percent(), 1.1, 1e-6);
}

// LE DÉFAUT DU M56, ET LA RAISON D'ÊTRE DE L'ANTI-RECUL (M57).
//
// Mascon au neutre, frein desserré, rampe de 1,1 % : avant le M57, la gravité (0,108
// m/s²) l'emportait sans partage sur les 600 N de Davis (0,006 m/s²) et la rame partait
// en arrière POUR TOUJOURS. Mesuré sur le banc : -6,5 km/h au bout de 17 s, et toujours
// en train d'accélérer. Le banc `NOIRE_SPEED=0` en devenait inutilisable (deux mesures
// ne cadraient jamais la même scène) et, en jeu, une rame arrêtée en gare quittait son
// quai à reculons.
NOIRE_TEST(anti_recul_tient_la_rame_sur_la_rampe) {
    const GradeTrack track{1.1};
    Wagon w{metro_config()};
    w.attach(&track);
    w.place_at(0.0);
    w.set_controls(0.0, 0.0, false);  // NEUTRE, frein desserré
    w.set_speed(0.0);

    run(w, 60.0);

    CHECK_NEAR(w.speed(), 0.0, 1e-12);
    CHECK_NEAR(w.chainage(), 0.0, 1e-9);  // pas un centimètre de dérive
    CHECK(w.rollback_hold());
    CHECK(w.immobilized());
}

// Le maintien n'est pas un blocage : il doit LÂCHER dès que la traction est demandée,
// sinon la rame ne repartirait jamais d'une gare en rampe.
NOIRE_TEST(anti_recul_relache_a_la_traction) {
    const GradeTrack track{1.1};
    Wagon w{metro_config()};
    w.attach(&track);
    w.place_at(0.0);
    w.set_controls(0.0, 0.0, false);
    w.set_speed(0.0);
    run(w, 10.0);
    CHECK(w.rollback_hold());

    w.set_controls(1.0, 0.0, false);  // P5
    run(w, 10.0);
    CHECK(!w.rollback_hold());
    CHECK(w.speed() > 1.0);  // elle est bien repartie, vers l'avant
}

// En descente, la rame roule vers l'AVANT sous son propre poids : l'anti-recul ne doit
// rien y faire. Il ne surveille qu'un seul sens — c'est un anti-recul, pas un frein.
NOIRE_TEST(anti_recul_ne_bride_pas_la_descente) {
    const GradeTrack track{-1.1};
    Wagon w{metro_config()};
    w.attach(&track);
    w.place_at(0.0);
    w.set_controls(0.0, 0.0, false);
    w.set_speed(0.0);

    run(w, 30.0);

    CHECK(w.speed() > 1.0);
    CHECK(!w.rollback_hold());
    CHECK(w.chainage() > 10.0);
}

// Le freinage de service doit immobiliser la rame et l'y maintenir — c'est la condition
// d'ouverture des portes (sécurité M23).
NOIRE_TEST(freinage_de_service_immobilise) {
    const GradeTrack track{0.0};
    Wagon w{metro_config()};
    w.attach(&track);
    w.place_at(0.0);
    w.set_speed(60.0 / 3.6);
    w.set_controls(0.0, 1.0, false);  // B8

    run(w, 40.0);

    CHECK_NEAR(w.speed(), 0.0, 1e-12);
    CHECK(w.immobilized());
}

// L'hyperbole de puissance (M13) : au-delà de la vitesse de base (1,9 MW / 90 kN =
// 21 m/s), l'effort disponible n'est plus l'effort maximal mais P/v. Sans elle, la rame
// accélérerait à 300 km/h comme au démarrage.
NOIRE_TEST(hyperbole_de_puissance_brise_l_acceleration) {
    const GradeTrack track{0.0};
    Wagon w{metro_config()};
    w.attach(&track);
    w.place_at(0.0);
    w.set_controls(1.0, 0.0, false);
    run(w, 3.0);  // laisse la rampe de traction atteindre la consigne (~2 s)

    // UN SEUL pas après chaque set_speed : l'effort se lit alors sur la vitesse voulue,
    // pas sur celle qu'aurait produite l'intégration.
    w.set_speed(5.0);  // bien sous la vitesse de base (21 m/s)
    w.update(kDt);
    const double effort_bas = w.tractive_effort();

    w.set_speed(40.0);  // bien au-dessus
    w.update(kDt);
    const double effort_haut = w.tractive_effort();

    CHECK_NEAR(effort_bas, 90000.0, 1.0);          // plafond d'effort
    CHECK_NEAR(effort_haut, 1900000.0 / 40.0, 1.0);  // P/v
    CHECK(effort_haut < effort_bas);
}

}  // namespace
