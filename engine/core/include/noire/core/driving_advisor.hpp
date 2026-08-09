#pragma once

namespace noire {

class SpeedLimits;

// M55 — AIDE À LA CONDUITE : « quelle vitesse, à quel moment ».
//
// Le conducteur disposait de deux informations, et toutes deux ARRIVAIENT TROP TARD :
// la limite ATS courante (qui ne dit rien de celle du kilomètre suivant) et l'écart au
// point d'arrêt (qui ne devient lisible qu'une fois le freinage engagé). Entre les
// deux, rien : aucun moyen de savoir, à 400 m d'une gare et à 70 km/h, si l'on est en
// avance ou en retard sur son freinage. On ne pouvait que deviner, puis constater.
//
// Ce module calcule l'ENVELOPPE DE VITESSE : la plus grande vitesse qu'on puisse tenir
// à un instant donné tout en restant capable d'honorer TOUTES les contraintes à venir.
// C'est le principe de tout ATC/TASC réel, et il tient en une phrase : pour chaque
// contrainte future (une limite plus basse, un point d'arrêt), la cinématique donne la
// vitesse maximale admissible ici pour l'atteindre à la bonne vitesse là-bas ; la
// consigne est le MINIMUM de toutes ces vitesses.
//
//     v_max(ici) = sqrt( v_contrainte² + 2 . a . distance )
//
// Rien de plus. Ce qui fait la valeur de l'aide, ce n'est pas la formule, c'est le
// choix de `a` : trop optimiste, la consigne devient intenable et le conducteur rate
// son arrêt en la suivant ; trop pessimiste, elle fait traîner la rame.

struct DrivingAdvice {
    double target_kmh = 0.0;      // consigne à tenir MAINTENANT
    double limit_kmh = 0.0;       // limite ATS courante, pour mémoire
    double stop_distance = 0.0;   // distance au point d'arrêt (m) ; < 0 = dépassé
    // Distance restante AVANT de devoir commencer à freiner pour l'arrêt. 0 => il faut
    // freiner maintenant. C'est la réponse à « à quel moment ».
    double distance_to_brake_point = 0.0;
    bool stop_ahead = false;      // un point d'arrêt est dans la fenêtre d'anticipation
    // Décélération à obtenir pour rejoindre l'enveloppe (m/s²), FREIN SEUL : la
    // résistance à l'avancement et la pente en ont déjà été retirées. 0 => rien à faire.
    double required_brake_decel = 0.0;
    // Cran de frein conseillé : 0 = aucun (laisser rouler), 1..8 = B1..B8. Il découle
    // directement de required_brake_decel — c'est la même information, traduite dans
    // l'unité du manipulateur, qui est celle où le conducteur agit.
    int recommended_notch = 0;
};

class DrivingAdvisor {
public:
    struct Config {
        // Décélération de RÉFÉRENCE du calcul d'enveloppe. Volontairement inférieure au
        // service maximal de la rame (~1,04 m/s² : 100 kN pour 96 t) pour deux raisons
        // qui se cumulent :
        //   * le frein pneumatique n'est pas instantané — il faut plusieurs secondes
        //     pour établir l'effort (AirBrakeConfig : 0,35 bar/s sur 1,5 bar, plus la
        //     constante de temps des cylindres) ;
        //   * une consigne calée sur le maximum ne laisse AUCUNE marge de rattrapage :
        //     au moindre retard, elle devient inatteignable et l'aide ment.
        // 0,75 m/s² laisse ~30 % de réserve : suivre la consigne reste confortable, et
        // la rattraper reste possible.
        double service_decel = 0.75;
        // Marge sous la limite ATS. L'ATS déclenche à limite + ats_margin ; viser la
        // limite exacte, c'est rouler collé au déclenchement. 2 km/h suffisent.
        double limit_margin_kmh = 2.0;
        // Au-delà, une contrainte est trop lointaine pour peser sur la consigne
        // courante (à 0,75 m/s², 1 200 m couvrent déjà un freinage depuis 155 km/h).
        double lookahead = 1200.0;
        // Effort de freinage maximal de service, rapporté à la masse (m/s²) : c'est
        // l'échelle sur laquelle les 8 crans se répartissent linéairement (cf. le
        // mapping cran -> demande dans l'app).
        double max_service_decel = 1.04;
    };

    // Deux constructeurs plutôt qu'un argument par défaut `= {}` : GCC refuse cette
    // forme quand le type par défaut est une classe imbriquée à initialiseurs de
    // membres, et la contourner ici coûte une ligne.
    DrivingAdvisor() = default;
    explicit DrivingAdvisor(Config config) : config_(config) {}

    // `chainage`      : position de la rame (m)
    // `speed_ms`      : vitesse actuelle (m/s, valeur absolue attendue)
    // `stop_chainage` : chainage du point d'arrêt visé
    // `natural_decel` : décélération déjà fournie GRATUITEMENT par la résistance à
    //                   l'avancement et la pente (m/s², positive = elle ralentit). Elle
    //                   est retirée du cran conseillé : sur une rampe de 1 %, la pente
    //                   pèse 0,098 m/s², soit près d'un cran de frein — ignorer ça,
    //                   c'est faire s'arrêter la rame avant son repère.
    [[nodiscard]] DrivingAdvice advise(const SpeedLimits& limits, double chainage,
                                       double speed_ms, double stop_chainage,
                                       double natural_decel) const;

    [[nodiscard]] const Config& config() const { return config_; }

private:
    Config config_{};
};

}  // namespace noire
