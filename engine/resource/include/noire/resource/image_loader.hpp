#pragma once

#include <cstddef>
#include <string>

#include "noire/resource/asset_types.hpp"

namespace noire::resource {

// Décode un fichier image (PNG/JPG/...) en RGBA8. false si introuvable/illisible.
bool load_image_file(const std::string& path, ImageData& out);

// Décode une image depuis un tampon mémoire (ex. texture embarquée d'un GLB).
bool decode_image_memory(const unsigned char* data, std::size_t size, ImageData& out);

}  // namespace noire::resource
