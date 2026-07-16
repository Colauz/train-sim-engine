#version 450

// HUD (M13) — le glyphe est un MASQUE DE BITS, pas une texture.
//
// On retrouve le texel de police sous ce pixel, puis on teste SON bit. Comme le
// Renderer force une échelle ENTIÈRE en pixels par texel, chaque texel couvre un bloc
// exact de NxN pixels : ce floor() rend donc rigoureusement ce que rendrait un
// échantillonnage `nearest` sur un atlas — d'où un HUD parfaitement net, sans texture,
// sans sampler, sans descriptor set.

layout(location = 0) in vec2 vCell;   // 0..5 x 0..7 dans la grille du glyphe
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in uvec2 vBits;  // entier => flat OBLIGATOIRE

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 cell = ivec2(floor(vCell));
    if (cell.x < 0 || cell.x > 4 || cell.y < 0 || cell.y > 6) {
        discard;  // garde-fou : le quad ne devrait jamais déborder de sa grille
    }

    // Bit i = texel (i % 5, i / 5) : miroir exact de build_table() (hud_font.hpp).
    uint index = uint(cell.y * 5 + cell.x);  // 0..34
    uint word = (index < 32u) ? vBits.x : vBits.y;
    uint bit = (index < 32u) ? index : index - 32u;
    if (((word >> bit) & 1u) == 0u) {
        discard;
    }

    // Couleur LINÉAIRE : l'attachement est _SRGB, donc le matériel décode avant de
    // mélanger et réencode après. Le blend a lieu en linéaire — c'est CORRECT, et c'est
    // écrit ici pour que personne ne vienne le « corriger ».
    outColor = vColor;
}
