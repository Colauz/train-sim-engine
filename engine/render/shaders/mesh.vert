#version 450

// UBO : matrices caméra (view + proj), identiques pour toute la frame.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

// Push constant : matrice Model de l'objet (déjà relative à la caméra, en float).
layout(push_constant) uniform PushConstants {
    mat4 model;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = camera.proj * camera.view * object.model * vec4(inPosition, 1.0);
    fragColor = inColor;
}
