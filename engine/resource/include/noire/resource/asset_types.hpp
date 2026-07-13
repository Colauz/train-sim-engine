#pragma once

#include <cstdint>
#include <vector>

#include "noire/render/vertex.hpp"

namespace noire::resource {

// Image décodée côté CPU (RGBA8), produite par le loader stb sur un worker.
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;  // width * height * 4, RGBA8
    [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// Une primitive de modèle décodée (CPU) : sommets + indices + texture de base.
struct PrimitiveData {
    std::vector<render::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    int image_index = -1;  // index dans ModelData::images (-1 = pas de texture)
};

// Modèle décodé (CPU) : produit par le loader cgltf sur un worker. ZÉRO Vulkan.
struct ModelData {
    std::vector<PrimitiveData> primitives;
    std::vector<ImageData> images;
    [[nodiscard]] bool valid() const { return !primitives.empty(); }
};

}  // namespace noire::resource
