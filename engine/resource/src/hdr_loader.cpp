#include "noire/resource/hdr_loader.hpp"

#include <glm/gtc/packing.hpp>

#include <cmath>
#include <vector>

#include <stb_image.h>

#include "noire/core/log.hpp"
#include "noire/core/math.hpp"

namespace noire::resource {

namespace {

constexpr float kPi = 3.14159265358979323846f;
// Plus grande valeur finie d'un half-float.
constexpr float kHalfMax = 65504.0f;

std::uint16_t to_half(float v) {
    // Un ciel HDR dépasse allègrement 65504 autour du disque solaire. Sans écrêtage on
    // produirait des +inf, qui contamineraient TOUTE la chaîne de mips dès le premier
    // blit (la moyenne d'un inf est un inf) : le ciel entier virerait au blanc. Le test
    // inversé attrape aussi les NaN et les négatifs au passage.
    if (!(v > 0.0f)) {
        v = 0.0f;
    }
    return glm::packHalf1x16(v < kHalfMax ? v : kHalfMax);
}

// Direction (non normalisée) d'un texel de face, selon la table de sélection de face
// cubemap de la spec Vulkan. `sc`/`tc` sont dans [-1, 1].
// Ordre des layers : 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
glm::vec3 face_direction(std::uint32_t face, float sc, float tc) {
    switch (face) {
        case 0: return {1.0f, -tc, -sc};
        case 1: return {-1.0f, -tc, sc};
        case 2: return {sc, 1.0f, tc};
        case 3: return {sc, -1.0f, -tc};
        case 4: return {sc, -tc, 1.0f};
        default: return {-sc, -tc, -1.0f};
    }
}

// Échantillonnage bilinéaire de l'équirect. u BOUCLE (la longitude est cyclique),
// v est CLAMPÉ (aux pôles il n'y a rien au-delà).
glm::vec3 sample_equirect(const float* src, int w, int h, float u, float v) {
    const float fx = u * static_cast<float>(w) - 0.5f;
    const float fy = v * static_cast<float>(h) - 0.5f;
    const auto x0 = static_cast<int>(std::floor(fx));
    const auto y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    auto wrap_x = [w](int x) { return ((x % w) + w) % w; };
    auto clamp_y = [h](int y) { return y < 0 ? 0 : (y >= h ? h - 1 : y); };

    const int xs[2] = {wrap_x(x0), wrap_x(x0 + 1)};
    const int ys[2] = {clamp_y(y0), clamp_y(y0 + 1)};

    glm::vec3 acc(0.0f);
    const float wx[2] = {1.0f - tx, tx};
    const float wy[2] = {1.0f - ty, ty};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const std::size_t idx = (static_cast<std::size_t>(ys[j]) * static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(xs[i])) * 3u;
            acc += glm::vec3(src[idx], src[idx + 1], src[idx + 2]) * (wx[i] * wy[j]);
        }
    }
    return acc;
}

}  // namespace

bool load_environment_file(const std::string& path, std::uint32_t face_size, CubemapData& out) {
    if (face_size == 0) {
        return false;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    // 3 canaux : un ciel n'a pas d'alpha, et forcer RGBA gonflerait de 33 % un tampon
    // CPU déjà lourd (2048x1024 => 25 Mio en float).
    float* src = stbi_loadf(path.c_str(), &w, &h, &channels, 3);
    if (src == nullptr || w <= 0 || h <= 0) {
        const char* reason = stbi_failure_reason();
        log::warn("HDR : décodage de '{}' échoué ({})", path,
                  reason != nullptr ? reason : "raison inconnue");
        if (src != nullptr) {
            stbi_image_free(src);
        }
        return false;
    }

    out.size = face_size;
    out.texels.resize(static_cast<std::size_t>(face_size) * face_size * 6u * 4u);

    const float inv = 1.0f / static_cast<float>(face_size);
    for (std::uint32_t face = 0; face < 6u; ++face) {
        for (std::uint32_t y = 0; y < face_size; ++y) {
            // Centre du texel (+0.5) : sans ça les faces seraient décalées d'un demi-texel
            // et les arêtes du cube ne raccorderaient pas.
            const float tc = (static_cast<float>(y) + 0.5f) * inv * 2.0f - 1.0f;
            for (std::uint32_t x = 0; x < face_size; ++x) {
                const float sc = (static_cast<float>(x) + 0.5f) * inv * 2.0f - 1.0f;
                const glm::vec3 dir = glm::normalize(face_direction(face, sc, tc));

                // Équirect standard (celui de Poly Haven) : u = longitude, v = 0 au ZÉNITH
                // (la première ligne de l'image, cf. l'en-tête « -Y 1024 +X 2048 »).
                const float u = std::atan2(dir.z, dir.x) / (2.0f * kPi) + 0.5f;
                const float v = std::acos(glm::clamp(dir.y, -1.0f, 1.0f)) / kPi;
                const glm::vec3 c = sample_equirect(src, w, h, u, v);

                const std::size_t texel =
                    ((static_cast<std::size_t>(face) * face_size + y) * face_size + x) * 4u;
                out.texels[texel] = to_half(c.r);
                out.texels[texel + 1] = to_half(c.g);
                out.texels[texel + 2] = to_half(c.b);
                out.texels[texel + 3] = to_half(1.0f);
            }
        }
    }

    stbi_image_free(src);  // 25 Mio rendus au système AVANT que le GPU ne s'en mêle
    log::info("HDR : '{}' ({}x{}) -> cubemap {}x{} ({:.1f} Mio)", path, w, h, face_size, face_size,
              static_cast<double>(out.texels.size() * sizeof(std::uint16_t)) / (1024.0 * 1024.0));
    return true;
}

}  // namespace noire::resource
