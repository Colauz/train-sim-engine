#version 450

// PLUIE (M14) — stries procédurales en espace écran, sans texture ni particule.
//
// Le principe : l'écran est découpé en COLONNES ; dans chaque colonne défilent des gouttes
// (segments verticaux courts) dont la phase dépend d'un hash de la colonne. Le tout est
// CISAILLÉ horizontalement par `tilt` : à l'arrêt la pluie tombe droite, à 320 km/h elle
// raye l'écran presque à l'horizontale, dans le sens où le paysage file. Quelques couches
// à des profondeurs différentes donnent le volume. C'est le rendu de pluie le moins cher
// qui soit — pas de depth, pas de fetch, un plein écran alpha-blendé.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    float intensity;  // 0..1 (le wetness) : densité ET opacité
    float tilt;       // cisaillement horizontal (repère vue) — croît avec la vitesse
    float time;       // secondes (défilement)
    float aspect;     // largeur/hauteur, pour des colonnes carrées
} pc;

float hash(vec2 p) {
    p = fract(p * vec2(443.8975, 397.2973));
    p += dot(p, p.yx + 19.19);
    return fract(p.x * p.y);
}

// Une couche de pluie. `cols` = nombre de colonnes ; `speed` = vitesse de chute ;
// `len` = longueur des stries ; `seed` décorrèle les couches.
float layer(vec2 uv, float cols, float speed, float len, float seed) {
    // Cisaillement : la strie penche d'autant plus que le train va vite.
    uv.x += uv.y * pc.tilt;

    vec2 g = vec2(uv.x * pc.aspect, uv.y) * cols;
    float col = floor(g.x);
    float ch = hash(vec2(col, seed));

    // Défilement vertical, vitesse légèrement variable par colonne (pluie pas métronomique).
    float y = g.y + pc.time * speed * (0.75 + 0.5 * ch) + ch * 13.0;
    float row = floor(y);
    float rh = hash(vec2(col, row + seed * 7.0));

    // ~35 % des cellules portent une goutte.
    float present = step(0.65, rh);

    // Strie : brillante en tête, s'estompe vers le bas (traînée de la goutte).
    float fy = fract(y);
    float streak = smoothstep(len, 0.0, fy) * present;

    // Finesse horizontale : un trait mince centré dans la colonne, jitté par colonne.
    float cx = fract(g.x) - 0.5;
    float jitter = (ch - 0.5) * 0.5;
    float thin = smoothstep(0.10, 0.0, abs(cx - jitter));

    return streak * thin;
}

void main() {
    if (pc.intensity <= 0.001) {
        discard;  // temps sec : rien à payer
    }

    // Deux couches à des échelles différentes = un premier plan net + un fond plus dense.
    float r = 0.0;
    r += layer(inUV, 22.0, 2.2, 0.55, 1.0) * 0.9;   // gouttes proches, longues
    r += layer(inUV, 40.0, 3.1, 0.40, 2.0) * 0.55;  // fond plus fin, plus rapide

    // La densité affichée monte avec l'intensité (et sature vite : sous l'averse, l'écran
    // est bien rayé mais on voit encore la voie).
    float alpha = clamp(r * pc.intensity, 0.0, 1.0) * 0.55;

    // Gris bleuté, LINÉAIRE (la swapchain est SRGB, encodage matériel).
    outColor = vec4(vec3(0.62, 0.66, 0.72), alpha);
}
