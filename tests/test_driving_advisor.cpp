#include <algorithm>
#include <cmath>

#include "check.hpp"
#include "grade_track.hpp"
#include "noire/core/driving_advisor.hpp"
#include "noire/core/speed_limits.hpp"
#include "noire/physics/consist.hpp"

using noire::DrivingAdvice;
using noire::DrivingAdvisor;
using noire::SpeedLimits;
using noire::kStationSpacing;
using noire::physics::Consist;
using noire::physics::ConsistConfig;
using noire::physics::WagonConfig;
using noire::test::GradeTrack;

namespace {

constexpr double kDt = 1.0 / 120.0;
// = kStopTolerance (application.cpp) : au-delà, les façades de quai restent closes.
constexpr double kStopTolerance = 0.5;

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

// --- Le module seul ---------------------------------------------------------

// Loin de tout, la consigne est la limite moins la marge : rien à anticiper.
NOIRE_TEST(consigne_en_palier_est_la_limite_moins_la_marge) {
    const SpeedLimits limits;
    const DrivingAdvisor advisor;
    // 1 000 m après une gare : pleine ligne à 90, la gare suivante est à 1 000 m et le
    // prochain abaissement (65) à 700 m — trop loin pour peser depuis cette vitesse.
    // La rame roule à 85, donc SOUS la consigne : l'aide ne doit rien conseiller.
    const DrivingAdvice a = advisor.advise(limits, 1000.0, 85.0 / 3.6,
                                           /*stop_chainage=*/1.0e9, /*natural_decel=*/0.0);
    CHECK_NEAR(a.limit_kmh, 90.0, 1e-9);
    CHECK_NEAR(a.target_kmh, 90.0 - advisor.config().limit_margin_kmh, 1e-9);
    CHECK(a.recommended_notch == 0);  // on est sur la courbe : rien à faire
}

// L'ENVELOPPE. À 100 m d'un point d'arrêt, la vitesse maximale admissible est celle
// dont on peut encore annuler l'énergie sur 100 m : v = sqrt(2.a.d).
NOIRE_TEST(enveloppe_d_arret_suit_la_cinematique) {
    const SpeedLimits limits;
    const DrivingAdvisor advisor;
    const double a = advisor.config().service_decel;
    const double d = 100.0;
    const double attendu_kmh = std::sqrt(2.0 * a * d) * 3.6;

    const DrivingAdvice adv = advisor.advise(limits, 1000.0, 10.0, 1000.0 + d, 0.0);
    CHECK(adv.stop_ahead);
    CHECK_NEAR(adv.stop_distance, d, 1e-9);
    CHECK_NEAR(adv.target_kmh, attendu_kmh, 1e-6);
}

// Le point de freinage : à 90 km/h il faut v²/(2a) mètres pour s'arrêter. Le compte à
// rebours affiché au pupitre est ce qui RESTE avant d'y être.
NOIRE_TEST(point_de_freinage_est_la_distance_restante) {
    const SpeedLimits limits;
    const DrivingAdvisor advisor;
    const double v = 90.0 / 3.6;
    const double a = advisor.config().service_decel;
    const double freinage = v * v / (2.0 * a);
    const double stop = 1000.0 + freinage + 200.0;

    const DrivingAdvice adv = advisor.advise(limits, 1000.0, v, stop, 0.0);
    CHECK_NEAR(adv.distance_to_brake_point, 200.0, 1e-6);
}

// La PENTE travaille déjà pour le conducteur : en rampe, elle vaut près d'un cran de
// frein. L'aide ne doit conseiller que le complément, sous peine de sur-freiner
// systématiquement et de faire s'arrêter la rame avant son repère.
NOIRE_TEST(le_cran_conseille_deduit_la_pente) {
    const SpeedLimits limits;
    const DrivingAdvisor advisor;
    const double v = 60.0 / 3.6;

    const DrivingAdvice plat = advisor.advise(limits, 1000.0, v, 1100.0, 0.0);
    const DrivingAdvice rampe = advisor.advise(limits, 1000.0, v, 1100.0, 0.30);

    CHECK(plat.recommended_notch > 0);
    CHECK(rampe.required_brake_decel < plat.required_brake_decel);
    CHECK(rampe.recommended_notch <= plat.recommended_notch);
}

// Repère franchi : la consigne reste ZÉRO tant que l'appelant désigne cet arrêt-là.
// Sans ça, une rame arrêtée 30 cm trop loin se voyait conseiller d'accélérer, portes
// encore fermées, vers la gare suivante.
NOIRE_TEST(consigne_nulle_une_fois_le_repere_franchi) {
    const SpeedLimits limits;
    const DrivingAdvisor advisor;
    const DrivingAdvice a = advisor.advise(limits, 1000.30, 0.0, 1000.0, 0.0);
    CHECK(a.stop_ahead);
    CHECK(a.stop_distance < 0.0);
    CHECK_NEAR(a.target_kmh, 0.0, 1e-9);
}

// --- Le module EN BOUCLE FERMÉE ---------------------------------------------

// L'AFFIRMATION DU README, rendue exécutable : « un conducteur qui suit la consigne à la
// lettre depuis 90 km/h immobilise la rame sur son repère ». C'était une mesure faite une
// fois à la main ; c'est maintenant un cas qui échoue le jour où quelqu'un touche à
// `service_decel`, à la masse de la rame ou au frein.
//
// Le « conducteur » est le plus bête possible, et c'est volontaire : il applique le cran
// que l'aide affiche, rien d'autre. Si l'aide est juste, cela suffit.
//
// CE QUI EST ASSERTÉ, ET POURQUOI PAS LE CENTIMÈTRE. L'assertion dure porte sur le SENS
// de l'erreur : la rame ne doit JAMAIS dépasser son repère. C'est la propriété de
// sécurité, et c'est exactement celle qu'un `service_decel` devenu trop optimiste casse.
// Le chiffre au centimètre, lui, dépend du modèle de conducteur autant que de l'aide :
// un conducteur bang-bang qui arrondit son cran PAR EXCÈS (c'est le choix documenté de
// DrivingAdvisor) s'arrête structurellement un peu court, et le mesurer ici reviendrait
// à tester le conducteur de test plutôt que l'aide. La cote fine se lit en jeu, sur la
// règle graduée du pupitre.
//
// Le conducteur ne remet PAS de traction dans les 30 derniers mètres : aucun conducteur
// ne le fait, et surtout cela garantit que le seul chemin vers un dépassement passe par
// une enveloppe fausse — ce qui est précisément ce qu'on veut détecter.
NOIRE_TEST(suivre_la_consigne_arrete_la_rame_sur_le_repere) {
    const GradeTrack track{0.0};
    WagonConfig wc = metro_config();
    ConsistConfig cc;
    cc.car_count = 2;
    cc.ats_margin_kmh = 5.0;

    Consist consist{wc, cc};
    consist.attach(&track);

    const double depart = 1000.0;
    const double repere = depart + 900.0;  // de quoi freiner confortablement depuis 90
    consist.place_at(depart);
    consist.set_speed(90.0 / 3.6);

    const DrivingAdvisor advisor;
    const noire::physics::Wagon& w = consist.loco();

    for (int i = 0; i < 120 * 300; ++i) {  // 300 s de garde-fou
        const double v = std::abs(w.speed());
        // natural_decel : la copie exacte de ce que calcule l'app (Impl::natural_decel).
        const double davis = wc.davis_a + wc.davis_b * v + wc.davis_c * v * v;
        const double natural = davis / wc.mass + 9.81 * w.grade_percent() / 100.0;

        const DrivingAdvice adv =
            advisor.advise(consist.speed_limits(), w.chainage(), v, repere, natural);

        double throttle = 0.0;
        double brake = 0.0;
        if (adv.recommended_notch > 0) {
            brake = static_cast<double>(adv.recommended_notch) / 8.0;
        } else if (v * 3.6 < adv.target_kmh - 6.0 && adv.stop_distance > 30.0) {
            throttle = 0.2;  // un cran de traction, comme le ferait un conducteur
        }
        consist.set_controls(throttle, brake, false, brake > 0.0);
        consist.update(kDt);

        if (w.speed() <= 0.0 && adv.stop_distance < 30.0) {
            break;
        }
    }

    const double erreur = w.chainage() - repere;  // > 0 = repère DÉPASSÉ
    CHECK_NEAR(w.speed(), 0.0, 1e-9);
    // 1. AUCUN DÉPASSEMENT. L'assertion qui compte : l'enveloppe a tenu jusqu'au bout.
    CHECK(erreur <= kStopTolerance);
    // 2. Et elle a bien mené la rame jusqu'à la gare. Borne volontairement large (cf.
    //    l'en-tête) : elle n'est là que pour attraper une consigne devenue absurdement
    //    pessimiste, qui immobiliserait la rame en pleine ligne.
    CHECK(erreur > -150.0);
}

}  // namespace
