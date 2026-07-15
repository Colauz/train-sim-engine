#include "noire/core/terrain.hpp"

#include <algorithm>
#include <cmath>

namespace noire {

namespace {

// Hachage entier : remplace la table de permutation classique du simplex. Sans table,
// le bruit est SANS ÉTAT — donc thread-safe et sans initialisation, ce qui compte ici
// puisque les workers l'appellent en parallèle.
std::uint32_t hash2(std::int32_t x, std::int32_t y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                      static_cast<std::uint32_t>(y) * 668265263u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}

// 8 gradients unitaires-ish : suffisant en 2D, et évite la division d'un vrai cercle.
const double kGrad[8][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1},
                            {1, 0}, {-1, 0}, {0, 1},  {0, -1}};

double grad_dot(std::int32_t i, std::int32_t j, double x, double y) {
    const double* g = kGrad[hash2(i, j) & 7u];
    return g[0] * x + g[1] * y;
}

std::int32_t fast_floor(double v) {
    const auto i = static_cast<std::int32_t>(v);
    return v < static_cast<double>(i) ? i - 1 : i;
}

// Bruit SIMPLEX 2D (Perlin 2001). Préféré au bruit de valeur qu'on utilisait pour le sol :
// celui-ci laisse apparaître sa grille en damier dès qu'on l'étale sur des kilomètres,
// là où le simplex, bâti sur une grille TRIANGULAIRE, n'a pas de direction privilégiée.
double simplex2(double xin, double yin) {
    static const double F2 = 0.5 * (std::sqrt(3.0) - 1.0);  // facteur d'inclinaison
    static const double G2 = (3.0 - std::sqrt(3.0)) / 6.0;  // et son inverse

    // 1) On incline le plan pour ramener la grille triangulaire sur une grille carrée.
    const double s = (xin + yin) * F2;
    const std::int32_t i = fast_floor(xin + s);
    const std::int32_t j = fast_floor(yin + s);

    const double t = static_cast<double>(i + j) * G2;
    const double x0 = xin - (static_cast<double>(i) - t);
    const double y0 = yin - (static_cast<double>(j) - t);

    // 2) Dans quel des deux triangles de la cellule sommes-nous ?
    const std::int32_t i1 = x0 > y0 ? 1 : 0;
    const std::int32_t j1 = x0 > y0 ? 0 : 1;

    const double x1 = x0 - static_cast<double>(i1) + G2;
    const double y1 = y0 - static_cast<double>(j1) + G2;
    const double x2 = x0 - 1.0 + 2.0 * G2;
    const double y2 = y0 - 1.0 + 2.0 * G2;

    // 3) Contribution des 3 sommets du triangle, pondérée par un noyau à support compact
    //    (nul au-delà de 0.5) : c'est ce qui rend le simplex O(1) et sans discontinuité.
    double n = 0.0;
    double t0 = 0.5 - x0 * x0 - y0 * y0;
    if (t0 > 0.0) {
        t0 *= t0;
        n += t0 * t0 * grad_dot(i, j, x0, y0);
    }
    double t1 = 0.5 - x1 * x1 - y1 * y1;
    if (t1 > 0.0) {
        t1 *= t1;
        n += t1 * t1 * grad_dot(i + i1, j + j1, x1, y1);
    }
    double t2 = 0.5 - x2 * x2 - y2 * y2;
    if (t2 > 0.0) {
        t2 *= t2;
        n += t2 * t2 * grad_dot(i + 1, j + 1, x2, y2);
    }
    return 70.0 * n;  // normalisation empirique classique => ~[-1, 1]
}

double smoothstep(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

Terrain::Terrain(const TrackSource& track, TerrainConfig config)
    : track_(track), config_(config) {
    // L'origine se DÉDUIT de la voie : à chainage 0, position.x vaut l'origine. Évite de
    // la passer en paramètre et de risquer une désynchronisation avec l'app.
    glm::dvec3 pos;
    glm::dvec3 tangent;
    track_.sample(0.0, pos, tangent);
    origin_x_ = pos.x;
}

double Terrain::fbm(double x, double z) const {
    double sum = 0.0;
    double amp = 1.0;
    double freq = 1.0 / config_.base_wavelength;
    double norm = 0.0;
    for (int o = 0; o < config_.octaves; ++o) {
        sum += amp * simplex2(x * freq, z * freq);
        norm += amp;
        amp *= 0.5;   // chaque octave pèse deux fois moins...
        freq *= 2.0;  // ...et ondule deux fois plus vite
    }
    return norm > 0.0 ? sum / norm : 0.0;  // ~[-1, 1]
}

double Terrain::distance_to_track(double wx, double wz) const {
    glm::dvec3 pos;
    glm::dvec3 tangent;
    track_.sample(wx - origin_x_, pos, tangent);
    return std::abs(wz - pos.z);
}

double Terrain::height(double wx, double wz) const {
    glm::dvec3 pos;
    glm::dvec3 tangent;
    track_.sample(wx - origin_x_, pos, tangent);

    // Plateforme : le terrain sous la voie affleure le pied du ballast.
    const double platform = pos.y - config_.ballast_depth;
    const double natural = config_.amplitude * fbm(wx, wz);

    // LE mélange. w = 0 sur la plateforme, 1 en pleine campagne. Entre les deux, la
    // transition EST le talus : remblai là où le terrain naturel est plus bas que la
    // voie, tranchée là où il est plus haut. Rien de spécial à coder pour l'un ou
    // l'autre — c'est le même smoothstep qui creuse et qui remblaie.
    const double w = smoothstep(config_.corridor_inner, config_.corridor_outer,
                                std::abs(wz - pos.z));
    return platform * (1.0 - w) + natural * w;
}

glm::dvec3 Terrain::normal(double wx, double wz, double step) const {
    // Différences CENTRÉES : deux fois plus précises qu'un différentiel avant, pour le
    // même nombre d'appels une fois le point central déjà connu.
    const double hx = height(wx + step, wz) - height(wx - step, wz);
    const double hz = height(wx, wz + step) - height(wx, wz - step);
    // Gradient (dh/dx, dh/dz) => normale (-dh/dx, 1, -dh/dz), normalisée.
    return glm::normalize(glm::dvec3(-hx / (2.0 * step), 1.0, -hz / (2.0 * step)));
}

}  // namespace noire
