#include "noire/scene/ground_plane.hpp"

namespace noire::scene {

RailMeshData generate_ground_plane(double half_extent, double uv_period) {
    RailMeshData out;
    const auto h = static_cast<float>(half_extent);
    const glm::vec3 n(0.0f, 1.0f, 0.0f);       // normale verticale : éclairage uniforme
    const glm::vec4 t(1.0f, 0.0f, 0.0f, 1.0f);

    // UV EN MÈTRES divisés par la période, CENTRÉS sur l'origine du plan : le point
    // d'ancrage est en (0, 0) de l'espace texture, donc un ré-ancrage sur une
    // période entière ne déplace pas le motif d'un texel.
    //
    // À 12 km de demi-côté et 4 m de tuile, la coordonnée de coin vaut 3000 : un
    // float32 y résout encore 0,4 mm, très en deçà d'un texel. La crainte du M51 sur
    // la quantification ne se vérifie donc pas — ce qui se voyait alors, c'était le
    // second sol, pas les UV.
    const float uv = static_cast<float>(half_extent / uv_period);
    out.vertices = {
        render::MeshVertex{{-h, 0.0f, h}, n, {-uv, uv}, t},
        render::MeshVertex{{h, 0.0f, h}, n, {uv, uv}, t},
        render::MeshVertex{{h, 0.0f, -h}, n, {uv, -uv}, t},
        render::MeshVertex{{-h, 0.0f, -h}, n, {-uv, -uv}, t},
    };
    out.indices = {0, 1, 2, 0, 2, 3};
    return out;
}

}  // namespace noire::scene
