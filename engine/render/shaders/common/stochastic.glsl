// Tuilage stochastique — casse la répétition d'une texture carrelée.
//
// Principe (Heitz & Neyret 2018, avec le mélange aiguisé de Mikkelsen) : au lieu de lire
// la texture une fois, on la lit TROIS fois à des positions et orientations aléatoires,
// puis on mélange les trois selon une grille TRIANGULAIRE. Chaque point du sol tombe dans
// un triangle et reçoit les 3 échantillons de ses sommets, pondérés par ses coordonnées
// barycentriques. La texture ne se répète donc jamais à l'identique : c'est un pavage
// apériodique.
//
// TROIS pièges, tous mortels si on les rate :
//   1. LES DÉRIVÉES. Les UV échantillonnés SAUTENT d'une cellule à l'autre. Un
//      `texture()` en déduirait des dérivées gigantesques, croirait à une compression
//      énorme et choisirait le dernier mip : le sol partirait en bouillie floue. D'où
//      `textureGrad` avec les dérivées de l'UV D'ORIGINE, transformées par la même
//      rotation que le tap.
//   2. LA VARIANCE. Mélanger linéairement 3 échantillons d'une texture MOYENNE son
//      contraste — le résultat vire au gris uniforme, et on a remplacé un damier par une
//      bouillie. D'où les poids AIGUISÉS : un seul échantillon domine presque partout.
//   3. LES NORMALES. Une normal map lue à travers une rotation stocke ses gradients dans
//      le repère TOURNÉ. Sans rotation inverse, l'éclairage part dans une direction
//      arbitraire par cellule — et l'IBL, qui dépend entièrement de N, sanctionne
//      immédiatement.
//
// Bonus : `textureGrad` est légal en flot de contrôle NON uniforme (contrairement à
// `texture()`, dont les dérivées implicites y sont indéfinies). C'est ce qui autorise
// terrain.frag à sauter les taps d'une couche dont le poids est nul.
#ifndef NOIRE_STOCHASTIC_GLSL
#define NOIRE_STOCHASTIC_GLSL

// Aiguisage des poids. 1 = mélange linéaire (contraste détruit) ; +inf = un seul
// échantillon (coutures franches). 5 laisse une transition étroite mais continue.
const float kStochasticSharpen = 5.0;
// Amplitude du décalage aléatoire, en tours de texture. Grand et non entier : il doit
// décorréler complètement les 3 taps.
const float kStochasticOffset = 43.0;

// Grille triangulaire (Heitz) : rend les 3 sommets voisins et les barycentriques.
// L'inclinaison ramène une grille carrée sur une grille de triangles équilatéraux —
// c'est ce qui évite qu'un pavage carré ne réintroduise sa propre régularité.
void triangleGrid(vec2 uv, out vec3 w, out ivec2 v1, out ivec2 v2, out ivec2 v3) {
    uv *= 3.4641016;  // 2*sqrt(3) : maille ~1 unité UV
    const mat2 toSkewed = mat2(1.0, 0.0, -0.57735027, 1.15470054);
    vec2 skewed = toSkewed * uv;
    ivec2 base = ivec2(floor(skewed));
    vec2 f = fract(skewed);
    float z = 1.0 - f.x - f.y;
    if (z > 0.0) {  // triangle « bas »
        w = vec3(z, f.y, f.x);
        v1 = base;
        v2 = base + ivec2(0, 1);
        v3 = base + ivec2(1, 0);
    } else {  // triangle « haut »
        w = vec3(-z, 1.0 - f.y, 1.0 - f.x);
        v1 = base + ivec2(1, 1);
        v2 = base + ivec2(1, 0);
        v3 = base + ivec2(0, 1);
    }
}

// Hash entier d'une cellule -> [0,1)². ENTIER, pas de sin() : nos UV montent à des
// milliers, où un hash trigonométrique perd toute précision et se met à répéter — ce qui
// réintroduirait exactement le motif qu'on cherche à détruire.
vec2 hashCell(ivec2 c) {
    uvec2 q = uvec2(c) * uvec2(1597334673u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334673u;
    n ^= n >> 15;  // avalanche : sans elle, les cellules voisines restent corrélées
    n *= 2246822519u;
    n ^= n >> 13;
    return vec2(n & 0xffffu, (n >> 16) & 0xffffu) / 65535.0;
}

mat2 rot2(float a) {
    float c = cos(a);
    float s = sin(a);
    return mat2(c, -s, s, c);
}

// Échantillonne un jeu PBR complet (base + ARM + normale) en tuilage stochastique.
// Les TROIS cartes partagent les mêmes taps : sinon l'albédo, la rugosité et le relief
// décriraient trois cailloux différents au même endroit.
void stochasticSurface(sampler2D baseMap, sampler2D armMap, sampler2D normalMap, vec2 uv,
                       out vec3 outBase, out vec3 outArm, out vec3 outNormal) {
    vec3 w;
    ivec2 c1, c2, c3;
    triangleGrid(uv, w, c1, c2, c3);

    w = pow(w, vec3(kStochasticSharpen));
    w /= (w.x + w.y + w.z);

    // Dérivées de l'UV CONTINU : c'est la seule référence correcte pour le choix du mip.
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);

    ivec2 cells[3] = ivec2[3](c1, c2, c3);
    float weights[3] = float[3](w.x, w.y, w.z);

    outBase = vec3(0.0);
    outArm = vec3(0.0);
    vec3 nAcc = vec3(0.0);

    for (int i = 0; i < 3; ++i) {
        vec2 h = hashCell(cells[i]);
        mat2 R = rot2(h.x * 6.2831853);
        vec2 suv = R * uv + h * kStochasticOffset;
        // Les dérivées subissent la MÊME transformation que l'UV : d(R*uv)/dx = R*(duv/dx).
        vec2 sdx = R * dx;
        vec2 sdy = R * dy;

        outBase += weights[i] * textureGrad(baseMap, suv, sdx, sdy).rgb;
        outArm += weights[i] * textureGrad(armMap, suv, sdx, sdy).rgb;

        // La normale est décompressée AVANT le mélange : moyenner des octets encodés
        // n'a pas de sens géométrique. Puis rotation INVERSE — le tap a lu la texture à
        // travers R, donc ses gradients sont exprimés dans le repère tourné ; transpose(R)
        // les ramène dans le repère tangent de la surface.
        vec3 n = textureGrad(normalMap, suv, sdx, sdy).rgb * 2.0 - 1.0;
        n.xy = transpose(R) * n.xy;
        nAcc += weights[i] * n;
    }

    // Re-normalisée puis RÉ-ENCODÉE : shadingNormal() attend un texel brut (0..1).
    outNormal = normalize(nAcc) * 0.5 + 0.5;
}

#endif  // NOIRE_STOCHASTIC_GLSL
