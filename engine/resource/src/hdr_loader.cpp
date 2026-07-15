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

float luminance(const glm::vec3& c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

// Direction monde d'un texel de l'équirect. Inverse exacte du mapping utilisé au
// rééchantillonnage : u = longitude, v = 0 au zénith.
glm::vec3 equirect_direction(int x, int y, int w, int h) {
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const float theta = v * kPi;                 // 0 au zénith
    const float phi = (u - 0.5f) * 2.0f * kPi;
    const float st = std::sin(theta);
    return {st * std::cos(phi), std::cos(theta), st * std::sin(phi)};
}

// Angle solide d'un texel de l'équirect : dω = sin(θ) dθ dφ. Le sin(θ) est ESSENTIEL —
// sans lui les pôles, qui sont massivement sur-échantillonnés par la projection
// équirectangulaire, écraseraient toute intégrale.
float texel_solid_angle(int y, int w, int h) {
    const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * kPi;
    return std::sin(theta) * (kPi / static_cast<float>(h)) *
           (2.0f * kPi / static_cast<float>(w));
}

// Base SH réelle d'ordre 2 (9 coefficients). L'axe « spécial » de la formule est z, alors
// que notre monde a y en haut : sans importance, c'est une base de fonctions sur la
// sphère — seule compte la cohérence entre projection (ici) et reconstruction (shader).
void sh_basis(const glm::vec3& d, float y[9]) {
    y[0] = 0.282095f;
    y[1] = 0.488603f * d.y;
    y[2] = 0.488603f * d.z;
    y[3] = 0.488603f * d.x;
    y[4] = 1.092548f * d.x * d.y;
    y[5] = 1.092548f * d.y * d.z;
    y[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    y[7] = 1.092548f * d.x * d.z;
    y[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

// Demi-angle du cône considéré comme « le soleil », et de la couronne qui sert à mesurer
// le ciel local juste autour. 6° est large pour un vrai disque solaire (0,27°) mais les
// HDRI 2k étalent le soleil sur quelques texels + un halo.
constexpr float kSunConeDeg = 6.0f;
constexpr float kSunOuterDeg = 12.0f;

// Trouve le soleil, l'ÉCRÊTE sur place dans l'équirect, et renvoie son irradiance.
// Écrêter ici (et non dans la cubemap) est volontaire : tout ce qui est calculé ensuite —
// SH et faces du cube — dérive de l'équirect, donc le soleil disparaît partout d'un coup.
void extract_sun(float* src, int w, int h, glm::vec3& out_direction, glm::vec3& out_color) {
    // 1) Le texel le plus brillant donne la direction approchée.
    float peak_lum = -1.0f;
    int peak_x = 0;
    int peak_y = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3u;
            const float l = luminance(glm::vec3(src[i], src[i + 1], src[i + 2]));
            if (l > peak_lum) {
                peak_lum = l;
                peak_x = x;
                peak_y = y;
            }
        }
    }
    const glm::vec3 peak = equirect_direction(peak_x, peak_y, w, h);

    // 2) Ciel local : luminance moyenne d'une couronne autour du cône. C'est le niveau
    //    auquel on ramènera le disque — pas zéro, sinon on creuserait un trou noir.
    const float cos_cone = std::cos(glm::radians(kSunConeDeg));
    const float cos_outer = std::cos(glm::radians(kSunOuterDeg));
    double bg_sum = 0.0;
    double bg_weight = 0.0;
    for (int y = 0; y < h; ++y) {
        const float dw = texel_solid_angle(y, w, h);
        for (int x = 0; x < w; ++x) {
            const float c = glm::dot(equirect_direction(x, y, w, h), peak);
            if (c <= cos_cone && c > cos_outer) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3u;
                bg_sum += static_cast<double>(luminance(glm::vec3(src[i], src[i + 1], src[i + 2]))) * dw;
                bg_weight += dw;
            }
        }
    }
    const auto background = static_cast<float>(bg_weight > 0.0 ? bg_sum / bg_weight : 0.0);

    // 3) Dans le cône : on intègre l'EXCÈS au-dessus du ciel local (c'est le soleil), et
    //    on ramène le texel au niveau du ciel local en préservant sa teinte.
    glm::vec3 irradiance(0.0f);
    glm::vec3 dir_acc(0.0f);
    float dir_weight = 0.0f;
    for (int y = 0; y < h; ++y) {
        const float dw = texel_solid_angle(y, w, h);
        for (int x = 0; x < w; ++x) {
            const glm::vec3 d = equirect_direction(x, y, w, h);
            if (glm::dot(d, peak) <= cos_cone) {
                continue;
            }
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3u;
            glm::vec3 c(src[i], src[i + 1], src[i + 2]);
            const float l = luminance(c);
            if (l <= background || l <= 0.0f) {
                continue;
            }
            const float keep = background / l;  // ramène la luminance à celle du ciel
            const glm::vec3 excess = c * (1.0f - keep);
            irradiance += excess * dw;
            const float weight = luminance(excess) * dw;
            dir_acc += d * weight;
            dir_weight += weight;

            c *= keep;
            src[i] = c.r;
            src[i + 1] = c.g;
            src[i + 2] = c.b;
        }
    }

    // Barycentre pondéré par l'énergie : plus stable que le seul texel pic.
    out_direction = dir_weight > 0.0f ? glm::normalize(dir_acc) : peak;
    // sun_color est calibré pour qu'une surface blanche lambertienne face au soleil lise
    // ~1.0, ce qui correspond à une irradiance de PI * sun_color (cf. mesh_textured.frag).
    out_color = irradiance / kPi;
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

    // --- ORDRE CRITIQUE ---------------------------------------------------------
    // 1) Extraire le soleil ÉCRÊTE l'équirect sur place. 2) et 3) en dérivent tous
    // les deux, donc le soleil est retiré des SH *et* de la cubemap d'un seul geste.
    // Projeter les SH avant l'extraction ferait compter le soleil deux fois dans le
    // diffus, exactement comme le laisser dans la cubemap le doublerait en spéculaire.
    extract_sun(src, w, h, out.sun_direction, out.sun_color);

    // 2) Irradiance du ciel (soleil déjà parti) en SH9.
    for (int y = 0; y < h; ++y) {
        const float dw = texel_solid_angle(y, w, h);
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3u;
            const glm::vec3 radiance(src[i], src[i + 1], src[i + 2]);
            float basis[9];
            sh_basis(equirect_direction(x, y, w, h), basis);
            for (int k = 0; k < 9; ++k) {
                out.sh[static_cast<std::size_t>(k)] += radiance * (basis[k] * dw);
            }
        }
    }

    // 3) Rééchantillonnage en 6 faces.
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
    log::info("HDR : soleil extrait dir=({:.3f}, {:.3f}, {:.3f}) élévation={:.1f}° "
              "sun_color=({:.3f}, {:.3f}, {:.3f})",
              out.sun_direction.x, out.sun_direction.y, out.sun_direction.z,
              glm::degrees(std::asin(glm::clamp(out.sun_direction.y, -1.0f, 1.0f))),
              out.sun_color.r, out.sun_color.g, out.sun_color.b);
    log::info("HDR : SH9 L00=({:.3f}, {:.3f}, {:.3f})", out.sh[0].r, out.sh[0].g, out.sh[0].b);
    return true;
}

}  // namespace noire::resource
