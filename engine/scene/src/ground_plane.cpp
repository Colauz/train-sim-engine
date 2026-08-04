#include "noire/scene/ground_plane.hpp"

namespace noire::scene {

RailMeshData generate_ground_plane(double half_extent) {
    RailMeshData out;
    const auto h = static_cast<float>(half_extent);
    const glm::vec3 n(0.0f, 1.0f, 0.0f);       // normale verticale : éclairage uniforme
    const glm::vec4 t(1.0f, 0.0f, 0.0f, 1.0f);

    // UV constants : la couleur est UNIE, il n'y a rien à étirer. Des UV en mètres
    // (wx / 8) vaudraient ici 1500, et leur quantification en float32 se verrait.
    out.vertices = {
        render::MeshVertex{{-h, 0.0f, h}, n, {0.0f, 0.0f}, t},
        render::MeshVertex{{h, 0.0f, h}, n, {1.0f, 0.0f}, t},
        render::MeshVertex{{h, 0.0f, -h}, n, {1.0f, 1.0f}, t},
        render::MeshVertex{{-h, 0.0f, -h}, n, {0.0f, 1.0f}, t},
    };
    out.indices = {0, 1, 2, 0, 2, 3};
    return out;
}

}  // namespace noire::scene
