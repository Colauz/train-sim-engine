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

}  // namespace noire
