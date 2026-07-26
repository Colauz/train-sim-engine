#pragma once

namespace noire {

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
