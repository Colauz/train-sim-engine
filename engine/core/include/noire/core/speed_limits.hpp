#pragma once

namespace noire {

// M55 — ENTRAXE DES GARES, définition UNIQUE.
//
// Il en existait deux : 2 000 m côté géométrie (application.cpp, qui pose les quais,
// les verrières et les points d'arrêt) et 1 200 m côté profil de vitesse
// (speed_limits.cpp, qui décide des limites ATS). Les deux mondes ne se
// rencontraient donc jamais : l'ATS imposait 45 km/h en pleine ligne et autorisait
// 90 km/h à l'entrée d'un quai. Tant qu'aucune aide n'affichait la limite à venir,
// l'incohérence restait discrète ; une consigne de conduite, elle, la rend absurde —
// elle ferait ralentir là où il n'y a rien et lancerait la rame vers son arrêt.
//
// Toute géométrie et tout profil de vitesse dérivent désormais de CETTE constante.
inline constexpr double kStationSpacing = 2000.0;

// PROFIL DE LIMITATION DE VITESSE — ligne de métro japonais (M30), cf. speed_limits.cpp.
// Aspects ATS/ATC : G (proceed, 90), YG (reduced, 65), Y (caution, 45), R (stop, 0).
// C'est l'unique source de vérité, partagée par l'ATS (qui l'applique) et par les signaux
// trackside (qui l'affichent aux points de transition EXACTS) : ils ne peuvent pas mentir.
class SpeedLimits {
public:
    // Limite (km/h) applicable au chainage x = celle de la zone qui le contient.
    [[nodiscard]] double limit_kmh(double chainage) const;

    // Zones = panneaux. Chaque zone commence à un chainage et porte une limite ; un panneau
    // se dresse à ce chainage pour l'annoncer.
    [[nodiscard]] int zone_count() const;
    [[nodiscard]] double zone_start(int zone) const;   // chainage du panneau
    [[nodiscard]] double zone_limit(int zone) const;   // limite affichée

    // Palier de couleur (0 = rouge sévère .. 4 = vert libre) pour une limite donnée.
    [[nodiscard]] int tier_for_limit(double limit_kmh) const;
};

}  // namespace noire
