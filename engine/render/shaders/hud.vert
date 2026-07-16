#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/global_ubo.glsl"

// HUD (M13) — UN glyphe = UNE instance, tout le HUD = UN vkCmdDraw.
//
// AUCUN TAMPON DE GÉOMÉTRIE : les 6 sommets du quad sont engendrés depuis gl_VertexIndex
// (même procédé que la skybox). Le seul binding est celui des INSTANCES.
//
// AUCUN PUSH CONSTANT : la taille du viewport en pixels est déjà dans l'UBO global
// (u.params.zw), renseignée par le Renderer pour wire.vert — le seul à connaître
// l'étendue réelle de la swapchain. On la relit telle quelle : elle est rafraîchie à
// chaque frame, donc le HUD suit le redimensionnement sans une ligne de code.

// binding 0, VK_VERTEX_INPUT_RATE_INSTANCE : un jeu par GLYPHE.
layout(location = 0) in vec2 inPosition;  // px, coin haut-gauche
layout(location = 1) in vec2 inSize;      // px
layout(location = 2) in uvec2 inBits;     // masque 5x7 (35 bits utiles sur 64)
layout(location = 3) in vec4 inColor;     // LINÉAIRE (la swapchain est SRGB)

layout(location = 0) out vec2 vCell;   // position dans la grille du glyphe : 0..5 x 0..7
layout(location = 1) out vec4 vColor;
// flat : un masque de bits n'a AUCUN sens interpolé. C'est ici qu'il se déclare — pas
// sur l'entrée de sommet, où le qualificateur serait illégal.
layout(location = 2) flat out uvec2 vBits;

void main() {
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 corner = corners[gl_VertexIndex];

    // Pixels -> NDC. Origine HAUT-GAUCHE, ce qui tombe juste : le y de Vulkan descend
    // déjà, donc aucun retournement n'est nécessaire (et c'est le genre de chose qu'on
    // finit par inverser deux fois si on ne l'écrit pas).
    vec2 px = inPosition + corner * inSize;
    vec2 ndc = px / u.params.zw * 2.0 - 1.0;

    // z = 0 sans importance : le test de profondeur est désactivé pour ce pipeline.
    gl_Position = vec4(ndc, 0.0, 1.0);

    vCell = corner * vec2(5.0, 7.0);
    vColor = inColor;
    vBits = inBits;
}
