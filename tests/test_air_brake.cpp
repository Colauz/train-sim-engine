#include "check.hpp"
#include "noire/physics/air_brake.hpp"

using noire::physics::AirBrake;
using noire::physics::AirBrakeConfig;

namespace {

constexpr double kDt = 1.0 / 120.0;

void run(AirBrake& b, double seconds) {
    const int steps = static_cast<int>(seconds / kDt);
    for (int i = 0; i < steps; ++i) {
        b.update(kDt);
    }
}

// Au repos, la conduite générale est CHARGÉE et les freins desserrés. Ce n'est pas un
// détail d'initialisation : une ligne à retard initialisée en pressions absolues plutôt
// qu'en dépressions vaudrait « 0 bar dans la CG », c'est-à-dire un freinage d'urgence à
// chaque démarrage (cf. l'en-tête d'air_brake.hpp).
NOIRE_TEST(cg_chargee_au_repos) {
    AirBrake b;
    run(b, 1.0);
    CHECK_NEAR(b.pipe_pressure(), b.config().nominal_pressure, 1e-9);
    CHECK_NEAR(b.force_fraction(), 0.0, 1e-9);
    CHECK(!b.emergency());
}

// LE FAIT QUI COMMANDE TOUT : le frein n'est pas une commande, c'est une ONDE. On tire
// le robinet, et il ne se passe RIEN pendant le temps de propagation.
NOIRE_TEST(temps_mort_de_propagation) {
    AirBrakeConfig cfg;
    AirBrake b{cfg};
    b.set_handle(1.0, false);

    // Temps mort de SERVICE = L / (2c), et non L / c : un Wagon forfaitaire représente la
    // rame entière, donc l'effort modélisé est la SOMME sur tous les véhicules, dont le
    // retard moyen est celui du MILIEU de la rame (cf. air_brake.cpp). On regarde à la
    // moitié de ce temps mort — il ne doit rien s'être passé.
    const double dead_time = cfg.train_length / (2.0 * cfg.propagation_speed);
    run(b, dead_time * 0.5);
    CHECK_NEAR(b.force_fraction(), 0.0, 1e-12);
    CHECK_NEAR(b.pipe_pressure(), cfg.nominal_pressure, 1e-12);

    // Puis l'effort monte tout seul, et met plusieurs secondes à s'établir.
    run(b, 1.0);
    CHECK(b.force_fraction() > 0.0);
    const double apres_1s = b.force_fraction();
    run(b, 5.0);
    CHECK(b.force_fraction() > apres_1s);
}

// Freinage de service maximal : l'effort converge vers 1,0 (= max_brake_force), sans le
// dépasser. C'est ce 1,0 qui calibre les 8 crans du manipulateur.
NOIRE_TEST(service_maximal_converge_vers_un) {
    AirBrake b;
    b.set_handle(1.0, false);
    run(b, 20.0);
    CHECK_NEAR(b.force_fraction(), 1.0, 0.02);
    CHECK(b.force_fraction() <= 1.0 + 1e-6);
}

// L'urgence n'est pas « le service en plus fort » : la CG est vidée par des valves
// réparties, d'où un temps mort presque nul et un effort supérieur au service.
NOIRE_TEST(urgence_plus_rapide_et_plus_forte_que_le_service) {
    AirBrakeConfig cfg;

    AirBrake service{cfg};
    service.set_handle(1.0, false);
    run(service, 1.0);

    AirBrake urgence{cfg};
    urgence.set_handle(1.0, true);
    run(urgence, 1.0);

    CHECK(urgence.emergency());
    CHECK(urgence.force_fraction() > service.force_fraction());

    run(urgence, 20.0);
    CHECK_NEAR(urgence.force_fraction(), cfg.emergency_ratio, 0.05);
    CHECK_NEAR(urgence.pipe_pressure(), 0.0, 0.05);  // CG vidée intégralement
}

// La recharge est plus LENTE que la vidange : c'est pourquoi un mécanicien ne « pompe »
// pas ses freins — il ne les récupérerait pas. Desserrer ne doit donc pas rendre
// l'effort instantanément.
NOIRE_TEST(desserrage_plus_lent_que_le_serrage) {
    AirBrake b;
    b.set_handle(1.0, false);
    run(b, 20.0);
    const double serre = b.force_fraction();
    CHECK(serre > 0.9);

    b.set_handle(0.0, false);
    run(b, 1.0);
    // Une seconde après le desserrage, l'effort a baissé mais reste très présent.
    CHECK(b.force_fraction() < serre);
    CHECK(b.force_fraction() > 0.1);

    run(b, 30.0);
    CHECK_NEAR(b.force_fraction(), 0.0, 0.02);
    CHECK_NEAR(b.pipe_pressure(), b.config().nominal_pressure, 0.05);
}

// Monotonie de la commande : un cran plus fort ne peut pas donner un effort plus faible.
NOIRE_TEST(effort_monotone_en_fonction_du_cran) {
    double precedent = -1.0;
    for (int notch = 1; notch <= 8; ++notch) {
        AirBrake b;
        b.set_handle(static_cast<double>(notch) / 8.0, false);
        run(b, 20.0);
        CHECK(b.force_fraction() > precedent);
        precedent = b.force_fraction();
    }
}

}  // namespace
