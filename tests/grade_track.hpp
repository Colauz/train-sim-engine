#pragma once

#include <cmath>

#include "noire/core/track_source.hpp"

namespace noire::test {

// VOIE D'ESSAI : une droite de pente constante, paramétrée EN LONGUEUR D'ARC.
//
// `arc_rate() == 1` exactement : un pas de chainage est un mètre de rail. C'est ce qui
// rend les attendus lisibles — « après 10 s à 0,108 m/s², la rame a reculé de 5,4 m »
// se vérifie au mètre, sans passer par une conversion arc<->paramètre. La voie réelle
// (ProceduralTrack) serpente et ondule ; ce n'est pas ce qu'on teste ici.
//
// `grade_percent` suit la convention du moteur : POSITIF = ça monte dans le sens des
// chainages croissants, et Wagon::grade_percent() doit le rendre à l'identique.
class GradeTrack final : public noire::TrackSource {
public:
    explicit GradeTrack(double grade_percent) {
        const double slope = grade_percent / 100.0;  // = tan(theta)
        sin_ = slope / std::sqrt(1.0 + slope * slope);
        cos_ = std::sqrt(1.0 - sin_ * sin_);
    }

    void sample(double x, glm::dvec3& position, glm::dvec3& tangent) const override {
        position = glm::dvec3(0.0, x * sin_, x * cos_);
        tangent = glm::dvec3(0.0, sin_, cos_);  // déjà normalisée
    }

    [[nodiscard]] double arc_rate(double /*x*/) const override { return 1.0; }

private:
    double sin_ = 0.0;
    double cos_ = 1.0;
};

}  // namespace noire::test
