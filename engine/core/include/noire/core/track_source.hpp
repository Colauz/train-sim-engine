#pragma once

#include "noire/core/math.hpp"

namespace noire {

// Source de voie : définit la géométrie de la voie en fonction d'un paramètre de
// « chainage » x (progression le long de la voie). Permet une voie INFINIE et
// analytique (pas de LUT) — et surtout, comme l'implémentation est SANS ÉTAT
// mutable, l'échantillonnage est thread-safe : la physique (thread principal) et
// la génération de chunks (worker) peuvent l'appeler simultanément sans verrou.
class TrackSource {
public:
    virtual ~TrackSource() = default;

    // Position (double, world space) et tangente normalisée au chainage x.
    virtual void sample(double x, glm::dvec3& position, glm::dvec3& tangent) const = 0;

    // Taux d'arc ds/dx (>= 1) : convertit la vitesse physique (le long de l'arc)
    // en avance du paramètre x. Vaut 1 sur une voie parfaitement droite.
    [[nodiscard]] virtual double arc_rate(double x) const = 0;
};

// M52 — CHAINAGE ATTEINT EN PARCOURANT `arc` MÈTRES RÉELS depuis `x_ref`.
//
// Le chainage est un PARAMÈTRE, pas une abscisse curviligne : sur une voie sinueuse,
// avancer de 40 m de rail ne fait pas avancer x de 40. Toute géométrie censée tomber
// en face d'un organe du train doit donc être posée en distance d'ARC — c'est ainsi
// que la physique place les voitures (Consist::chainage_behind) — et convertie ici.
// L'ignorer produit une dérive proportionnelle à la longueur de la rame : quelques
// centimètres en alignement, jusqu'à ~1 % de la distance là où la voie serpente,
// soit une demi-porte à la queue d'une rame de 60 m.
//
// Point fixe évalué au MILIEU de l'intervalle courant : converge en 2-3 tours.
[[nodiscard]] inline double chainage_at_arc(const TrackSource& track, double x_ref,
                                            double arc) {
    double x = x_ref + arc;
    for (int i = 0; i < 4; ++i) {
        const double rate = track.arc_rate(0.5 * (x_ref + x));
        x = x_ref + arc / (rate > 1e-6 ? rate : 1.0);
    }
    return x;
}

}  // namespace noire
