#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// POLICE DU HUD (M13) — 5x7, embarquée en MASQUE DE BITS, sans texture.
//
// LE FAIT QUI COMMANDE TOUT : un HUD de cabine affiche trois lignes de chiffres. Un
// atlas de police coûterait une image, un sampler, un descriptor set, et surtout le
// téléversement ASYNCHRONE qui va avec (is_texture_ready) — donc un HUD qui n'existe
// pas pendant les premières frames, précisément celles de l'écran de chargement.
//
// LA SOLUTION : un glyphe 5x7 tient sur 35 bits. On les fait voyager dans les DONNÉES
// D'INSTANCE (uvec2), et le fragment teste le bit du texel sous le pixel. C'est
// l'équivalent exact d'un échantillonnage `nearest` sur un atlas — mais sans atlas, sans
// sampler, sans set, et dessinable dès la première frame.
//
// Le prix : la police est en CAPITALES seulement (43 glyphes saisis à la main). Pour un
// afficheur de cabine, ce n'est pas une limite — c'est le registre juste.
namespace noire::render::font {

inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;
inline constexpr int kGlyphBits = kGlyphWidth * kGlyphHeight;  // 35

// Plage couverte : espace (32) .. Z (90). Au-delà, rien n'est dessiné.
inline constexpr unsigned char kFontFirst = 32;
inline constexpr unsigned char kFontLast = 90;
inline constexpr std::size_t kFontTableSize = kFontLast - kFontFirst + 1;  // 59

// Glyphe PLEIN (35 bits à 1) : c'est lui qui dessine les plaques de fond du HUD. Une
// plaque emprunte ainsi le chemin d'instance des glyphes sans un seul branchement de
// plus dans le shader — elle n'est qu'un glyphe dont tous les texels sont allumés.
inline constexpr std::uint64_t kGlyphBlock = (std::uint64_t{1} << kGlyphBits) - 1;

namespace detail {

struct GlyphArt {
    char code;
    const char* rows[kGlyphHeight];
};

// Saisis en art ASCII : c'est la seule forme sous laquelle une police se relit et se
// corrige. L'empaquetage en bits se fait à la compilation (build_table), donc la
// lisibilité ici ne coûte rien au runtime.
inline constexpr GlyphArt kArt[] = {
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
    {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
    {'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},

    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}},
    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ####"}},
    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
    {'N', {"#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},

    {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
    {',', {"     ", "     ", "     ", "     ", " ##  ", " ##  ", "  #  "}},
    {':', {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}},
    {'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
    {'+', {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}},
    {'/', {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}},
    {'%', {"##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"}},
};

// Bit i = texel (i % 5, i / 5), origine en haut à gauche. Le fragment refait exactement
// ce calcul : toute divergence ici se verrait comme une police transposée.
constexpr std::array<std::uint64_t, kFontTableSize> build_table() {
    std::array<std::uint64_t, kFontTableSize> table{};
    for (const GlyphArt& art : kArt) {
        std::uint64_t bits = 0;
        for (int row = 0; row < kGlyphHeight; ++row) {
            for (int col = 0; col < kGlyphWidth; ++col) {
                if (art.rows[row][col] != ' ') {
                    bits |= std::uint64_t{1} << (row * kGlyphWidth + col);
                }
            }
        }
        table[static_cast<std::size_t>(art.code) - kFontFirst] = bits;
    }
    return table;
}

inline constexpr std::array<std::uint64_t, kFontTableSize> kTable = build_table();

}  // namespace detail

// Masque 35 bits d'un caractère ; 0 (donc rien de dessiné) hors de la plage couverte.
//
// LE CAST EN unsigned char N'EST PAS COSMÉTIQUE : `char` est signé sur x86, et ce dépôt
// est saturé de français accenté. Le premier octet d'un « é » en UTF-8 vaut 0xC3, soit
// -61 en signé : `c - 32` donnerait -93, un index NÉGATIF dans la table. Un HUD qui
// afficherait un jour « PATINÉ » lirait alors hors du tableau.
[[nodiscard]] inline std::uint64_t glyph_bits(char c) {
    const auto uc = static_cast<unsigned char>(c);
    // Police en capitales : on replie les minuscules, on ne les dessine pas en creux.
    const unsigned char up =
        (uc >= 'a' && uc <= 'z') ? static_cast<unsigned char>(uc - 32u) : uc;
    if (up < kFontFirst || up > kFontLast) {
        return 0;
    }
    return detail::kTable[static_cast<std::size_t>(up - kFontFirst)];
}

}  // namespace noire::render::font
