#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/pbr.glsl"

// Fragment d'un câble. Le ruban est PLAT ; on lui rend sa rondeur en reconstruisant une
// normale de cylindre à partir de la coordonnée transverse. Sans ça, un fil vu de près
// serait un ruban de scotch : uniformément éclairé, sans le filet de lumière qui court le
// long d'un câble tendu et qui est, à vrai dire, tout ce qu'on en voit.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbrFactors;  // x = metallic, y = roughness
} object;

layout(location = 0) in vec3 cameraRelPos;
layout(location = 1) in vec3 wireSide;
layout(location = 2) in vec3 wireTangent;
layout(location = 3) in float across;    // -1 au bord gauche, +1 au bord droit
layout(location = 4) in float coverage;  // fraction de pixel couverte (cf. wire.vert)

layout(location = 0) out vec4 outColor;

void main() {
    // Normale de cylindre : à `across` du centre, la surface d'un tube s'incline de
    // asin(across) hors du plan du ruban. On la reconstruit par Pythagore, sans trigo.
    float a = clamp(across, -1.0, 1.0);
    vec3 V = normalize(-cameraRelPos);
    vec3 N = normalize(wireSide * a + V * sqrt(max(1.0 - a * a, 0.0)));
    // Orthogonalisation contre la tangente : un cylindre n'a aucune courbure le long de son
    // axe, donc sa normale y est rigoureusement perpendiculaire.
    N = normalize(N - wireTangent * dot(N, wireTangent));

    float metallic = clamp(object.pbrFactors.x, 0.0, 1.0);
    float roughness = clamp(object.pbrFactors.y, kMinRoughness, 1.0);
    vec3 color = shadeSurface(object.baseColorFactor.rgb, metallic, roughness, N, cameraRelPos);

    // LA couverture. Elle sort du vertex shader, où l'on sait de combien on a élargi le
    // ruban au-delà de sa taille vraie ; on rend ici en opacité ce qu'on a volé en largeur.
    // C'est l'intégrale analytique que ferait un supersampling — à ceci près qu'elle ne
    // coûte rien et qu'elle ne peut pas, elle, manquer le fil entre deux échantillons.
    outColor = vec4(color, coverage * object.baseColorFactor.a);
}
