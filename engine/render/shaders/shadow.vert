#version 450

// Passe depth-only du soleil (M8 étape 1) : aucun fragment shader, seule la
// profondeur est écrite. La matrice poussée est déjà `lightViewProj * model`,
// donc ce shader sert aux DEUX formats de sommet (Vertex et MeshVertex) : la
// position est à l'offset 0 dans les deux, seul le stride du binding diffère.
layout(push_constant) uniform PushConstants {
    mat4 lightMvp;  // lightViewProj * model (model déjà relatif à la caméra)
} object;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = object.lightMvp * vec4(inPosition, 1.0);
}
