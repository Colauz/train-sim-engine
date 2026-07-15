#pragma once

#include <cstdint>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

struct RailProfile {
    double gauge = 1.435;
    float rail_half_width = 0.06f;
    float rail_height = 0.12f;
    double sample_step = 1.0;  // pas d'échantillonnage le long de la voie (m)
    // Période de répétition de l'UV le long du rail (m) : u = chainage / uv_period.
    float uv_period = 2.0f;
};

// Maillage de voie généré : sommets PBR + indices, prêts pour create_mesh_indexed.
// La COULEUR n'est plus portée par les sommets (M8 étape 3) : elle appartient au
// matériau (base_color_factor), et l'éclairage réel remplace l'ancien ombrage par face.
struct RailMeshData {
    std::vector<render::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    [[nodiscard]] bool empty() const { return vertices.empty() || indices.empty(); }
};

// Extrude deux rails le long de la voie sur la plage de chainage [x_start, x_end].
// Sommets exprimés RELATIVEMENT à `origin` (float) => compatible origine flottante,
// chaque chunk ayant sa propre origine. Fonction PURE (aucun état, aucune API GPU)
// => appelable depuis un worker thread.
[[nodiscard]] RailMeshData generate_rail_mesh(const TrackSource& track, double x_start,
                                              double x_end, const WorldPosition& origin,
                                              const RailProfile& profile = {});

}  // namespace noire::scene
