#include "noire/scene/ground_sheet.hpp"

#include <cmath>

namespace noire::scene {

RailMeshData generate_ground_sheet(const HeightSampler& height_fn,
                                   const WorldPosition& center, double range, double cell) {
    RailMeshData out;
    const glm::vec3 n(0.0f, 1.0f, 0.0f);  // normale verticale : éclairage uniforme
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
    // M49 : 10 cm au-dessus du terrain. À 2 cm, l'herbe transperçait la nappe par
    // endroits et le Z-fighting dessinait un damier gris/vert très laid.
    constexpr double lift = 0.10;

    const long i_min = static_cast<long>(std::floor((center.x - range) / cell));
    const long i_max = static_cast<long>(std::floor((center.x + range) / cell));
    const long j_min = static_cast<long>(std::floor((center.z - range) / cell));
    const long j_max = static_cast<long>(std::floor((center.z + range) / cell));

    const auto rel = [&](double wx, double wz) {
        return glm::vec3(static_cast<float>(wx - center.x),
                         static_cast<float>(height_fn(wx, wz) + lift - center.y),
                         static_cast<float>(wz - center.z));
    };

    for (long i = i_min; i < i_max; ++i) {
        for (long j = j_min; j < j_max; ++j) {
            const double x0 = static_cast<double>(i) * cell;
            const double z0 = static_cast<double>(j) * cell;
            const double x1 = x0 + cell, z1 = z0 + cell;

            const auto base = static_cast<std::uint32_t>(out.vertices.size());
            out.vertices.push_back(render::MeshVertex{rel(x0, z1), n, {0.0f, 0.0f}, tangent});
            out.vertices.push_back(render::MeshVertex{rel(x1, z1), n, {1.0f, 0.0f}, tangent});
            out.vertices.push_back(render::MeshVertex{rel(x1, z0), n, {1.0f, 1.0f}, tangent});
            out.vertices.push_back(render::MeshVertex{rel(x0, z0), n, {0.0f, 1.0f}, tangent});
            out.indices.insert(out.indices.end(),
                               {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    return out;
}

}  // namespace noire::scene
