#version 450

// PLUIE (M14) — triangle plein écran engendré depuis gl_VertexIndex (aucun tampon de
// géométrie, même procédé que la skybox et le HUD). Le fragment fait tout le travail.
//
// Ce pipeline n'a NI descriptor set NI UBO : tout ce dont le fragment a besoin
// (intensité, inclinaison, temps, aspect) arrive par un unique push constant. C'est un
// effet écran isolé — inutile de le coupler au bloc global std140.

layout(location = 0) out vec2 outUV;  // 0..1 sur l'écran

void main() {
    // (-1,-1) (3,-1) (-1,3) : couvre tout l'écran en un seul triangle.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    outUV = p;                       // 0..1 sur la zone visible
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
