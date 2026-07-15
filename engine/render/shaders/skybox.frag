#version 450

// set = 1 : la cubemap HDR d'environnement (R16G16B16A16_SFLOAT + chaîne de mips).
// Le mip 0 est le ciel net ; les mips suivants serviront de niveaux de rugosité à
// l'étape 6b (IBL spéculaire). Ici on ne lit que le mip 0.
layout(set = 1, binding = 0) uniform samplerCube envMap;

layout(location = 0) in vec3 viewDir;

layout(location = 0) out vec4 outColor;

// ACES filmique (Narkowicz) — DOIT rester identique à mesh_textured.frag : le ciel et la
// géométrie sont deux moitiés de la même image. Un opérateur différent de chaque côté
// ferait apparaître une marche nette à l'horizon.
vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // textureLod explicite : le triangle plein écran n'a pas de dérivées d'UV utiles,
    // et on veut de toute façon le niveau le plus net.
    vec3 sky = textureLod(envMap, normalize(viewDir), 0.0).rgb;

    // Le contenu de la cubemap est de la RADIANCE HDR brute (valeurs > 1 courantes, le
    // disque solaire monte à plusieurs milliers) : sans tone mapping tout le ciel
    // saturerait en blanc pur.
    // Aucune correction gamma manuelle : la swapchain est en VK_FORMAT_B8G8R8A8_SRGB,
    // le matériel encode à l'écriture.
    outColor = vec4(acesFilm(sky), 1.0);
}
