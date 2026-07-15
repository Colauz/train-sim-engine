#version 450

// Layout canonique : cf. mesh.vert. Le ciel n'utilise que `view` et `proj`, mais le bloc
// est déclaré à l'IDENTIQUE dans tous les shaders (source de vérité unique).
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;   // rgb = couleur brouillard, a = densité
    vec4 params;            // x = wetness
    vec4 sunDirection;      // xyz = direction VERS le soleil (normalisée)
    vec4 sunColor;          // rgb = couleur/intensité du soleil, a = intensité ambiante
    mat4 lightViewProj[2];  // une matrice par cascade d'ombre (kShadowCascades)
    vec4 cascadeSplits;     // x,y = fin de chaque cascade (distance en espace vue)
    // Irradiance du ciel en harmoniques sphériques d'ordre 2 (M8 étape 6b). vec4 et
    // NON vec3 : en std140 un tableau a un stride de 16 octets quoi qu'il arrive —
    // déclarer vec3[9] désaligne tout silencieusement. Seul .rgb porte l'information.
    vec4 sh[9];
} u;

layout(location = 0) out vec3 viewDir;

void main() {
    // Triangle plein écran SANS tampon de sommets : 3 sommets déduits de gl_VertexIndex,
    // soit (-1,-1), (3,-1), (-1,3). Il déborde du viewport, ce qui est voulu : un seul
    // triangle évite la couture diagonale d'un quad et coûte moins qu'un cube.
    vec2 ndc = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;

    // REVERSE-Z (M9) : le plan lointain est à z = 0, plus à 1. Combiné au test
    // GREATER_OR_EQUAL et au clear à 0.0, le ciel ne survit que là où rien n'a été
    // dessiné : la géométrie déjà rasterisée le rejette en early-z.
    gl_Position = vec4(ndc, 0.0, 1.0);

    // Rayon de vue : on remonte le NDC à travers la projection. Fait ICI (3 invocations
    // par frame) et surtout pas au fragment, où inverse() sur une mat4 se paierait par
    // pixel. On divise par w : en reverse-Z le facteur d'échelle n'est plus garanti
    // positif, et un signe inversé retournerait le ciel — la division rend le calcul
    // indépendant de la convention de profondeur.
    vec4 viewPos = inverse(u.proj) * vec4(ndc, 0.0, 1.0);
    viewPos /= viewPos.w;

    // La vue est une PURE ROTATION (origine flottante : la caméra EST à l'origine), donc
    // son inverse est sa transposée. Le résultat vit dans l'espace relatif caméra, qui
    // n'est qu'une translation du monde : ses axes sont donc les axes du monde, ce qui
    // permet d'indexer la cubemap directement.
    viewDir = transpose(mat3(u.view)) * viewPos.xyz;
}
