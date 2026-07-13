#include "noire/resource/image_loader.hpp"

#include <stb_image.h>

#include "noire/core/log.hpp"

namespace noire::resource {

namespace {

// stb rend un tampon RGBA (4 canaux forcés) ; on le recopie puis on le libère.
bool adopt(unsigned char* decoded, int width, int height, ImageData& out) {
    if (decoded == nullptr) {
        return false;
    }
    out.width = width;
    out.height = height;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    out.pixels.assign(decoded, decoded + count);
    stbi_image_free(decoded);
    return true;
}

const char* stb_reason() {
    const char* reason = stbi_failure_reason();
    return reason != nullptr ? reason : "raison inconnue";
}

}  // namespace

bool load_image_file(const std::string& path, ImageData& out) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);  // force RGBA
    if (decoded == nullptr) {
        log::warn("Image : décodage de '{}' échoué ({})", path, stb_reason());
        return false;
    }
    return adopt(decoded, width, height, out);
}

bool decode_image_memory(const unsigned char* data, std::size_t size, ImageData& out) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded =
        stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
    if (decoded == nullptr) {
        log::warn("Image : décodage mémoire échoué ({})", stb_reason());
        return false;
    }
    return adopt(decoded, width, height, out);
}

}  // namespace noire::resource
