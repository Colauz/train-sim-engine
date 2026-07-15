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

// Format de sommet des modèles chargés (M7) et de la voie (M8) : position + normale
// + UV + tangente. Alimente le chemin indexé device-local (create_mesh_indexed) et le
// pipeline texturé/éclairé.
//
// `tangent` suit la CONVENTION glTF : xyz = tangente (sens des u croissants), w =
// handedness (±1). La bitangente se reconstruit par cross(normal, tangent.xyz) * w —
// on ne la stocke donc pas. Indispensable au normal mapping (M8 étape 3+).
struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent;
};

// Topologie d'un maillage (abstraction de VkPrimitiveTopology côté API publique).
enum class Topology {
    Triangles,
    Lines,
};

// Identifiant opaque d'un maillage géré par le Renderer.
using MeshId = std::uint32_t;

// Identifiant opaque d'une texture gérée par le Renderer. 0 => texture de secours
// adaptée au rôle (blanc, metallic-roughness neutre, ou normale plate).
using TextureId = std::uint32_t;

// Identifiant opaque d'un matériau PBR (= un descriptor set 1 : 3 textures). 0 dans un
// DrawItem => le matériau par défaut (tout en textures de secours).
using MaterialId = std::uint32_t;

// Espace colorimétrique d'une texture — détermine le format Vulkan, donc si le
// matériel applique la conversion sRGB->linéaire à l'échantillonnage.
//   SrgbColor  : couleurs vues par l'œil (base color)         => R8G8B8A8_SRGB
//   LinearData : données numériques (metallic/roughness,
//                normal map) qu'il ne faut SURTOUT pas décoder => R8G8B8A8_UNORM
enum class TextureFormat {
    SrgbColor,
    LinearData,
};

}  // namespace noire::render
