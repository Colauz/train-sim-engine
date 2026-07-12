#pragma once

#include "noire/core/math.hpp"
#include "noire/core/spline.hpp"

namespace noire::physics {

// Bogie CINÉMATIQUE (M3) : contraint sur une voie (Spline), il avance à vitesse
// constante et s'oriente sur la tangente de la courbe. C'est la base de la future
// dynamique ferroviaire (efforts d'attelage, adhérence, suspensions — M+).
//
// Position en double (origine flottante) ; matrice d'orientation en float.
class Bogie {
public:
    void attach(const Spline* track) { track_ = track; }
    void set_distance(double s) { distance_ = s; }
    void set_speed(double meters_per_second) { speed_ = meters_per_second; }

    // Appelé dans la boucle fixed_update (pas de temps déterministe).
    void update(double dt);

    [[nodiscard]] const WorldPosition& position() const { return position_; }
    [[nodiscard]] const glm::mat4& orientation() const { return orientation_; }
    [[nodiscard]] double distance() const { return distance_; }
    [[nodiscard]] double speed() const { return speed_; }

private:
    const Spline* track_ = nullptr;  // non-possédant
    double distance_ = 0.0;          // abscisse curviligne le long de la voie (m)
    double speed_ = 0.0;             // m/s (signe = sens de marche)

    WorldPosition position_{0.0, 0.0, 0.0};
    glm::mat4 orientation_{1.0f};
};

}  // namespace noire::physics
