#pragma once

#include <cstdint>
#include <vector>

#include "noire/render/vertex.hpp"

namespace noire::resource {

// Image décodée côté CPU (RGBA8), produite par le loader stb sur un worker.
// `color_space` est dicté par le RÔLE de l'image dans le matériau, pas par le fichier :
// une base color est du sRGB, une metallic-roughness ou une normal map sont des données
// numériques. Une même image servant deux rôles produit donc deux entrées.
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;  // width * height * 4, RGBA8
    render::TextureFormat color_space = render::TextureFormat::SrgbColor;
    [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// Matériau PBR metallic-roughness décodé (CPU). Les index pointent dans
// ModelData::images ; -1 = pas de texture pour ce rôle => secours côté renderer.
// Les valeurs par défaut sont celles de la spec glTF.
struct MaterialData {
    int base_color_image = -1;
    int metallic_roughness_image = -1;
    int normal_image = -1;
    glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
};

// Une primitive de modèle décodée (CPU) : sommets + indices + matériau.
struct PrimitiveData {
    std::vector<render::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    int material_index = -1;  // index dans ModelData::materials (-1 = matériau par défaut)
};

// Modèle décodé (CPU) : produit par le loader cgltf sur un worker. ZÉRO Vulkan.
struct ModelData {
    std::vector<PrimitiveData> primitives;
    std::vector<ImageData> images;
    std::vector<MaterialData> materials;
    [[nodiscard]] bool valid() const { return !primitives.empty(); }
};

}  // namespace noire::resource
