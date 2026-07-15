#version 450

// UBO global (set 0, partagé avec le pipeline debug). Layout canonique : cf. mesh.vert.
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;   // rgb = couleur brouillard, a = densité
    vec4 params;            // x = wetness
    vec4 sunDirection;      // xyz = direction VERS le soleil (normalisée)
    vec4 sunColor;          // rgb = couleur/intensité du soleil, a = intensité ambiante
    mat4 lightViewProj[2];  // une matrice par cascade d'ombre (kShadowCascades)
    vec4 cascadeSplits;     // x,y = fin de chaque cascade (distance en espace vue)
} u;

// Miroir exact de TexturedPushConstants (renderer.cpp). Déclaré à l'identique dans le
// fragment : le vertex n'utilise que `model`, mais la portée couvre les deux étages.
layout(push_constant) uniform PushConstants {
    mat4 model;             // déjà relatif à la caméra (origine flottante)
    vec4 baseColorFactor;
    vec4 pbrFactors;        // x = metallic, y = roughness, z = normalScale
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;  // xyz = tangente, w = handedness (glTF)

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 cameraRelPos;  // position relative caméra (fog)
layout(location = 2) out vec3 fragNormal;    // normale monde (éclairage)
layout(location = 3) out vec4 fragTangent;   // tangente monde + handedness préservé

void main() {
    vec4 rel = object.model * vec4(inPosition, 1.0);
    cameraRelPos = rel.xyz;
    // Normale et tangente transformées par la partie rotation du modèle (pas d'échelle
    // non uniforme ici). Le handedning (w) ne subit AUCUNE transformation.
    fragNormal = normalize(mat3(object.model) * inNormal);
    fragTangent = vec4(normalize(mat3(object.model) * inTangent.xyz), inTangent.w);
    fragUV = inUV;
    gl_Position = u.proj * u.view * rel;
}
