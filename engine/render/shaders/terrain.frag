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

// Le grain des textures. 1 unité fragUV = uv_period = 8 m, donc la texture se répète tous
// les 8/kDetailRepeat mètres.
//
// Ce facteur valait 4 — soit une répétition tous les DEUX MÈTRES — au motif qu'« une herbe
// qui se répète tous les 8 m se lit comme un damier ». C'était l'exact contraire : à 2 m,
// l'oeil voit des dizaines de copies dans un seul regard et le damier saute aux yeux ; à
// 8 m il n'en voit que quelques-unes et le motif se dissout. Vérifié à l'image, le réseau
// de losanges à mi-distance disparaît complètement.
// `aerial_grass_rock` est d'ailleurs une texture AÉRIENNE, photographiée depuis le ciel :
// elle est faite pour couvrir plusieurs mètres, pas 2 m.
const float kDetailRepeat = 1.0;

// Échelles des bruits, exprimées en 1/(unités fragUV). Une cellule de bruit mesure
// 1/scale unités fragUV, et 1 unité fragUV = uv_period = 8 M : la longueur d'onde en
// mètres vaut donc 8/scale. (Les commentaires précédents lisaient 1/scale comme des
// mètres — ils sous-estimaient d'un facteur 8.)
// Deux octaves très écartées : la grande dessine les PLAQUES (champs, zones rocailleuses),
// la petite dentelle leurs bords pour qu'aucune frontière ne soit un trait net.
const float kPatchScale = 0.018;  // 8/0.018 = ~444 m : les grandes plaques
const float kEdgeScale = 0.30;    // 8/0.30  = ~27 m  : dentelle des bords

// --- MACRO-VARIATION -----------------------------------------------------------
// Le carrelage se VOIT à mi-distance, en réseau de losanges : `uv = fragUV * 4` et
// 1 fragUV = 8 m, donc la texture se répète tous les DEUX MÈTRES. Le tuilage stochastique
// casse cette grille de près, mais il s'efface avec la distance (ses poids crénellent, cf.
// common/stochastic.glsl) — et la grille réapparaît exactement là où il abdique.
//
// Le remède ici ne cherche PAS à cacher la maille de 2 m : à 200 m de longueur d'onde, on
// en est à cent fois trop grand pour ça. Il attaque l'autre moitié du problème — l'UNIFORMITÉ.
// Un « papier peint » se trahit autant par sa régularité locale que par le fait que TOUT
// se ressemble d'un bout à l'autre du champ. De vastes plages plus claires et plus sombres
// (variations géologiques de la craie, épaisseur d'herbe) rendent le champ hétérogène, et
// l'oeil cesse de chercher la grille.
//
// Deux octaves, toutes deux TRÈS basses fréquences : c'est ce qui garantit l'absence de
// grésillement. Un bruit dont la longueur d'onde vaut des centaines de mètres reste étalé
// sur des dizaines de pixels même à l'horizon — il ne peut PAS créneler, contrairement aux
// poids barycentriques du stochastique (cellules de 0,58 m) qui, eux, tombaient sous le
// pixel et grésillaient.
const float kMacroScaleA = 0.033;  // 8/0.033 = ~240 m
const float kMacroScaleB = 0.089;  // 8/0.089 = ~90 m
// Un bruit de gradient culmine à ±0.25 (mesuré, PAS ±0.5) : avec la 2e octave à 0.5, la
// somme couvre ±0.375, et ce gain la ramène à ±0.17 d'albédo. « Légèrement » : au-delà, le
// sol se met à ressembler à un ciel nuageux peint au sol, ce qui trahit autant qu'une grille.
const float kMacroStrength = 0.45;

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

    // Macro-variation, appliquée PARTOUT et non « seulement au loin ». La réserver à la
    // distance ferait apparaître puis disparaître les plages à mesure que le train avance :
    // un artefact MOBILE, donc bien pire que le défaut soigné. Une variation géologique ne
    // dépend pas de qui la regarde.
    float macro = gradientNoise(fragUV * kMacroScaleA) +
                  0.5 * gradientNoise(fragUV * kMacroScaleB);
    // MULTIPLICATIVE : c'est une variation de RÉFLECTANCE (plus ou moins d'herbe, craie plus
    // ou moins affleurante), pas une lumière ajoutée. Un additif délaverait les zones sombres
    // et casserait la cohérence avec l'éclairage.
    albedo *= max(1.0 + macro * kMacroStrength, 0.0);
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
