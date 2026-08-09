#include "noire/core/driving_advisor.hpp"

#include <algorithm>
#include <cmath>

#include "noire/core/speed_limits.hpp"

namespace noire {

namespace {

constexpr double kMsToKmh = 3.6;
constexpr double kKmhToMs = 1.0 / 3.6;

// Vitesse maximale admissible ICI pour se présenter à `distance` mètres d'ici à la
// vitesse `target_ms`, en décélérant à `decel`. C'est l'unique équation de tout le
// module — le reste n'est que le choix des contraintes à lui soumettre.
[[nodiscard]] double approach_speed(double target_ms, double distance, double decel) {
    if (distance <= 0.0) {
        return target_ms;  // on y est (ou on l'a dépassée) : plus de marge à dépenser
    }
    return std::sqrt(target_ms * target_ms + 2.0 * decel * distance);
}

}  // namespace

DrivingAdvice DrivingAdvisor::advise(const SpeedLimits& limits, double chainage,
                                     double speed_ms, double stop_chainage,
                                     double natural_decel) const {
    DrivingAdvice advice;
    const double a = std::max(0.05, config_.service_decel);

    advice.limit_kmh = limits.limit_kmh(chainage);

    // --- Contrainte 1 : la limite ATS ICI ------------------------------------
    // La marge est retranchée, jamais en dessous de zéro : sur une zone à 0 km/h
    // (terminus), la consigne est bien l'arrêt, pas une vitesse négative.
    double target_kmh = std::max(0.0, advice.limit_kmh - config_.limit_margin_kmh);
    // Distance de la contrainte qui DÉTERMINE la consigne. Elle sert à traduire l'excès
    // de vitesse en cran de frein : résorber 20 km/h en 100 m ou en 800 m, ce n'est pas
    // le même cran. La première version prenait toujours la distance à l'arrêt, ce qui
    // sous-freinait chaque fois qu'un abaissement de limite, plus proche, dominait.
    // 50 m par défaut : une limite déjà en vigueur ici se corrige tout de suite.
    double binding_distance = 50.0;

    // --- Contrainte 2 : chaque limite PLUS BASSE à venir ----------------------
    // On ne retient que les abaissements : une zone plus rapide devant ne contraint
    // rien (on y accélérera une fois le panneau franchi, pas avant). Et on borne à
    // `lookahead` — au-delà, la racine carrée rend une vitesse si haute qu'elle ne
    // pourrait de toute façon jamais devenir le minimum.
    const int zones = limits.zone_count();
    for (int z = 0; z < zones; ++z) {
        const double start = limits.zone_start(z);
        const double distance = start - chainage;
        if (distance <= 0.0 || distance > config_.lookahead) {
            continue;
        }
        const double zone_kmh = limits.zone_limit(z);
        if (zone_kmh >= advice.limit_kmh) {
            continue;  // pas un abaissement
        }
        const double allowed_kmh =
            approach_speed(std::max(0.0, zone_kmh - config_.limit_margin_kmh) * kKmhToMs,
                           distance, a) * kMsToKmh;
        if (allowed_kmh < target_kmh) {
            target_kmh = allowed_kmh;
            binding_distance = distance;
        }
    }

    // --- Contrainte 3 : le point d'arrêt en gare ------------------------------
    // Vitesse cible LÀ-BAS : zéro. C'est la contrainte la plus dure de la ligne, et
    // c'est elle qui, sur les derniers centaines de mètres, devient le minimum.
    advice.stop_distance = stop_chainage - chainage;
    // La contrainte s'applique dès que le point d'arrêt est dans la fenêtre — Y COMPRIS
    // s'il est DÉJÀ DERRIÈRE. C'est l'appelant qui décide quel arrêt est servi ; tant
    // qu'il en désigne un, la consigne doit y mener. La première version la lâchait
    // dès le repère franchi : une rame arrêtée 30 cm trop loin se voyait conseiller
    // 43 km/h, portes encore fermées, alors que la seule consigne sensée était ZÉRO.
    advice.stop_ahead = advice.stop_distance <= config_.lookahead;
    if (advice.stop_ahead) {
        const double allowed_kmh =
            (advice.stop_distance > 0.0 ? approach_speed(0.0, advice.stop_distance, a) : 0.0) *
            kMsToKmh;
        if (allowed_kmh < target_kmh) {
            target_kmh = allowed_kmh;
            binding_distance = std::max(1.0, advice.stop_distance);
        }

        // « À quel moment freiner » : la distance de freinage nécessaire à la vitesse
        // ACTUELLE est v²/(2a) ; le point de freinage est donc à cette distance du
        // repère d'arrêt. Ce qui reste avant de l'atteindre est ce que le conducteur
        // veut lire pendant qu'il roule encore en palier.
        const double braking_distance = (speed_ms * speed_ms) / (2.0 * a);
        advice.distance_to_brake_point = std::max(0.0, advice.stop_distance - braking_distance);
    }

    advice.target_kmh = std::max(0.0, target_kmh);

    // --- Traduction en action : le cran de frein ------------------------------
    // On ne freine que si l'on est AU-DESSUS de l'enveloppe. La décélération à
    // trouver est celle qui ramène la vitesse actuelle sur la consigne, non pas
    // instantanément (ce serait un mur), mais à l'échéance de la contrainte qui
    // domine — c'est-à-dire sur la distance qui nous en sépare.
    const double target_ms = advice.target_kmh * kKmhToMs;
    if (speed_ms > target_ms + 0.05) {
        // Résorber l'excès sur la distance de la contrainte QUI DOMINE — celle qui a
        // fixé la consigne, et pas une autre.
        const double needed =
            (speed_ms * speed_ms - target_ms * target_ms) / (2.0 * binding_distance);
        // La pente et la résistance à l'avancement travaillent déjà pour nous : le
        // frein ne doit fournir que le complément. Sans cette soustraction, l'aide
        // sur-freine systématiquement en rampe montante.
        advice.required_brake_decel = std::max(0.0, needed - natural_decel);
        const double fraction = advice.required_brake_decel / std::max(0.01, config_.max_service_decel);
        // Cran par EXCÈS : mieux vaut être un cran trop fort — on peut toujours
        // desserrer, alors qu'un cran trop faible ne se rattrape qu'en distance, et
        // la distance, en approche de gare, ne se rattrape pas.
        advice.recommended_notch =
            std::clamp(static_cast<int>(std::ceil(fraction * 8.0)), 1, 8);
    }

    return advice;
}

}  // namespace noire
