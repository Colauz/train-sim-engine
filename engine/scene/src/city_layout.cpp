#include "noire/scene/city_layout.hpp"

#include <cmath>

#include "noire/core/terrain.hpp"

namespace noire::scene {

bool is_space_clear(const Terrain& terrain, double wx, double wz, double footprint,
                    const ExclusionZones& zones) {
    // UN SEUL balayage de spline pour les deux règles : distance_to_track rend la
    // distance à l'axe ET le chainage du point le plus proche. Les gares étant
    // repérées par leur chainage, c'est ce couple qui permet de tout trancher.
    double chainage = 0.0;
    const double d_axis = terrain.distance_to_track(wx, wz, &chainage);

    // Règle 1 — la voie.
    if (d_axis - footprint < zones.track_clearance) {
        return false;
    }

    // Règle 2 — la gare la plus proche, en coordonnées (chainage, écart latéral).
    // `along` est la distance au SEGMENT de quai le long de la voie : nulle tant
    // qu'on est à hauteur des quais, elle ne croît qu'au-delà de leurs extrémités.
    // La combinaison hypot(latéral, longitudinal) est donc exactement la distance à
    // la capsule — et se réduit au disque de rayon `station_clearance` autour du
    // centre si la gare était ponctuelle.
    if (zones.station_spacing > 0.0) {
        const double s_station =
            std::round(chainage / zones.station_spacing) * zones.station_spacing;
        const double along =
            std::max(0.0, std::abs(chainage - s_station) - zones.station_length * 0.5);
        const double d_station = std::hypot(d_axis, along);
        if (d_station - footprint < zones.station_clearance) {
            return false;
        }
    }
    return true;
}

}  // namespace noire::scene
