// CYCLE JOUR / NUIT — le gain appliqué au CIEL et à l'AMBIANTE.
//
// Il vit ici, dans un fichier partagé, pour la même raison que le coeur PBR : le ciel
// qu'on VOIT (skybox.frag) et le ciel qui ÉCLAIRE (pbr.glsl : SH + environnement
// préfiltré + brouillard) sont deux lectures de la même cubemap, et cette cubemap est
// STATIQUE — c'est un HDRI de plein jour, capturé une fois. Seule cette modulation la
// fait vivre. La dupliquer des deux côtés, c'est garantir qu'ils divergeront, et la
// divergence a un nom précis ici : c'est le défaut que ce fichier corrige.
//
// LE DÉFAUT (M56 et avant). skybox.frag assombrissait déjà son échantillon avec le
// facteur nuit ; pbr.glsl, lui, n'assombrissait RIEN. À 2 h du matin, le ciel était donc
// noir — et les quais, la verrière, le viaduc, les rails restaient éclairés par un ciel
// de midi qui n'existait plus à l'écran. Mesuré au pixel sur le banc épinglé : un quai
// rendait (114, 131, 150) à 13 h et (114, 131, 150) à 2 h. Rigoureusement identique. Le
// cycle jour/nuit du M21 était, sur toute la géométrie texturée, purement décoratif.
//
// DEUX gains, et non un seul, parce que ce sont deux grandeurs différentes :
//
//   * skyRadianceGain — ce qu'on VOIT en levant les yeux, et donc aussi la couleur du
//     brouillard (le brouillard EST le ciel vu à travers la brume). La nuit, un ciel
//     urbain n'est pas noir : il est d'un bleu très sombre, teinté par le halo de la
//     ville. 2 % suffisent à le distinguer du vide sans jamais concurrencer les néons.
//
//   * ambientIrradianceGain — ce qui ÉCLAIRE les surfaces. Il ne tombe PAS aussi bas, et
//     ce n'est pas une licence : dans une ville de cette densité, la lumière ambiante
//     nocturne ne vient pas du ciel, elle vient de la VILLE — façades allumées, enseignes,
//     éclairage public, tout cela renvoyé par le bitume et la brume. C'est précisément
//     pourquoi on ne voit pas les étoiles à Tokyo. 12 % du jour, poussés vers le froid :
//     un quai reste lisible (on y conduit) sans jamais avoir l'air d'être en plein jour.
//
// Les deux passent par la même racine carrée que le crépuscule d'origine : elle ralentit
// le début de la transition, ce qui donne un coucher progressif plutôt qu'un interrupteur.
#ifndef NOIRE_SKY_GLSL
#define NOIRE_SKY_GLSL

// Radiance résiduelle du ciel en pleine nuit, en fraction du ciel de jour.
const vec3 kNightSkyRadiance = vec3(0.018, 0.024, 0.048);
// Irradiance ambiante résiduelle en pleine nuit — le halo urbain, pas le ciel.
const vec3 kNightAmbient = vec3(0.26, 0.28, 0.36);

// `nightFactor` = u.skyParams.x : 0 en plein jour, 1 quand le soleil est sous l'horizon.
vec3 skyRadianceGain(float nightFactor) {
    return mix(vec3(1.0), kNightSkyRadiance, sqrt(clamp(nightFactor, 0.0, 1.0)));
}

vec3 ambientIrradianceGain(float nightFactor) {
    return mix(vec3(1.0), kNightAmbient, sqrt(clamp(nightFactor, 0.0, 1.0)));
}

#endif  // NOIRE_SKY_GLSL
