#pragma once

namespace noire {

// PROFIL DE LIMITATION DE VITESSE (M17, en dur au M17.5) — la « carte » des vitesses le
// long de la voie, calquée sur un vrai départ de ligne française :
//     0 – 2 km   :  30 km/h   (zone de gare / dépôt)
//     2 – 15 km  : 160 km/h   (ligne classique)
//    15 – 20 km  : 220 km/h   (raccordement LGV)
//    20 km et +  : 320 km/h   (LGV pleine ligne)
//
// Le chainage sert d'abscisse (arc_rate ≈ 1 sur notre tracé => chainage ≈ km réels). C'est
// l'unique source de vérité, partagée par le KVB (qui l'applique) et par les panneaux
// trackside (qui l'affichent aux points de transition EXACTS) : ils ne peuvent pas se
// contredire.
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
