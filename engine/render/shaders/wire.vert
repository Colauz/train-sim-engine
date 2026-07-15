#version 450

#extension GL_GOOGLE_include_directive : require
#include "common/global_ubo.glsl"

// RENDU DES CÂBLES SOUS-PIXEL (M12).
//
// LE FAIT QUI COMMANDE TOUT : un fil de contact fait 15 mm. À notre fov (60°) sur 720 px,
// un pixel vaut 1,604 mrad, donc le fil ne mesure UN pixel qu'à 9,4 m. À 50 m il en fait
// 0,19 ; à 3 km, 0,0031 — un TROIS-CENTIÈME de pixel. Autrement dit, passé le nez du train,
// la caténaire est intégralement sous-pixel.
//
// POURQUOI PAS DES TUBES 3D : leur largeur vraie ne couvre aucun centre de pixel. Le
// rastériseur répond en tout ou rien — le fil apparaît, disparaît, réapparaît un pixel plus
// loin dès que la caméra bouge d'un cheveu. C'est du crénelage binaire, et aucun filtrage de
// texture ne le rattrape : le défaut est dans la GÉOMÉTRIE, pas dans la texture.
//
// POURQUOI PAS VK_PRIMITIVE_TOPOLOGY_LINE_LIST : une ligne matérielle fait toujours 1 pixel
// PLEIN. À 3 km, elle peint donc un fil de 15 mm comme s'il était large de 4,8 m — le ciel
// se quadrillerait de traits noirs francs. Pire, sa position s'arrondit au centre du pixel :
// le trait RAMPE de pixel en pixel quand on avance. On échangerait la disparition contre le
// grouillement.
//
// LA SOLUTION : ruban face-caméra + largeur CLAMPÉE en pixels + COUVERTURE analytique.
//   1. le ruban est déplié ici, face à la caméra, autour de la ligne médiane ;
//   2. sa demi-largeur est forcée à kWireMinHalfPx pixels au minimum => le rastériseur a
//      toujours de quoi mordre, la géométrie ne peut plus se dérober entre deux pixels ;
//   3. on rend l'opacité de ce qu'on a volé en largeur : coverage = largeurVraie/largeurRendue.
//      À 3 km, on dessine 1 px large à 0,3 % d'opacité — soit EXACTEMENT ce qu'un
//      supersampling infini calculerait pour un fil couvrant 0,3 % du pixel.
// Le crénelage disparaît parce que la couverture est une fonction CONTINUE de la distance :
// plus aucune décision binaire, donc plus rien à faire clignoter.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbrFactors;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;   // inutilisée : la normale est reconstruite au fragment
layout(location = 2) in vec2 inUV;       // x = côté (-1/+1), y = rayon VRAI du fil (m)
layout(location = 3) in vec4 inTangent;  // xyz = direction du fil

layout(location = 0) out vec3 cameraRelPos;
layout(location = 1) out vec3 wireSide;    // direction d'élargissement, en espace monde
layout(location = 2) out vec3 wireTangent;
layout(location = 3) out float across;     // -1..+1 en travers du ruban : reconstruit le cylindre
layout(location = 4) out float coverage;   // fraction de pixel réellement couverte

// Demi-largeur minimale, en pixels. 0.5 => un ruban d'un pixel de large : le minimum pour
// que le rastériseur touche à coup sûr un centre de pixel sur une ligne fine.
const float kWireMinHalfPx = 0.5;

// Plancher de couverture. La couverture exacte rendrait le fil INVISIBLE au loin (0,3 % à
// 3 km), ce qui est physiquement juste mais dessert la lecture : l'oeil, lui, accroche une
// ligne sombre très fine sur un ciel clair bien au-delà de ce que sa couverture prédit
// (le contraste local supplée la surface). Ce plancher est donc une TRICHE ASSUMÉE, et la
// seule du fichier. Elle ne réintroduit aucun grésillement : la couverture reste une
// fonction continue de la distance, on ne fait que l'empêcher de tomber à zéro.
const float kWireMinCoverage = 0.25;

void main() {
    vec4 rel4 = object.model * vec4(inPosition, 1.0);
    vec3 rel = rel4.xyz;  // position relative caméra (origine flottante)
    vec3 T = normalize(mat3(object.model) * inTangent.xyz);

    // La caméra EST l'origine : la direction de vue est la position elle-même.
    vec3 V = normalize(rel);
    // Le ruban s'élargit perpendiculairement AU FIL et AU REGARD : c'est ce qui le fait
    // toujours présenter sa pleine largeur, quel que soit l'angle sous lequel on le croise.
    vec3 side = cross(T, V);
    float sideLen = length(side);
    // Fil vu EXACTEMENT en enfilade : la perpendiculaire est indéfinie. On prend n'importe
    // quelle direction orthogonale à la vue — le ruban est alors réduit à un point, ça n'a
    // aucune importance visuelle, mais un normalize(0) donnerait un NaN qui, lui, se voit.
    side = sideLen > 1e-5 ? side / sideLen : normalize(cross(V, vec3(0.0, 1.0, 0.0)));

    float halfWidth = inUV.y;  // rayon vrai, en mètres

    // Combien de PIXELS vaut cette demi-largeur ici ? On projette le point médian et le
    // point élargi, et on mesure l'écart en pixels. Passer par la projection plutôt que par
    // une formule en 1/z est délibéré : ça reste exact quel que soit l'endroit de l'écran
    // et quoi qu'il advienne de la matrice (reverse-z compris — les x,y n'en dépendent pas).
    mat4 vp = u.proj * u.view;
    vec4 clipC = vp * vec4(rel, 1.0);
    vec4 clipS = vp * vec4(rel + side * halfWidth, 1.0);
    vec2 viewport = u.params.zw;
    vec2 ndcC = clipC.xy / max(clipC.w, 1e-6);
    vec2 ndcS = clipS.xy / max(clipS.w, 1e-6);
    float halfPx = length((ndcS - ndcC) * 0.5 * viewport);

    // On n'élargit JAMAIS vers le bas : un fil déjà plus large qu'un pixel garde sa taille
    // vraie (scale = 1, coverage = 1) et se rend comme un tube ordinaire.
    float scale = max(1.0, kWireMinHalfPx / max(halfPx, 1e-6));
    coverage = max(1.0 / scale, kWireMinCoverage);

    across = inUV.x;
    wireSide = side;
    wireTangent = T;
    vec3 expanded = rel + side * (inUV.x * halfWidth * scale);
    cameraRelPos = expanded;
    gl_Position = vp * vec4(expanded, 1.0);
}
