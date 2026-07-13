#pragma once

#include <cstdint>

#include "noire/core/math.hpp"  // glm

namespace noire::render {

// Format de sommet du M2 : position + couleur (grille, rails procéduraux, cube de
// secours) — dessiné par le pipeline « debug » à couleur par sommet.
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
};

// Format de sommet des modèles chargés (M7) : position + normale + UV. Alimente le
// chemin indexé device-local (create_mesh_indexed) et, dès l'étape 3, le pipeline
// texturé/éclairé.
struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// Topologie d'un maillage (abstraction de VkPrimitiveTopology côté API publique).
enum class Topology {
    Triangles,
    Lines,
};

// Identifiant opaque d'un maillage géré par le Renderer.
using MeshId = std::uint32_t;

// Identifiant opaque d'une texture gérée par le Renderer. 0 => texture de secours
// (blanche 1x1) : c'est aussi la valeur par défaut d'un DrawItem non texturé.
using TextureId = std::uint32_t;

}  // namespace noire::render
