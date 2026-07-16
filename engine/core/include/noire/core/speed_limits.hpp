#pragma once

namespace noire {

// PROFIL DE LIMITATION DE VITESSE (M17) — la « carte » des vitesses le long de la voie.
//
// Fonction PURE et DÉTERMINISTE du chainage : la voie est découpée en BLOCS de longueur
// fixe (2 km), et chaque bloc porte une limite prise dans un petit jeu de paliers. C'est
// l'unique source de vérité, partagée par le KVB (qui l'applique) et par les panneaux
// trackside (qui l'affichent) — les deux ne peuvent donc jamais se contredire.
//
// Les paliers varient DOUCEMENT d'un bloc à l'autre (au plus un cran) : une chute d'un
// palier est freinable dans les 2 km du bloc au frein de service, sinon le KVB
// déclencherait l'urgence sans que le conducteur ait pu réagir.
class SpeedLimits {
public:
    // Limite (km/h) applicable au chainage x = celle du bloc qui le contient.
    [[nodiscard]] double limit_kmh(double chainage) const;
    // Limite d'un bloc donné (par son indice).
    [[nodiscard]] double limit_for_block(long block) const;

    [[nodiscard]] long block_index(double chainage) const;
    [[nodiscard]] double block_length() const { return block_length_; }
    [[nodiscard]] double block_start(long block) const;

    // Indice du palier (0..4) d'une limite — pour colorer les panneaux par gravité.
    [[nodiscard]] int tier_for_block(long block) const;

private:
    double block_length_ = 2000.0;
};

}  // namespace noire
