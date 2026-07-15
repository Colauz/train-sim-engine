#pragma once

#include <cstdint>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"

namespace noire {

// Paramètres du relief. Valeurs calées sur la Champagne crayeuse autour de
// Châlons-en-Champagne : vallonnée en douceur, pas de canyon.
struct TerrainConfig {
    // Dénivelé autour du niveau moyen. À 15 m, le talus moyen le long de la voie ne fait
    // que 4,4 m : la ligne effleure le terrain au lieu de le fendre, et l'oeil lit « plat ».
    // À 25 m : 6,3 m de talus moyen, 20 m au plus fort — soit exactement la fourchette
    // « 20 à 40 m » visée, sans jamais devenir un canyon.
    double amplitude = 25.0;
    // Longueur d'onde de la 1re octave. C'est elle, PAS l'amplitude, qui décide si le
    // relief se voit : à 2400 m pour 12 m d'amplitude, la pente max tombe à 4° et l'oeil
    // lit « plat » (mesuré). À 1200 m : 8,6° et ~19 m de relief par km — vallonné et
    // lisible, sans devenir alpin. En dessous de 900 m ça devient abrupt, ce qui n'est
    // plus la Champagne.
    double base_wavelength = 1200.0;
    int octaves = 4;                 // chaque octave double la fréquence et halve l'amplitude
    // Corridor ferroviaire : c'est LE mécanisme du jalon. À moins de `inner` de l'axe, le
    // terrain EST la plateforme (plat) ; au-delà de `outer`, il est purement naturel ;
    // entre les deux, l'interpolation FABRIQUE le remblai ou la tranchée — il n'y a rien
    // à synchroniser, c'est la même fonction des deux côtés.
    double corridor_inner = 25.0;
    double corridor_outer = 120.0;
    double ballast_depth = 0.8;      // le terrain sous la voie = plan de roulement - 0.8
};

// Relief procédural, ANALYTIQUE et SANS ÉTAT : height() est pure et thread-safe, donc
// appelable simultanément depuis les workers (génération de tuiles), depuis le
// générateur de voie (accotement) et depuis le semis de végétation. Aucune LUT, aucun
// heightmap : le monde est infini et reproductible à l'identique.
//
// HYPOTHÈSE : la voie progresse selon +x, donc le chainage vaut (x monde - origine).
// C'est vrai de ProceduralTrack. Une vraie ligne Châlons-Paris, aux courbes arbitraires,
// exigerait une recherche du point le plus proche sur la spline — à traiter le jour où
// la voie ne sera plus une fonction de x.
class Terrain {
public:
    Terrain(const TrackSource& track, TerrainConfig config = {});

    // Altitude du sol au point monde (wx, wz).
    [[nodiscard]] double height(double wx, double wz) const;
    // Normale du sol, par différences centrées sur height().
    [[nodiscard]] glm::dvec3 normal(double wx, double wz, double step = 1.0) const;
    // Distance horizontale à l'axe de la voie. Sert au semis de végétation : aucun arbre
    // ne doit pousser sur la plateforme.
    [[nodiscard]] double distance_to_track(double wx, double wz) const;

    [[nodiscard]] const TerrainConfig& config() const { return config_; }

private:
    [[nodiscard]] double fbm(double x, double z) const;

    const TrackSource& track_;
    TerrainConfig config_;
    double origin_x_ = 0.0;  // déduit du track : position.x à chainage 0
};

}  // namespace noire
