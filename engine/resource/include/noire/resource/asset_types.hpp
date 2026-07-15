#pragma once

#include <array>
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

// Cubemap d'environnement décodée (CPU), produite par le loader HDR sur un worker.
// Les 6 faces sont contiguës dans l'ORDRE VULKAN (+X, -X, +Y, -Y, +Z, -Z), chacune
// `size * size` texels RGBA en HALF-float (d'où l'uint16 : 2 octets par canal) — c'est
// déjà le format GPU (R16G16B16A16_SFLOAT), donc l'upload est une simple memcpy.
// L'équirectangulaire source, lui, ne monte JAMAIS sur le GPU.
struct CubemapData {
    std::uint32_t size = 0;             // côté d'une face, en texels
    std::vector<std::uint16_t> texels;  // 6 * size * size * 4

    // --- Éclairage extrait du ciel (M8 étape 6b) ---
    // Le soleil est RETIRÉ des `texels` ci-dessus et republié ici : le laisser dans la
    // cubemap ET l'éclairer en directionnel le compterait deux fois (une fois par la
    // réflexion spéculaire de l'env, une fois par Cook-Torrance).
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};  // VERS le soleil (normalisée, monde)
    glm::vec3 sun_color{0.0f};                  // = irradiance / PI (cf. FrameUniforms)
    // Irradiance du ciel (soleil déjà retiré) en harmoniques sphériques d'ordre 2.
    std::array<glm::vec3, 9> sh{};

    [[nodiscard]] bool valid() const {
        return size > 0 && texels.size() == static_cast<std::size_t>(size) * size * 6u * 4u;
    }
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
