#pragma once

#include <cstdint>

#include "noire/core/math.hpp"  // glm

namespace noire::render {

// Format de sommet unique pour le M2 : position + couleur.
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};

// Topologie d'un maillage (abstraction de VkPrimitiveTopology côté API publique).
enum class Topology {
    Triangles,
    Lines,
};

// Identifiant opaque d'un maillage géré par le Renderer.
using MeshId = std::uint32_t;

}  // namespace noire::render
