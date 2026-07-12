#version 450

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 proj;
    vec4 fogColorDensity;  // rgb = couleur brouillard, a = densité
    vec4 params;           // x = wetness
} u;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 cameraRelPos;

layout(location = 0) out vec4 outColor;

void main() {
    float wetness = u.params.x;

    // Mouillé : plus sombre et légèrement bleuté (les rails perdent en luminosité).
    vec3 base = fragColor * (1.0 - 0.45 * wetness) + vec3(0.02, 0.03, 0.05) * wetness;

    // Brouillard de distance exponentiel (la distance = norme de la position relative caméra).
    float dist = length(cameraRelPos);
    float fog = clamp(1.0 - exp(-u.fogColorDensity.a * dist), 0.0, 1.0);

    vec3 color = mix(base, u.fogColorDensity.rgb, fog);
    outColor = vec4(color, 1.0);
}
