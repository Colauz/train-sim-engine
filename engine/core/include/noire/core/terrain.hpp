#pragma once

#include <cstdint>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"

namespace noire {

// Paramètres du relief. M31 (Neo-Tokyo) : la ville est bâtie sur une plaine douce —
// l'ancien vallonné champenois (25 m) enterrait les immeubles ou les faisait flotter.
struct TerrainConfig {
    // Dénivelé autour du niveau moyen. 8 m suffisent à animer le sol entre les tours sans
    // jamais menacer les empreintes des bâtiments (qui s'enterrent de quelques mètres).
    double amplitude = 8.0;
    // Longueur d'onde de la 1re octave. Grande : la plaine urbaine ondule en douceur.
    double base_wavelength = 1200.0;
    int octaves = 4;                 // chaque octave double la fréquence et halve l'amplitude
    // Corridor ferroviaire : historiquement la zone aplanie autour de la voie. Depuis le
    // M31 la voie est un viaduc et le terrain est partout naturel (cf. Terrain::height) —
    // ces champs ne servent plus qu'au semis (exclusion autour de l'axe).
    double corridor_inner = 25.0;
    double corridor_outer = 120.0;
    double ballast_depth = 0.8;      // le terrain sous la voie = plan de roulement - 0.8
};

// Relief procédural, ANALYTIQUE et SANS ÉTAT : height() est pure et thread-safe, donc
// appelable simultanément depuis les workers (génération de tuiles), depuis le
// générateur de voie (accotement) et depuis le semis de végétation. Aucune LUT, aucun
// heightmap : le monde est infini et reproductible à l'identique.
class Terrain {
public:
    Terrain(const TrackSource& track, TerrainConfig config = {});

    // Altitude du sol au point monde (wx, wz).
    [[nodiscard]] double height(double wx, double wz) const;
    // Normale du sol, par différences centrées sur height().
    [[nodiscard]] glm::dvec3 normal(double wx, double wz, double step = 1.0) const;
    // Distance horizontale euclidienne à l'axe de la voie (point le plus proche sur la
    // spline, M45). Sert au semis : aucun bâtiment ne doit empiéter sur le corridor.
    //
    // M51 : `out_chainage` reçoit, si fourni, le chainage du point le plus proche —
    // il est calculé de toute façon, et c'est LUI qui permet de savoir de quelle gare
    // on est près (les gares sont à des chainages, pas à des coordonnées monde).
    // Renvoyer les deux évite un second balayage de spline par test d'occupation.
    [[nodiscard]] double distance_to_track(double wx, double wz,
                                           double* out_chainage = nullptr) const;

    [[nodiscard]] const TerrainConfig& config() const { return config_; }

private:
    [[nodiscard]] double fbm(double x, double z) const;

    const TrackSource& track_;
    TerrainConfig config_;
    double origin_x_ = 0.0;  // déduit du track : position.x à chainage 0
};

}  // namespace noire
