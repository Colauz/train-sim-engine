#pragma once

#include <vector>

namespace noire::scene {

// M52 — LE PLAN DE LA RAME. Une seule description de « où sont les portes », d'où
// dérivent la physique (ConsistConfig), les baies de la façade de quai, le repère
// d'arrêt et la tolérance d'arrêt de précision.
//
// C'était le fond du bug M52 : la gare posait ses portes sur une trame de 2,5 m et
// son repère d'arrêt à 3 m du bout du quai — deux nombres sans aucun rapport avec la
// rame. Trois descriptions divergentes du même train donnent trois désalignements.
// Ici, il n'y en a plus qu'une, et tout le reste en découle par calcul.
//
// CONVENTION : toutes les cotes sont des CHAINAGES relatifs, comptés dans le sens de
// la marche (+ = vers l'avant). Attention, tools/gen_metro.py compte son axe z vers
// l'ARRIÈRE : les implantations de portes ci-dessous sont donc les opposées des
// siennes. C'est le seul couplage qu'on ne peut pas supprimer sans lire le .glb au
// démarrage — d'où les noms d'origine cités en regard de chaque cote.
struct TrainLayout {
    // --- Composition (doit alimenter physics::ConsistConfig) ------------------
    int car_count = 2;                  // voitures derrière la motrice
    double loco_half_length = 10.0;     // demi-caisse motrice (BODY_LEN / 2)
    double car_spacing = 20.5;          // entraxe des bogies Jacobs = longueur voiture
    double head_to_first_jacobs = 0.6;  // jeu motrice <-> 1er bogie Jacobs

    // --- Gabarit (doit alimenter la géométrie de gare) -----------------------
    double half_width = 1.475;    // HALF_W : caisse de 2,95 m
    double floor_height = 1.20;   // plancher au-dessus du plan de roulement (2,20 - IN_FLOOR)
    double cab_forward = 8.55;    // cabine EN AVANT du centre de caisse (gen_metro : z = -8.55)
    double door_half_width = 0.65;                        // DOOR_HALF => 1,30 m de passage
    std::vector<double> motrice_doors{6.5, 2.5, -2.5, -7.5};  // MOTRICE_CENTERS, signe inversé
    std::vector<double> car_doors{7.5, 2.5, -2.5, -7.5};      // DOOR_CENTERS, signe inversé

    // --- Repères longitudinaux, relatifs au CENTRE DE CAISSE DE LA MOTRICE ----
    // (c'est ce que rend Wagon::chainage(), donc le repère naturel du reste du code)
    [[nodiscard]] double first_jacobs() const { return -(loco_half_length + head_to_first_jacobs); }
    [[nodiscard]] double nose() const { return loco_half_length; }
    [[nodiscard]] double tail() const { return first_jacobs() - car_count * car_spacing; }
    [[nodiscard]] double length() const { return nose() - tail(); }
    [[nodiscard]] double center() const { return (nose() + tail()) * 0.5; }

    // Centre de caisse de la voiture k (0 = la première derrière la motrice) : le
    // milieu de ses deux bogies Jacobs, cf. Consist::update_running_gear.
    [[nodiscard]] double car_center(int k) const {
        return first_jacobs() - (static_cast<double>(k) + 0.5) * car_spacing;
    }

    // --- Point d'arrêt, relatif au CENTRE DE LA GARE --------------------------
    // Chainage de la motrice quand le centre de la rame coïncide avec celui de la
    // gare : c'est LE point d'arrêt idéal, celui que matérialise le repère.
    [[nodiscard]] double stop_offset() const { return -center(); }
    // Et le chainage de la cabine à cet instant : la position du losange sur le quai.
    [[nodiscard]] double stop_marker_offset() const { return stop_offset() + cab_forward; }

    // Chainage de CHAQUE porte de la rame, relatif au centre de la gare, la rame
    // étant à son point d'arrêt idéal. C'est le plan que la façade de quai recopie.
    [[nodiscard]] std::vector<double> door_chainages() const {
        std::vector<double> out;
        out.reserve(motrice_doors.size() + car_doors.size() * static_cast<std::size_t>(car_count));
        const double stop = stop_offset();
        for (const double z : motrice_doors) {
            out.push_back(stop + z);
        }
        for (int k = 0; k < car_count; ++k) {
            for (const double z : car_doors) {
                out.push_back(stop + car_center(k) + z);
            }
        }
        return out;
    }
};

}  // namespace noire::scene
