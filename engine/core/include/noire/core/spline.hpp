#pragma once

#include <vector>

#include "noire/core/math.hpp"

namespace noire {

// Courbe passant par des points de contrôle (Catmull-Rom, une variante de Hermite
// cubique). Entièrement en DOUBLE : indispensable à l'origine flottante.
//
// Paramétrée par la DISTANCE d'arc `s` (mètres), pas par le paramètre brut : une
// table de correspondance (LUT) arc-longueur -> paramètre est construite une fois,
// ce qui permet de faire avancer un bogie à vitesse physique constante.
class Spline {
public:
    // (Re)construit la courbe et sa LUT arc-longueur à partir des points de contrôle.
    void set_control_points(std::vector<glm::dvec3> points);

    [[nodiscard]] double length() const { return length_; }
    [[nodiscard]] bool empty() const { return control_points_.size() < 2; }

    [[nodiscard]] glm::dvec3 position_at(double s) const;
    [[nodiscard]] glm::dvec3 tangent_at(double s) const;  // direction normalisée

    // Position + tangente en un seul échantillonnage (évite un double calcul).
    void sample(double s, glm::dvec3& position, glm::dvec3& tangent) const;

private:
    // Évalue la courbe au paramètre u in [0, nb_segments] : position + dérivée (dp/du).
    void eval(double u, glm::dvec3& position, glm::dvec3& derivative) const;

    std::vector<glm::dvec3> control_points_;
    std::vector<double> lut_s_;  // longueur d'arc cumulée
    std::vector<double> lut_u_;  // paramètre correspondant
    double length_ = 0.0;
};

}  // namespace noire
