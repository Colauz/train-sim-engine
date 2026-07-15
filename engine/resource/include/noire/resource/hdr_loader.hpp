#pragma once

#include <cstdint>
#include <string>

#include "noire/resource/asset_types.hpp"

namespace noire::resource {

// Charge un ciel HDR équirectangulaire (.hdr Radiance, via stb_image) et le
// rééchantillonne en cubemap RGBA16F. Tourne sur un worker du JobSystem : ZÉRO Vulkan.
//
// Faire la conversion ici plutôt que sur le GPU évite une passe de rendu vers cubemap
// (ou un pipeline compute) et évite surtout de téléverser l'équirect : seules les 6
// faces montent en VRAM.
//
// `face_size` = côté d'une face. 1024 => 6*1024²*8 o = 48 Mio, +33 % avec les mips.
bool load_environment_file(const std::string& path, std::uint32_t face_size, CubemapData& out);

}  // namespace noire::resource
