#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/pbr.glsl"
#include "common/stochastic.glsl"

// set = 1 : DEUX jeux PBR complets. C'est la seule raison d'être d'un pipeline terrain
// distinct : le set 1 des matériaux ordinaires n'a que 3 bindings.
// Convention Poly Haven : _arm = AO / Roughness / Metallic en R/G/B, soit EXACTEMENT la
// convention glTF metallic-roughness (G = rough, B = metal). Aucun repack.
layout(set = 1, binding = 0) uniform sampler2D grassBase;
layout(set = 1, binding = 1) uniform sampler2D grassArm;
layout(set = 1, binding = 2) uniform sampler2D grassNormal;
layout(set = 1, binding = 3) uniform sampler2D chalkBase;
layout(set = 1, binding = 4) uniform sampler2D chalkArm;
layout(set = 1, binding = 5) uniform sampler2D chalkNormal;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbrFactors;
} object;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 cameraRelPos;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;

layout(location = 0) out vec4 outColor;

// Le grain des textures : 1 unité UV = 8 m côté clipmap, mais une herbe qui se répète
// tous les 8 m se lit comme un damier. On la répète plus vite (petites touffes) et on
// laisse le BRUIT, à bien plus grande échelle, casser la régularité.
const float kDetailRepeat = 4.0;

// Échelles du bruit de transition, en unités UV (donc x8 pour des mètres). Deux octaves
// très écartées : la grande dessine les PLAQUES (champs, zones rocailleuses), la petite
// dentelle leurs bords pour qu'aucune frontière ne soit un trait net.
const float kPatchScale = 0.018;  // ~ 7 m d'UV => plaques de ~55 m
const float kEdgeScale = 0.30;    // ~ 3 m : dentelle des bords

// Teinte de l'herbe. Nécessaire, et pas cosmétique : TOUTES les textures de sol de Poly
// Haven sont photographiées en lumière naturelle et tirent vers l'ocre désaturé —
// `aerial_grass_rock` a une teinte de 48° (de l'ocre), pas du vert (mesuré). Sans ce
// facteur, la Champagne est en permanence grillée. La craie, elle, est déjà pâle et
// n'est pas teintée : son sol EST blanc, c'est ce qui a donné son nom à la région.
const vec3 kGrassTint = vec3(0.62, 1.12, 0.45);

// Gradient d'une cellule, pour le bruit de transition. Réutilise le hash entier de
// stochastic.glsl, recentré sur [-1, 1].
vec2 hash2(vec2 p) {
    return hashCell(ivec2(floor(p))) * 2.0 - 1.0;
}

float gradientNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * (3.0 - 2.0 * f);  // lissage cubique : pas d'arête entre cellules
    return mix(mix(dot(hash2(i + vec2(0, 0)), f - vec2(0, 0)),
                   dot(hash2(i + vec2(1, 0)), f - vec2(1, 0)), w.x),
               mix(dot(hash2(i + vec2(0, 1)), f - vec2(0, 1)),
                   dot(hash2(i + vec2(1, 1)), f - vec2(1, 1)), w.x), w.y);
}

void main() {
    vec2 uv = fragUV * kDetailRepeat;

    // --- Le masque de splatting -------------------------------------------------
    // La PENTE seule ne suffit pas : notre relief plafonne à ~14°, donc un seuil sur la
    // pente ne déclencherait la craie que sur les talus de la voie et nulle part ailleurs
    // (mesuré au M11 phase 1). C'est le BRUIT qui porte l'essentiel du mélange, la pente
    // n'étant qu'un biais qui garantit que les flancs raides se dénudent bien.
    float slope = 1.0 - clamp(fragNormal.y, 0.0, 1.0);
    float patches = gradientNoise(fragUV * kPatchScale);
    float edge = gradientNoise(fragUV * kEdgeScale);

    // Un bruit de gradient culmine à ±0.25, PAS ±0.5 (mesuré) : les gains sont calés
    // là-dessus, sinon le masque n'atteint jamais ses bornes et le mélange reste coincé
    // à mi-chemin — deux textures superposées en permanence, ce qui ne ressemble à rien.
    // slope*8 : 0..0.03 (0..14°) -> 0..0.24, assez pour dénuder les talus sans dominer.
    float mask = 0.5 + slope * 8.0 + patches * 1.9 + edge * 0.45;
    // smoothstep large : une transition franche trahirait immédiatement le procédé.
    float chalk = smoothstep(0.25, 0.75, mask);

    // --- Les deux jeux PBR, en TUILAGE STOCHASTIQUE, puis mélangés -----------------
    // 3 taps x 3 cartes x 2 couches = 18 lectures au pire. On saute donc la couche dont
    // le poids est nul — branche spatialement COHÉRENTE (le masque varie lentement), et
    // légale parce que stochasticSurface() utilise textureGrad : ses dérivées sont
    // explicites, là où un texture() en flot non uniforme serait indéfini.
    vec3 gBase = vec3(0.0), gArm = vec3(0.5), gNor = vec3(0.5, 0.5, 1.0);
    vec3 cBase = vec3(0.0), cArm = vec3(0.5), cNor = vec3(0.5, 0.5, 1.0);
    if (chalk < 0.999) {
        stochasticSurface(grassBase, grassArm, grassNormal, uv, gBase, gArm, gNor);
    }
    if (chalk > 0.001) {
        stochasticSurface(chalkBase, chalkArm, chalkNormal, uv, cBase, cArm, cNor);
    }

    vec3 albedo = mix(gBase * kGrassTint, cBase, chalk);
    // La RUGOSITÉ est interpolée elle aussi : c'est ce qui fait que le soleil accroche
    // différemment l'herbe (mate) et la craie (plus minérale). Mélanger l'albédo seul
    // donnerait deux couleurs sur un même matériau — l'oeil le voit tout de suite.
    float roughness = clamp(mix(gArm.g, cArm.g, chalk), kMinRoughness, 1.0);
    float metallic = 0.0;  // herbe et craie sont deux diélectriques

    // Les NORMALES aussi : on mélange les texels bruts puis on renormalise. Interpoler
    // dans l'espace tangent avant décompression suffit à cette échelle, et évite de
    // construire deux TBN.
    vec3 nMap = normalize(mix(gNor, cNor, chalk) * 2.0 - 1.0) * 0.5 + 0.5;
    vec3 N = shadingNormal(fragNormal, fragTangent, nMap, 1.0);

    outColor = vec4(shadeSurface(albedo, metallic, roughness, N, cameraRelPos), 1.0);
}
