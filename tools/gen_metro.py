#!/usr/bin/env python3
"""Rame de métro japonais (type E235, Yamanote Line) — M30, LE GRAND PIVOT.

Fini le TGV et son nez pointu : une caisse CUBIQUE, triviale à modéliser,
construite par PANNEaux (plancher, 2 murs latéraux, toit plat, faces d'extrémité)
avec des TROUS PHYSIQUES pour les fenêtres, les portes et le pare-brise.

Trois fichiers :
  * metro_motrice.glb : voiture de tête avec CABINE. Face avant quasi PLATE avec
    un ÉNORME trou carré pour le pare-brise (verre PBR BLEND + doubleSided) —
    rien ne bouche la vue depuis la tête du conducteur (0, 0.25, -8.55).
    Pupitre japonais : manipulateur unique à gauche (T-handle), 2 écrans, siège.
  * metro_voiture.glb : voiture intermédiaire, 4 PORTES DOUBLES coulissantes par
    face. Les battants sont REGROUPÉS PAR MOUVEMENT dans les 8 DERNIÈRES primitives
    du GLB — 4 groupes d'acier (droit/A, droit/B, gauche/A, gauche/B) puis leurs
    4 bandes vertes. Convention que l'app anime (bouchon latéral puis coulissement).
  * metro_bogie.glb : bogie, origine au plan de roulement ; les 2 dernières
    primitives sont les essieux centrés à l'origine (roues Ø920, voie 1435 mm).

Intérieur de métro : BANQUETTES LONGITUDINALES le long des murs, grand espace
vide au centre, vrai sol, vrai plafond.

M50 : 1 bloc de climatisation gris clair par voiture (4,0 x 2,0 x 0,4 m, centré
sur le toit ; décalé à z = +5 sur la motrice, où le pantographe occupe déjà le
milieu). M48 : pantographe en
losange sur la motrice, phares blancs et feux rouges ÉMISSIFS HDR aux DEUX
extrémités de la motrice (rame réversible). Toute cette géométrie vit dans des
parts dont le matériau précède MAT_DOOR0 : les 8 groupes de battants restent les
8 DERNIÈRES primitives, et les 2 essieux les 2 dernières du bogie.

Carrosserie : ACIER INOXYDABLE BROSSÉ (métallique) + bande verte Yamanote.

M53 — TEXTURES. La rame était intégralement en APLATS de couleur : une caisse
« gris métallique », un intérieur « blanc cassé », un pupitre « gris foncé ». Un
aplat ne porte aucune échelle et aucune matière : la carrosserie ressemblait à du
plastique peint, et le poste de conduite à un bloc de résine. Chaque grande
surface reçoit donc maintenant un jeu PBR complet (base color / ARM / normale)
engendré par tools/gen_textures.py, RÉFÉRENCÉ par URI relative depuis
assets/textures/train/ (les cartes ne sont pas embarquées : elles sont partagées
par les trois modèles, et les dupliquer trois fois dans les .glb n'apporterait
rien qu'un dépôt plus lourd).

Corollaire indispensable : les UV. Elles étaient toutes en 0..1 PAR QUAD, donc
chaque facette étirait une texture entière — un panneau de 4 m et une baguette de
4 cm recevaient exactement la même image. Elles sont désormais PLANAIRES ET
MÉTRIQUES (projection sur les deux axes dominants de la facette, divisée par
UV_PERIOD) : deux facettes voisines partagent le même espace texture, le motif
est continu d'un panneau à l'autre, et une tuile fait toujours un mètre.

Repère caisse : x = droite, y = haut, z = arrière ; rail à y = -2.20.
Aucune dépendance externe (stdlib seule)."""

import struct
import json
import math
import os
import sys

# --- Cotes (mètres) -------------------------------------------------------------
RAIL = -2.20
BODY_LEN = 20.0
HALF_W = 1.475          # 2.95 m (gabarit banlieue JP, voie 1067 mm... ici 1435)
WALL = 0.10
BODY_BOT = -1.15        # bas de caisse (1.05 m au-dessus du rail)
ROOF_Y = 1.40           # faces d'extrémité montent jusqu'ici (3.60 m au-dessus du rail)
IN_FLOOR = -1.00        # sol intérieur
IN_CEIL = 1.10          # plafond intérieur

Z_HEAD = -BODY_LEN / 2.0    # -10.0 (avant)
Z_TAIL = BODY_LEN / 2.0     # +10.0 (arrière)

# Bandeau vitré latéral (appui à 1.05 m au-dessus du sol).
WIN_SILL, WIN_LINTEL = 0.05, 0.85

# Portes doubles coulissantes : 4 embrasures par face, 1.30 m de passage libre.
DOOR_Y0, DOOR_Y1 = -1.00, 0.90
DOOR_CENTERS = (-7.5, -2.5, 2.5, 7.5)
DOOR_HALF = 0.65

# Climatisation de toit (M50) : un bloc par voiture, 4,0 x 2,0 x 0,4 m.
CLIM_HALF_L = 2.0       # 4,0 m dans le sens de la marche
CLIM_HALF_W = 1.0       # 2,0 m en travers (toit large de 2,95 m)
CLIM_H = 0.40           # hauteur au-dessus de la tôle de toiture
CLIM_Z_MOTRICE = 5.0    # décalé vers l'arrière : le pantographe tient le milieu

# Pare-brise de la motrice : énorme trou carré dans la face avant.
WS_X, WS_Y0, WS_Y1 = 1.20, -0.20, 1.10

# Cabine : tête conducteur (0, 0.25, -8.55), cloison à z = -8.0.
Z_CLOISON = -8.0

# --- Textures (M53) ---------------------------------------------------------------
# Jeux PBR engendrés par tools/gen_textures.py. Chemin RELATIF au .glb, qui vit dans
# assets/models/ — le chargeur résout les URI relatives depuis le dossier du fichier
# (cf. resolve_image dans gltf_loader.cpp).
TEXDIR = "../textures/train"
UV_PERIOD = 1.0   # côté, en mètres, d'une tuile de texture. Doit valoir la période
                  # déclarée dans gen_textures.py pour la rame, sinon l'échelle ment.


def tex(name):
    """Un jeu complet : base color (sRGB) + ARM + normale."""
    return {"diff": f"{TEXDIR}/{name}_diff.png",
            "arm": f"{TEXDIR}/{name}_arm.png",
            "nor": f"{TEXDIR}/{name}_nor.png"}


def relief(name):
    """ARM + normale SANS base color : la matière (grain, rugosité, relief) vient de
    la texture, la COULEUR reste au facteur. C'est ce qu'il faut pour une surface
    peinte dans une teinte précise — bande verte, jupe, climatiseurs : leur teinte
    est une identité (le vert Yamanote), pas quelque chose qu'on laisse dériver au
    gré d'une carte. Elles gagnent le relief sans perdre leur couleur."""
    return {"arm": f"{TEXDIR}/{name}_arm.png", "nor": f"{TEXDIR}/{name}_nor.png"}


# --- Matériaux PBR --------------------------------------------------------------
# Les facteurs sont des MULTIPLICATEURS de la texture (convention glTF). Quand une
# base color est fournie, le facteur reste donc à 1 : c'est la carte qui décide de
# l'albédo, et la remultiplier par l'ancienne teinte l'assombrirait deux fois.
MATERIALS = [
    # 0: acier — inoxydable BROSSÉ. Le brossage est une variation de rugosité, pas de
    # couleur : sans la carte ARM, un aplat métallique ne ressemblera jamais à de l'inox.
    {"name": "acier", "factor": [1.0, 1.0, 1.0, 1.0], "metallic": 1.0, "roughness": 1.0,
     **tex("steel")},
    # 1: vitrage — verre PBR, transparence absolue (cahier des charges M30). Aucune
    # texture : une vitre propre n'a pas de matière à montrer.
    {"name": "vitrage", "factor": [0.1, 0.1, 0.1, 0.3], "metallic": 0.0, "roughness": 0.02,
     "blend": True, "doubleSided": True},
    # 2: bande — vert vif Yamanote (#6CBB5A), peinture sur tôle.
    {"name": "bande", "factor": [0.42, 0.73, 0.35, 1.0], "metallic": 0.20, "roughness": 0.55,
     **relief("panel")},
    # 3: jupe — équipement sous caisse, gris foncé mat et grainé
    {"name": "jupe", "factor": [0.16, 0.17, 0.18, 1.0], "metallic": 0.10, "roughness": 1.0,
     **relief("console")},
    # 4: interieur — panneaux de mélamine, double face
    {"name": "interieur", "factor": [1.0, 1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 1.0,
     "doubleSided": True, **tex("panel")},
    # 5: banquette — moquette bleue tissée
    {"name": "banquette", "factor": [1.0, 1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 1.0,
     **tex("fabric")},
    # 6: pupitre — console de conduite, plastique technique grainé
    {"name": "pupitre", "factor": [1.0, 1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 1.0,
     **tex("console")},
    # 7: ecran — écrans de conduite bleutés, LÉGÈREMENT émissifs (M53) : un écran
    # allumé ne dépend pas de l'éclairage de la cabine, il en est une source.
    {"name": "ecran", "factor": [0.05, 0.15, 0.30, 1.0], "metallic": 0.0, "roughness": 0.10,
     "emissive": [0.10, 0.45, 0.85]},
    # 8: commande — manipulateur / boutons
    {"name": "commande", "factor": [0.75, 0.20, 0.15, 1.0], "metallic": 0.40, "roughness": 1.0,
     **relief("console")},
    # 9: phare — bloc optique avant, blanc chaud ÉMISSIF HDR (M48, facteur > 1 :
    # le loader accepte les valeurs hors spec, cf. asset_types.hpp)
    {"name": "phare", "factor": [1.0, 0.95, 0.80, 1.0], "metallic": 0.0, "roughness": 0.20,
     "emissive": [4.0, 3.8, 3.2]},
    # 10: feu_ar — feu rouge arrière ÉMISSIF HDR (M48)
    {"name": "feu_ar", "factor": [0.30, 0.02, 0.02, 1.0], "metallic": 0.0, "roughness": 0.30,
     "emissive": [4.0, 0.10, 0.05]},
    # 11: clim — boîtiers de climatisation de toit, gris clair mat (M48)
    {"name": "clim", "factor": [0.70, 0.71, 0.72, 1.0], "metallic": 0.05, "roughness": 1.0,
     **relief("panel")},
    # 12: panto — pantographe, acier sombre métallique (M48)
    {"name": "panto", "factor": [0.10, 0.10, 0.11, 1.0], "metallic": 0.85, "roughness": 1.0,
     **relief("steel")},
]
MAT_ACIER, MAT_GLASS, MAT_BANDE, MAT_JUPE, MAT_INTERIOR = 0, 1, 2, 3, 4
MAT_BENCH, MAT_PUPITRE, MAT_ECRAN, MAT_COMMANDE, MAT_PHARE = 5, 6, 7, 8, 9
MAT_FEU_AR, MAT_CLIM, MAT_PANTO = 10, 11, 12

# LES INDICES SUIVANTS SE CALCULENT. Ils étaient écrits en dur (13, 29, 45...), ce
# qui rendait l'ajout d'un matériau au milieu de la liste silencieusement destructeur :
# l'app repère les battants de porte par leur RANG de primitive, et tout décalage les
# aurait fait animer la mauvaise pièce.
# 13: sol — revêtement de plancher, distinct de la mélamine des parois (M53). Un métro
# n'a pas le même sol que ses murs, et c'est la surface qu'on regarde le plus.
MAT_FLOOR = len(MATERIALS)
MATERIALS.append({"name": "sol", "factor": [1.0, 1.0, 1.0, 1.0], "metallic": 0.0,
                  "roughness": 1.0, **tex("floor")})
# --- M54 : LES BATTANTS REGROUPÉS PAR MOUVEMENT ---------------------------------
# Il y avait 16 battants dans 16 parts séparées, plus 16 bandes : 32 primitives, donc
# 32 draw calls par voiture, soit 96 pour la rame — un tiers de toute la scène pour
# des panneaux de 65 cm.
#
# Or ces 16 battants n'ont que QUATRE mouvements distincts : flanc droit ou gauche
# (sortie du bouchon vers +x ou -x), vantail A ou B (coulissement vers -z ou +z). Tous
# les battants d'un même groupe subissent EXACTEMENT la même translation à chaque
# frame. Les garder séparés ne servait donc à rien — c'était payer 4 fois le même
# draw. On les fusionne par groupe : 4 parts d'acier + 4 de bande verte.
#
# Ordre des groupes, que l'app recopie : 0 = droit/A, 1 = droit/B, 2 = gauche/A,
# 3 = gauche/B.
DOOR_GROUPS = 4
MAT_DOOR0 = len(MATERIALS)
for _ in range(DOOR_GROUPS):
    MATERIALS.append({"name": "porte", "factor": [1.0, 1.0, 1.0, 1.0],
                      "metallic": 1.0, "roughness": 1.0, **tex("steel")})
# Bande verte, découpée sur les mêmes groupes (M36)
MAT_DOOR_BANDE0 = len(MATERIALS)
for _ in range(DOOR_GROUPS):
    MATERIALS.append({"name": "bande", "factor": [0.42, 0.73, 0.35, 1.0],
                      "metallic": 0.20, "roughness": 0.55, **relief("panel")})
# Bogie + 2 essieux (parts séparées, les 3 dernières du GLB bogie). Ils DOIVENT rester
# après les battants : l'app lit « les 32 dernières primitives » sur la motrice et la
# voiture, où ces trois parts-là sont vides.
MAT_BOGIE = len(MATERIALS)
MATERIALS.append({"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0,
                  "roughness": 1.0, **relief("console")})
MAT_AXLE_A = len(MATERIALS)
MATERIALS.append({"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30})
MAT_AXLE_B = len(MATERIALS)
MATERIALS.append({"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30})

# Embrasures de portes (z0, z1, y0, y1). La motrice décale sa 1re porte vers
# l'arrière (la cloison de cabine occupe z = -8.0).
DOORWAYS = [(zc - DOOR_HALF, zc + DOOR_HALF, DOOR_Y0, DOOR_Y1) for zc in DOOR_CENTERS]
MOTRICE_CENTERS = (-6.5, -2.5, 2.5, 7.5)
MOTRICE_DOORWAYS = [(zc - DOOR_HALF, zc + DOOR_HALF, DOOR_Y0, DOOR_Y1)
                    for zc in MOTRICE_CENTERS]

# Baies vitrées latérales : une fenêtre entre chaque paire de portes et aux bouts.
def side_openings(doorways=DOORWAYS):
    """Ouvertures d'un mur latéral : portes + fenêtres entre les portes."""
    openings = list(doorways)
    centers = [0.5 * (d[0] + d[1]) for d in doorways]
    spans = [(Z_HEAD + 0.5, centers[0] - DOOR_HALF)]
    for a, b in zip(centers, centers[1:]):
        spans.append((a + DOOR_HALF, b - DOOR_HALF))
    spans.append((centers[-1] + DOOR_HALF, Z_TAIL - 0.5))
    for za, zb in spans:
        if zb - za > 0.3:
            openings.append((za + 0.15, zb - 0.15, WIN_SILL, WIN_LINTEL))
    return openings


OPENINGS = side_openings()
MOTRICE_OPENINGS = side_openings(MOTRICE_DOORWAYS)
WINDOWS = [o for o in OPENINGS if o[2] == WIN_SILL]
MOTRICE_WINDOWS = [o for o in MOTRICE_OPENINGS if o[2] == WIN_SILL]


# --- Algèbre --------------------------------------------------------------------
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def mul(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 1.0, 0.0)


def uv_axes(n):
    """Axes de projection UV d'une facette, choisis par sa normale DOMINANTE.

    C'est un mapping planaire par axe — la solution standard pour une géométrie
    entièrement faite de boîtes alignées : chaque face tombe exactement sur l'un des
    trois plans, sans distorsion, et deux faces du même plan restent solidaires. On
    prend systématiquement +y comme axe vertical de texture quand c'est possible, pour
    qu'un motif directionnel (le brossage de l'inox, l'armure d'une moquette) ait la
    même orientation sur les deux flancs de la caisse."""
    ax, ay, az = abs(n[0]), abs(n[1]), abs(n[2])
    if ax >= ay and ax >= az:
        return (0.0, 0.0, 1.0), (0.0, 1.0, 0.0)   # flanc : u = z (longueur), v = y
    if ay >= az:
        return (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)   # sol / toit : u = x, v = z
    return (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)       # face d'extrémité : u = x, v = y


class Part:
    """Une part = un matériau = une primitive glTF."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

    def orient(self):
        """M56 — REMET TOUTES LES FACES À L'ENDROIT.

        glTF impose le sens TRIGONOMÉTRIQUE vu de l'extérieur (spec 3.7.2.1), et le
        moteur rastérise avec VK_FRONT_FACE_COUNTER_CLOCKWISE + back-face culling. Or
        `add_box` énumérait ses coins dans le sens HORAIRE : toute la rame était donc
        cousue à l'envers. Le GPU jetait ses faces extérieures et ne gardait que
        l'intérieur de la paroi opposée — d'où une caisse qui se traversait du regard,
        des banquettes flottant à travers la tôle et des roues trouées.

        Plutôt que de corriger chaque énumération de coins (et de risquer d'en réintroduire
        une à la prochaine pièce), on NORMALISE ICI, une fois pour toutes, juste avant la
        sérialisation : si la normale géométrique d'un triangle contredit la normale de ses
        sommets — qui, elle, a toujours été juste — on échange deux indices. C'est un
        invariant vérifiable (cf. tools/check_topology.py), pas une convention à retenir."""
        flipped = 0
        for t in range(0, len(self.indices), 3):
            ia, ib, ic = self.indices[t], self.indices[t + 1], self.indices[t + 2]
            a, b, c = self.positions[ia], self.positions[ib], self.positions[ic]
            g = cross(sub(b, a), sub(c, a))
            vn = self.normals[ia]
            if dot(g, vn) < 0.0:
                self.indices[t + 1], self.indices[t + 2] = ic, ib
                flipped += 1
        return flipped

    def add(self, verts):
        base = len(self.positions)
        for p, n, uv, tg in verts:
            self.positions.append(p)
            self.normals.append(n)
            self.uvs.append(uv)
            self.tangents.append(tg)
        for k in range(1, len(verts) - 1):
            self.indices.extend([base, base + k, base + k + 1])

    def add_quad(self, p0, p1, p2, p3, n=None):
        if n is None:
            n = norm(cross(sub(p1, p0), sub(p2, p0)))
        # UV PLANAIRES MÉTRIQUES (M53) : la facette est projetée sur les deux axes du
        # repère caisse qui ne sont pas le sien, et la coordonnée est la cote EN
        # MÈTRES divisée par UV_PERIOD. Conséquence directe : deux facettes coplanaires
        # engendrées séparément (un mur découpé en dalles autour de ses ouvertures, par
        # exemple) partagent exactement le même espace texture — le motif traverse la
        # découpe sans couture. Les anciennes UV 0..1 par quad rendaient ça impossible :
        # chaque dalle réétirait la texture entière sur sa propre taille.
        u_ax, v_ax = uv_axes(n)
        # w = sens du bitangent attendu par le shader (B = cross(N, T) * w). On veut
        # B le long de +v, sinon la normal map est lue à l'envers sur un axe et le
        # relief se creuse là où il devrait saillir.
        w = 1.0 if dot(cross(n, u_ax), v_ax) >= 0.0 else -1.0
        tg = (u_ax[0], u_ax[1], u_ax[2], w)
        verts = []
        for p in (p0, p1, p2, p3):
            uv = (dot(p, u_ax) / UV_PERIOD, dot(p, v_ax) / UV_PERIOD)
            verts.append((p, n, uv, tg))
        self.add(verts)

    def add_box(self, x0, y0, z0, x1, y1, z1,
                top=True, bot=True, front=True, back=True, left=True, right=True):
        c000, c100 = (x0, y0, z0), (x1, y0, z0)
        c110, c010 = (x1, y1, z0), (x0, y1, z0)
        c001, c101 = (x0, y0, z1), (x1, y0, z1)
        c111, c011 = (x1, y1, z1), (x0, y1, z1)
        if back:   self.add_quad(c000, c100, c110, c010, (0, 0, -1))
        if front:  self.add_quad(c101, c001, c011, c111, (0, 0, 1))
        if left:   self.add_quad(c001, c000, c010, c011, (-1, 0, 0))
        if right:  self.add_quad(c100, c101, c111, c110, (1, 0, 0))
        if bot:    self.add_quad(c001, c101, c100, c000, (0, -1, 0))
        if top:    self.add_quad(c010, c110, c111, c011, (0, 1, 0))


def quad_orient(part, a, b, c, d, want):
    n = norm(cross(sub(b, a), sub(c, a)))
    if dot(n, want) < 0.0:
        b, d = d, b
        n = mul(n, -1.0)
    part.add_quad(a, b, c, d, n)


def wall_with_openings(part, xa, xb, z0, z1, y0, y1, openings):
    """Mur latéral plan AVEC TROUS PHYSIQUES (découpe en dalles z/y)."""
    z_edges = sorted({z0, z1} | {o[0] for o in openings} | {o[1] for o in openings})
    for za, zb in zip(z_edges, z_edges[1:]):
        zc = 0.5 * (za + zb)
        covering = [o for o in openings if o[0] <= zc <= o[1]]
        if not covering:
            part.add_box(xa, y0, za, xb, y1, zb)
            continue
        y_edges = sorted({y0, y1} | {o[2] for o in covering} | {o[3] for o in covering})
        for ya, yb in zip(y_edges, y_edges[1:]):
            yc = 0.5 * (ya + yb)
            if any(o[2] <= yc <= o[3] for o in covering):
                continue
            part.add_box(xa, ya, za, xb, yb, zb)


# ==============================================================================
# CAISSE COMMUNE (motrice et voiture)
# ==============================================================================
def build_floor(parts):
    """Plancher + jupe d'équipement sous caisse. Jupe rentrée de 5 mm sur les
    côtés et abaissée de 5 mm sous le plancher : aucune face coplanaire avec
    les murs ou le bas des dalles (anti z-fighting, M30.5)."""
    parts[MAT_JUPE].add_box(-HALF_W + 0.005, BODY_BOT - 0.15, Z_HEAD + 0.2,
                            HALF_W - 0.005, IN_FLOOR - 0.005, Z_TAIL - 0.2)
    parts[MAT_JUPE].add_box(-HALF_W + 0.10, RAIL + 0.35, Z_HEAD + 1.2,
                            HALF_W - 0.10, BODY_BOT - 0.14, Z_TAIL - 1.2)


def build_side_walls(parts, openings=OPENINGS, windows=WINDOWS, doorways=DOORWAYS):
    """Murs latéraux avec fenêtres et portes DÉCOUPÉES, verre dans les baies."""
    for side in (-1.0, 1.0):
        xa, xb = sorted((side * HALF_W, side * (HALF_W - WALL)))
        wall_with_openings(parts[MAT_ACIER], xa, xb, Z_HEAD, Z_TAIL,
                           BODY_BOT, ROOF_Y, openings)
        for (z0, z1, y0, y1) in windows:
            parts[MAT_GLASS].add_box(xa - 0.005, y0, z0, xb + 0.005, y1, z1,
                                     top=False, bot=False, front=False, back=False)
        # Bande verte Yamanote DÉCOUPÉE sur la carrosserie fixe (M36) :
        # Saute les ouvertures des portes afin de ne pas flotter à l'ouverture.
        x0, x1 = sorted((side * (HALF_W + 0.002), side * (HALF_W + 0.014)))
        door_spans = sorted([(d[0], d[1]) for d in doorways])
        cur_z = Z_HEAD + 0.3
        for (dz0, dz1) in door_spans:
            if dz0 > cur_z:
                parts[MAT_BANDE].add_box(x0, WIN_SILL - 0.22, cur_z,
                                         x1, WIN_SILL - 0.04, dz0)
            cur_z = max(cur_z, dz1)
        if Z_TAIL - 0.3 > cur_z:
            parts[MAT_BANDE].add_box(x0, WIN_SILL - 0.22, cur_z,
                                     x1, WIN_SILL - 0.04, Z_TAIL - 0.3)


def build_roof(parts, clim_z=0.0):
    """Toit plat indépendant + UN BLOC DE CLIMATISATION par voiture (M50).

    Cotes STRICTES : 4,0 m de long (axe z, dans le sens de la marche) x 2,0 m de
    large (axe x) x 0,4 m de haut. Le bloc est CENTRÉ sur le toit — x = 0, et le
    toit faisant 2,95 m de large il reste 0,475 m de rive de chaque côté, la
    caisse ne déborde pas du gabarit.

    Il est enfoncé de 5 mm dans la tôle pour n'être JAMAIS coplanaire avec elle
    (tools/check_coplanar.py) : les 0,4 m demandés sont ceux qui dépassent du
    toit, mesurés depuis sa surface supérieure (ROOF_Y + 0.10).

    `clim_z` décale le bloc sur la MOTRICE, dont le milieu de toit est déjà pris
    par l'embase du pantographe (z de -1,6 à +1,6, cf. build_pantograph) : laissé
    à z = 0, le climatiseur l'engloberait — exactement le genre d'interpénétration
    que le M50 corrige ailleurs."""
    roof_top = ROOF_Y + 0.10
    parts[MAT_ACIER].add_box(-HALF_W, ROOF_Y, Z_HEAD, HALF_W, roof_top, Z_TAIL)
    parts[MAT_CLIM].add_box(-CLIM_HALF_W, roof_top - 0.005, clim_z - CLIM_HALF_L,
                            CLIM_HALF_W, roof_top + CLIM_H, clim_z + CLIM_HALF_L)


def add_bar(part, a, b, w):
    """Barre cylindrique approchée (prisme carré de section w) entre deux points.
    Utilisée pour le pantographe (M48).

    M56 — Toutes les barres d'un même demi-cadre partagent l'abscisse x = ±0.45 et la
    même section : leurs flancs tombaient donc dans EXACTEMENT le même plan, et aux
    articulations — où deux barres se recouvrent, comme dans un vrai pantographe — le
    rastériseur n'avait aucun moyen de départager les deux surfaces. D'où 24 paires en
    z-fighting sur le toit de la motrice, c'est-à-dire un scintillement bien visible dès
    qu'on regarde la rame de trois quarts. On désaccorde donc la section de chaque barre
    de 0 à 2 %% (moins d'un millimètre), de façon DÉTERMINISTE (dérivée de ses extrémités) :
    deux barres distinctes n'ont plus jamais un flanc coplanaire, et 0,8 mm sur 4 cm ne
    se voit pas."""
    add_bar.count = getattr(add_bar, "count", 0) + 1
    d = norm(sub(b, a))
    ref = (0.0, 1.0, 0.0) if abs(d[1]) < 0.9 else (1.0, 0.0, 0.0)
    u_hat = norm(cross(d, ref))
    # Décalage latéral propre à la barre : 0,3 mm par rang, 0 à 3,6 mm. Il déplace les
    # DEUX flancs du même montant, donc il désaccorde les plans sans déformer la section.
    off = 0.0003 * (1 + add_bar.count % 17)
    a = (a[0] + off * u_hat[0], a[1] + off * u_hat[1], a[2] + off * u_hat[2])
    b = (b[0] + off * u_hat[0], b[1] + off * u_hat[1], b[2] + off * u_hat[2])
    u = mul(u_hat, w * 0.5)
    v = mul(norm(cross(d, u_hat)), w * 0.5)
    a0 = (a[0] - u[0] - v[0], a[1] - u[1] - v[1], a[2] - u[2] - v[2])
    a1 = (a[0] + u[0] - v[0], a[1] + u[1] - v[1], a[2] + u[2] - v[2])
    a2 = (a[0] + u[0] + v[0], a[1] + u[1] + v[1], a[2] + u[2] + v[2])
    a3 = (a[0] - u[0] + v[0], a[1] - u[1] + v[1], a[2] - u[2] + v[2])
    b0 = (b[0] - u[0] - v[0], b[1] - u[1] - v[1], b[2] - u[2] - v[2])
    b1 = (b[0] + u[0] - v[0], b[1] + u[1] - v[1], b[2] + u[2] - v[2])
    b2 = (b[0] + u[0] + v[0], b[1] + u[1] + v[1], b[2] + u[2] + v[2])
    b3 = (b[0] - u[0] + v[0], b[1] - u[1] + v[1], b[2] - u[2] + v[2])
    part.add_quad(a0, a1, b1, b0)  # face -v
    part.add_quad(a1, a2, b2, b1)  # face +u
    part.add_quad(a2, a3, b3, b2)  # face +v
    part.add_quad(a3, a0, b0, b3)  # face -u
    quad_orient(part, a3, a2, a1, a0, mul(d, -1.0))
    quad_orient(part, b1, b2, b3, b0, d)


def build_pantograph(parts):
    """Pantographe simple sur le toit de la motrice (M48) : 4 isolateurs, cadre
    en losange à deux bras parallèles, barre de contact à ~1.0 m au-dessus du
    toit (toit à ROOF_Y + 0.10). Centré à z = 0, entre les deux climatiseurs."""
    panto = parts[MAT_PANTO]
    y_roof = ROOF_Y + 0.10
    z0, z1 = -1.6, 1.6          # embase le long de l'axe
    y_mid, y_top, y_bar = y_roof + 0.55, y_roof + 0.92, y_roof + 1.00
    for sx in (-0.45, 0.45):
        # Isolateurs (base isolante, gris clair).
        for sz in (z0, z1):
            parts[MAT_CLIM].add_box(sx - 0.06, y_roof, sz - 0.06,
                                    sx + 0.06, y_roof + 0.12, sz + 0.06)
        # Longeron de base.
        add_bar(panto, (sx, y_roof + 0.12, z0), (sx, y_roof + 0.12, z1), 0.05)
        # Bras inférieurs (montent vers l'articulation centrale).
        add_bar(panto, (sx, y_roof + 0.12, z0), (sx, y_mid, 0.0), 0.045)
        add_bar(panto, (sx, y_roof + 0.12, z1), (sx, y_mid, 0.0), 0.045)
        # Bras supérieurs (vers le cadre porte-barre).
        add_bar(panto, (sx, y_mid, 0.0), (sx, y_top, -0.55), 0.04)
        add_bar(panto, (sx, y_mid, 0.0), (sx, y_top, 0.55), 0.04)
        # Cadre supérieur + liaisons verticales à la barre de contact.
        add_bar(panto, (sx, y_top, -0.55), (sx, y_top, 0.55), 0.04)
        add_bar(panto, (sx, y_top, 0.0), (sx, y_bar, 0.0), 0.04)
    # Traverse d'articulation et barre de contact (axe x).
    add_bar(panto, (-0.45, y_mid, 0.0), (0.45, y_mid, 0.0), 0.04)
    add_bar(panto, (-0.70, y_bar, 0.0), (0.70, y_bar, 0.0), 0.05)


def build_interior(parts, centers=DOOR_CENTERS):
    """Sol, plafond, et BANQUETTES LONGITUDINALES le long des murs (grand espace
    vide au centre — c'est un métro). Tout panneau est rentré de 5 mm sous le
    plan des murs : aucune face coplanaire (M30.5)."""
    inner = HALF_W - WALL - 0.005
    slab = inner - 0.005  # sol/plafond rentrés de 5 mm sous les banquettes (M30.5)
    # M53 : le PLANCHER a son propre matériau (revêtement vinyle moucheté). C'est la
    # surface la plus regardée d'une voiture de métro — la laisser en mélamine blanche
    # comme les parois, c'était perdre le seul repère de matière de l'intérieur.
    parts[MAT_FLOOR].add_box(-slab, IN_FLOOR - 0.015, Z_HEAD + 0.1,
                             slab, IN_FLOOR + 0.005, Z_TAIL - 0.1)
    parts[MAT_INTERIOR].add_box(-slab, IN_CEIL, Z_HEAD + 0.1,
                                slab, IN_CEIL + 0.05, Z_TAIL - 0.1)
    # Banquettes : assise à 45 cm du sol, adossée au mur, entre les portes.
    spans = [(Z_HEAD + 0.3, centers[0] - DOOR_HALF)]
    for a, b in zip(centers, centers[1:]):
        spans.append((a + DOOR_HALF, b - DOOR_HALF))
    spans.append((centers[-1] + DOOR_HALF, Z_TAIL - 0.3))
    for side in (-1.0, 1.0):
        x_wall = side * (inner - 0.010)  # 5 mm sous la dalle, 15 mm du mur (M30.5)
        x_edge = side * (inner - 0.45)
        xa, xb = sorted((x_wall, x_edge))
        for za, zb in spans:
            if zb - za < 0.4:
                continue
            # Assise enterrée de 8 mm dans la dalle de sol : sa face de fond ne
            # coïncide ni avec la cloison (-10 mm) ni avec le pupitre (-12 mm).
            parts[MAT_BENCH].add_box(xa, IN_FLOOR - 0.008, za + 0.1, xb, IN_FLOOR + 0.45, zb - 0.1)
            # dossier contre le mur
            xd0, xd1 = sorted((x_wall, x_wall - side * 0.12))
            parts[MAT_BENCH].add_box(xd0, IN_FLOOR + 0.45, za + 0.1,
                                     xd1, IN_FLOOR + 0.95, zb - 0.1)
    # Barres verticales au centre de chaque embrasure (poteaux d'accès).
    for zc in centers:
        parts[MAT_INTERIOR].add_box(-0.03, IN_FLOOR, zc - 0.03, 0.03, IN_CEIL, zc + 0.03)

    # M53 — BARRES DE MAINTIEN ET POIGNÉES SUSPENDUES (つり革). Un métro japonais vide
    # de tout équipement de maintien ne ressemble pas à un métro : ces deux lignes
    # d'accessoires au-dessus des banquettes sont ce qui donne à l'intérieur sa
    # PROFONDEUR — sans elles, le volume est une boîte lisse où l'œil n'a rien à
    # accrocher, quelle que soit la qualité des textures des parois.
    for side in (-1.0, 1.0):
        x_bar = side * (inner - 0.55)
        # Barre longitudinale sous le plafond, d'un bout à l'autre de la voiture.
        parts[MAT_INTERIOR].add_box(x_bar - 0.022, IN_CEIL - 0.24, Z_HEAD + 0.5,
                                    x_bar + 0.022, IN_CEIL - 0.196, Z_TAIL - 0.5)
        # Poignées tous les 60 cm, sauf en face des embrasures (on ne suspend rien
        # au-dessus d'un flux de descente).
        z = Z_HEAD + 0.9
        while z < Z_TAIL - 0.9:
            if all(abs(z - zc) > DOOR_HALF + 0.25 for zc in centers):
                # Sangle plate + anneau : deux boîtes, ~30 cm de haut au total.
                parts[MAT_INTERIOR].add_box(x_bar - 0.012, IN_CEIL - 0.50, z - 0.035,
                                            x_bar + 0.012, IN_CEIL - 0.22, z + 0.035)
                parts[MAT_COMMANDE].add_box(x_bar - 0.045, IN_CEIL - 0.60, z - 0.030,
                                            x_bar + 0.045, IN_CEIL - 0.49, z + 0.030)
            z += 0.60


def build_end_face(parts, zend, with_windshield):
    """Face d'extrémité, plaquée 10 cm VERS L'EXTÉRIEUR de la caisse : sa face
    intérieure (z = zend) est dos-à-dos avec les faces de bout des murs et du
    toit (normales opposées, jamais rasterisées ensemble) au lieu d'être
    coplanaire avec elles (anti z-fighting, M30.5). Motrice : ÉNORME trou carré
    de pare-brise.

    M56 — LA TRANCHE ÉTAIT OUVERTE. La plaque s'arrêtait à ROOF_Y et renonçait à
    ses faces gauche, droite et supérieure « parce qu'elles tombent dans le plan
    des murs » : elles y tombent, mais 10 cm PLUS LOIN en z, là où il n'y a plus
    ni mur ni toit. Il restait donc, aux deux bouts de chaque voiture, trois
    fentes de 10 cm ouvertes sur le vide — le regard entrait dans la caisse par
    la tranche, et c'est ce qui se voyait le plus entre deux voitures attelées.
    La plaque monte maintenant jusqu'au DESSUS du toit et elle est FERMÉE sur ses
    six faces. Coplanaire avec les murs, oui ; superposée à eux, jamais (les
    plages de z sont disjointes), donc aucun z-fighting — c'est exactement ce que
    vérifie tools/check_coplanar.py."""
    z0, z1 = sorted((zend, zend + (-0.10 if zend < 0 else 0.10)))
    roof_top = ROOF_Y + 0.10
    if not with_windshield:
        parts[MAT_ACIER].add_box(-HALF_W, BODY_BOT, z0, HALF_W, roof_top, z1)
        return
    # Piliers latéraux, bande basse, bande haute : le trou est RÉEL. Seules les
    # faces qui se SUPERPOSENT vraiment à une voisine (les flancs internes des
    # bandeaux, cachés par les piliers) restent supprimées.
    parts[MAT_ACIER].add_box(-HALF_W, BODY_BOT, z0, -WS_X, roof_top, z1)
    parts[MAT_ACIER].add_box(WS_X, BODY_BOT, z0, HALF_W, roof_top, z1)
    parts[MAT_ACIER].add_box(-WS_X, BODY_BOT, z0, WS_X, WS_Y0, z1,
                             left=False, right=False)
    parts[MAT_ACIER].add_box(-WS_X, WS_Y1, z0, WS_X, roof_top, z1,
                             left=False, right=False)
    # Verre du pare-brise, à fleur de la face (z = zend), décalé de ±10 mm.
    parts[MAT_GLASS].add_box(-WS_X, WS_Y0, zend - 0.01, WS_X, WS_Y1, zend + 0.01,
                             top=False, bot=False, left=False, right=False)


def build_lights(parts):
    """Feux de la motrice (M48) : rame RÉVERSIBLE — phares blancs ÉMISSIFS HDR +
    feux rouges émissifs aux DEUX extrémités (kLocoTransform = rotation nulle :
    la même motrice peut se retrouver en tête quel que soit le sens). Blocs en
    saillie de 4 cm hors de la face, enterrés de 5 mm dans la plaque d'extrémité
    (jamais coplanaires, M30.5). Phares HORS de l'ouverture du pare-brise."""
    for zend in (Z_HEAD, Z_TAIL):
        if zend < 0.0:
            lz0, lz1 = zend - 0.14, zend - 0.095
        else:
            lz0, lz1 = zend + 0.095, zend + 0.14
        for sx in (-1.0, 1.0):
            parts[MAT_PHARE].add_box(sx * 1.34 - 0.10, WS_Y0 + 0.05, lz0,
                                     sx * 1.34 + 0.10, WS_Y0 + 0.30, lz1)
            parts[MAT_FEU_AR].add_box(sx * 1.34 - 0.08, WS_Y0 - 0.30, lz0,
                                      sx * 1.34 + 0.08, WS_Y0 - 0.06, lz1)


# --- Pupitre : le plan incliné et son repère (M56) --------------------------------
# Section du dièdre dans le plan (y, z), vue de la gauche :
#   A = bord bas-avant (côté conducteur), B = bord haut-arrière (côté pare-brise).
PANEL_X = 1.05
PANEL_A = (-0.300, -8.980)      # (y, z)
PANEL_B = (-0.020, -9.220)      # (y, z)
PANEL_BASE_Y = -0.305           # la joue descend 5 mm DANS la console : rien de coplanaire


def _panel_frame():
    """(origine, axe de pente unitaire, normale sortante unitaire) dans le plan (y, z)."""
    dy, dz = PANEL_B[0] - PANEL_A[0], PANEL_B[1] - PANEL_A[1]
    ln = math.hypot(dy, dz)
    sy, sz = dy / ln, dz / ln
    # Normale perpendiculaire à la pente, choisie du côté OPPOSÉ au fond du dièdre.
    ny, nz = -sz, sy
    if ny * (PANEL_A[0] - PANEL_B[0]) + nz * (PANEL_A[1] - PANEL_B[1]) > 0.0:
        ny, nz = -ny, -nz
    return ln, (sy, sz), (ny, nz)


PANEL_LEN, PANEL_S, PANEL_N = _panel_frame()


def panel_point(x, s, t):
    """Point du repère du panneau : s = 0..1 le long de la pente (de A vers B),
    t = déport en mètres suivant la normale sortante."""
    d = s * PANEL_LEN
    return (x,
            PANEL_A[0] + d * PANEL_S[0] + t * PANEL_N[0],
            PANEL_A[1] + d * PANEL_S[1] + t * PANEL_N[1])


def panel_slab(part, x0, x1, s0, s1, t0, t1):
    """Pavé posé SUR la pente : rectangle (s0..s1) x (x0..x1), épaissi de t0 à t1
    suivant la normale. Six faces, orientation rattrapée par Part.orient()."""
    n = (0.0, PANEL_N[0], PANEL_N[1])
    up = (0.0, PANEL_S[0], PANEL_S[1])
    c = {(a, b, d): panel_point(x0 if a == 0 else x1, s0 if b == 0 else s1,
                                t0 if d == 0 else t1)
         for a in (0, 1) for b in (0, 1) for d in (0, 1)}
    part.add_quad(c[0, 0, 1], c[1, 0, 1], c[1, 1, 1], c[0, 1, 1], n)                  # dessus
    part.add_quad(c[0, 0, 0], c[0, 1, 0], c[1, 1, 0], c[1, 0, 0], mul(n, -1.0))       # dessous
    part.add_quad(c[0, 1, 0], c[0, 1, 1], c[1, 1, 1], c[1, 1, 0], up)                 # haut de pente
    part.add_quad(c[0, 0, 0], c[1, 0, 0], c[1, 0, 1], c[0, 0, 1], mul(up, -1.0))      # bas de pente
    part.add_quad(c[1, 0, 0], c[1, 1, 0], c[1, 1, 1], c[1, 0, 1], (1.0, 0.0, 0.0))    # joue +x
    part.add_quad(c[0, 0, 0], c[0, 0, 1], c[0, 1, 1], c[0, 1, 0], (-1.0, 0.0, 0.0))   # joue -x


def build_panel_wedge(part):
    """Le dièdre du panneau d'instruments : pente, fond vertical, semelle, 2 joues."""
    ay, az = PANEL_A
    by, bz = PANEL_B
    for sx in (-1.0, 1.0):
        x = sx * PANEL_X
        part.add([((x, ay, az), (sx, 0.0, 0.0), (az, ay), (0.0, 0.0, 1.0, 1.0)),
                  ((x, by, bz), (sx, 0.0, 0.0), (bz, by), (0.0, 0.0, 1.0, 1.0)),
                  ((x, PANEL_BASE_Y, bz), (sx, 0.0, 0.0), (bz, PANEL_BASE_Y),
                   (0.0, 0.0, 1.0, 1.0))])
    n = (0.0, PANEL_N[0], PANEL_N[1])
    part.add_quad((-PANEL_X, ay, az), (PANEL_X, ay, az),
                  (PANEL_X, by, bz), (-PANEL_X, by, bz), n)                      # la pente
    part.add_quad((-PANEL_X, PANEL_BASE_Y, bz), (PANEL_X, PANEL_BASE_Y, bz),
                  (PANEL_X, by, bz), (-PANEL_X, by, bz), (0.0, 0.0, -1.0))       # le fond
    part.add_quad((-PANEL_X, PANEL_BASE_Y, bz), (PANEL_X, PANEL_BASE_Y, bz),
                  (PANEL_X, ay, az), (-PANEL_X, ay, az), (0.0, -1.0, 0.0))       # la semelle


def build_cab(parts):
    """Cabine : cloison, pupitre avec manipulateur T à gauche, écrans, siège.
    Le volume entre le sol (y=-1.00), le plafond (y=1.10), la face avant et la
    cloison (z=-8.0) est VIDE — seule la vitre du pare-brise ferme l'avant."""
    # Cloison arrière de cabine, rentrée de 5 mm sous le plan des murs (M30.5),
    # enterrée de 10 mm dans la dalle de sol (face de fond hors des plans -8/-12 mm).
    parts[MAT_INTERIOR].add_box(-HALF_W + WALL + 0.005, IN_FLOOR - 0.010, Z_CLOISON,
                                HALF_W - WALL - 0.005, IN_CEIL, Z_CLOISON + 0.08)
    # Pupitre : console posée sur le sol, sous le pare-brise (fond enterré de 12 mm).
    parts[MAT_PUPITRE].add_box(-1.10, IN_FLOOR - 0.012, -9.65, 1.10, -0.30, -8.95)
    # Panneau d'instruments incliné, face au conducteur.
    #
    # M56 — CE N'ÉTAIT PAS UN PANNEAU, C'ÉTAIT UNE BOÎTE PERCÉE. On empilait ici un
    # quad incliné ET un parallélépipède plein occupant tout le volume, privé de son
    # dessus et de son dessous. Trois conséquences, toutes visibles depuis le siège :
    # le plan incliné était noyé dans le bloc (donc invisible), le bloc s'ouvrait vers
    # le haut et vers le bas sur son propre vide, et les deux ÉCRANS de conduite — dont
    # la boîte occupe z de -9,15 à -9,11, strictement à l'intérieur — étaient purement
    # et simplement enterrés dedans.
    #
    # Le pupitre est désormais un vrai PRISME (dièdre) : trois faces plates, deux joues
    # triangulaires, fermé. Les écrans et leur encadrement sont posés SUR la pente,
    # dans son repère (s, t), donc réellement affleurants et réellement visibles.
    build_panel_wedge(parts[MAT_PUPITRE])
    # M53 — CASQUETTE ANTI-REFLET au-dessus des instruments. Toutes les cabines en
    # ont une, pour la même raison : sans elle, le pare-brise renvoie les écrans dans
    # les yeux du conducteur. Ici elle joue en plus un rôle de composition — elle
    # ferme le champ par le haut et donne au poste sa silhouette reconnaissable.
    parts[MAT_PUPITRE].add_box(-1.05, -0.02, -9.30, 1.05, 0.03, -9.06)
    # Deux écrans de conduite (vitesse / ATS) POSÉS SUR LA PENTE, avec leur
    # encadrement : 8 mm de saillie pour le cadre, 12 mm pour la dalle, donc aucune
    # face coplanaire ni avec la pente ni entre elles.
    for x0, x1 in ((-0.45, -0.10), (0.10, 0.45)):
        panel_slab(parts[MAT_PUPITRE], x0 - 0.025, x1 + 0.025, 0.12, 0.88, 0.0, 0.008)
        panel_slab(parts[MAT_ECRAN], x0, x1, 0.18, 0.82, 0.004, 0.012)
    # Bandeau de boutons (portes, klaxon, éclairage) sur le PLAT de la console, en
    # avant du panneau incliné — c'est-à-dire dans la seule zone du pupitre que rien
    # d'autre n'occupe. Les poser entre les deux écrans les aurait mis à fleur de
    # l'encadrement : deux faces exactement coplanaires, donc du z-fighting garanti
    # (tools/check_coplanar.py le refuse, et il a raison).
    for i in range(4):
        bx = 0.18 + i * 0.075
        parts[MAT_COMMANDE].add_box(bx, -0.305, -9.44, bx + 0.045, -0.275, -9.36)
    # MANIPULATEUR UNIQUE (T-handle) à GAUCHE : traction poussé, frein tiré.
    parts[MAT_COMMANDE].add_box(-0.72, -0.30, -9.05, -0.58, -0.24, -8.85)  # embase
    parts[MAT_COMMANDE].add_box(-0.665, -0.24, -8.96, -0.635, -0.02, -8.94)  # tige
    parts[MAT_COMMANDE].add_box(-0.77, -0.04, -8.97, -0.56, 0.00, -8.93)    # barre en T
    # M53 — Console latérale droite (radio, coupure) : le pupitre japonais est en L.
    # x borné à 1,045 : la joue du dièdre est à 1,05, les deux flancs ne sont donc
    # pas coplanaires (le z-fighting que check_coplanar.py signalait ici).
    parts[MAT_PUPITRE].add_box(0.72, -0.30, -9.05, 1.045, -0.16, -8.72)
    for i in range(3):
        parts[MAT_COMMANDE].add_box(0.78 + i * 0.08, -0.16, -8.96,
                                    0.82 + i * 0.08, -0.13, -8.86)
    # Siège conducteur (tête à (0, 0.25, -8.55)).
    parts[MAT_PUPITRE].add_box(-0.25, IN_FLOOR, -8.75, 0.25, -0.55, -8.30)   # socle
    parts[MAT_BENCH].add_box(-0.28, -0.55, -8.80, 0.28, -0.45, -8.25)        # assise
    parts[MAT_BENCH].add_box(-0.26, -0.45, -8.27, 0.26, 0.15, -8.17)         # dossier


def build_door_leaf(steel_part, bande_part, side, z0, z1):
    """Un vantail coulissant (panneau en acier + segment de bande verte M36)."""
    x_out = side * (HALF_W - 0.004)
    x_in = side * (HALF_W - 0.084)
    steel_part.add_box(min(x_out, x_in), DOOR_Y0 + 0.004, z0 + 0.004,
                       max(x_out, x_in), DOOR_Y1 - 0.004, z1 - 0.004)

    # Segment de bande verte fixé sur la face extérieure du vantail (M36)
    bx0, bx1 = sorted((side * (HALF_W - 0.004 + 0.002), side * (HALF_W - 0.004 + 0.014)))
    bande_part.add_box(bx0, WIN_SILL - 0.22, z0 + 0.004,
                       bx1, WIN_SILL - 0.04, z1 - 0.004)


def build_door_parts(parts, doorways):
    """LES 8 DERNIÈRES PARTS du GLB : 4 groupes de battants en acier, puis les 4
    groupes de bande verte correspondants (M36).

    Un GROUPE réunit tous les battants qui bougent ensemble — les 4 vantaux A du
    flanc droit, par exemple. Ordre : droit/A, droit/B, gauche/A, gauche/B. C'est la
    convention d'INDEX que l'app C++ anime, et la seule chose qu'elle sache de ce
    fichier : quatre translations, quatre primitives."""
    for d in range(8):
        side = 1.0 if d < 4 else -1.0
        z0, z1, _, _ = doorways[d % 4]
        zc = 0.5 * (z0 + z1)
        base = 0 if d < 4 else 2          # flanc droit -> groupes 0/1, gauche -> 2/3
        # Vantail A (coulisse vers -z) puis B (vers +z).
        build_door_leaf(parts[MAT_DOOR0 + base], parts[MAT_DOOR_BANDE0 + base], side, z0, zc)
        build_door_leaf(parts[MAT_DOOR0 + base + 1], parts[MAT_DOOR_BANDE0 + base + 1],
                        side, zc, z1)


def build_motrice(out):
    parts = [Part() for _ in MATERIALS]
    build_floor(parts)
    build_side_walls(parts, MOTRICE_OPENINGS, MOTRICE_WINDOWS, MOTRICE_DOORWAYS)
    build_roof(parts, clim_z=CLIM_Z_MOTRICE)
    build_pantograph(parts)
    build_end_face(parts, Z_HEAD, with_windshield=True)
    build_end_face(parts, Z_TAIL, with_windshield=False)
    build_lights(parts)
    build_interior(parts, MOTRICE_CENTERS)
    build_cab(parts)
    # Portes de la motrice ANIMÉES elles aussi (M30.5) : 32 dernières primitives.
    build_door_parts(parts, MOTRICE_DOORWAYS)
    write_glb(out, parts, "E235_motrice")


# ==============================================================================
# VOITURE — 4 doubles portes par face, battants = 32 dernières primitives
# ==============================================================================
def build_voiture(out):
    parts = [Part() for _ in MATERIALS]
    build_floor(parts)
    build_side_walls(parts, OPENINGS, WINDOWS, DOORWAYS)
    build_roof(parts)
    build_end_face(parts, Z_HEAD, with_windshield=False)
    build_end_face(parts, Z_TAIL, with_windshield=False)
    build_interior(parts)
    build_door_parts(parts, DOORWAYS)
    write_glb(out, parts, "E235_voiture")


# ==============================================================================
# BOGIE — origine au plan de roulement, 2 essieux en dernières primitives
# ==============================================================================
WHEEL_R = 0.46          # rayon au cercle de roulement — Ø 920 mm (l'app cale l'essieu
                        # à cette hauteur au-dessus du plan de roulement)
GAUGE_HALF = 0.7175     # demi-écartement, 1435 mm entre faces internes des champignons
FLANGE_R = 0.488        # boudin de 28 mm au-dessus du cercle de roulement (norme UIC)

# M56 — LE BOUDIN ÉTAIT DU MAUVAIS CÔTÉ. L'ancien profil posait la couronne de plus
# grand rayon À L'EXTÉRIEUR de la voie (|x| de 0,785 à 0,810, donc au-delà du champignon
# qui s'arrête à 0,7895) : une roue ainsi taillée déraille au premier aiguillage. Le
# boudin d'un chemin de fer court À L'INTÉRIEUR des rails, c'est lui qui tient l'essieu
# dans la voie. Cotes remises d'aplomb sur la pratique UIC :
#   * écartement intérieur des roues (back-to-back) 1360 mm => face interne à 0,680 ;
#   * boudin de 32,5 mm d'épaisseur, donc de 0,680 à 0,7125 ;
#   * table de roulement de 135 mm de large, de 0,7125 à 0,8475.
# Le cercle de roulement (0,7125..0,8475) recouvre bien le champignon (0,7175..0,7895).
WHEEL_BACK = 0.680      # face interne de la jante
WHEEL_FLANGE_OUT = 0.7125   # fin du boudin / début de la table
WHEEL_FRONT = 0.8475    # face externe de la jante


def revolve(part, profile, sign, segments=24):
    """Solide de révolution autour de l'axe x, à partir d'un PROFIL (x, r) fermé.

    Un profil est une liste de couples (x, rayon) parcourue dans l'ordre ; entre deux
    points consécutifs on engendre soit une couronne (même x), soit un manchon (même r),
    soit un tronc de cône. `sign` = ±1 miroite le profil pour l'autre roue.

    Le point de tout ceci : un profil fermé (premier et dernier point sur l'axe, r = 0)
    donne PAR CONSTRUCTION un volume étanche et d'orientation cohérente. L'ancienne roue
    était assemblée à la main, morceau par morceau — d'où une couronne de boudin dont la
    normale regardait à l'envers et des faces que le back-face culling escamotait."""
    for k in range(len(profile) - 1):
        x0, r0 = profile[k]
        x1, r1 = profile[k + 1]
        if r0 == 0.0 and r1 == 0.0:
            continue
        # Normale de la génératrice, dans le plan (x, r), tournée vers l'EXTÉRIEUR.
        dx, dr = (x1 - x0) * sign, r1 - r0
        ln = math.hypot(dx, dr) or 1.0
        nx, nr = dr / ln, -dx / ln
        for j in range(segments):
            a0 = 2.0 * math.pi * j / segments
            a1 = 2.0 * math.pi * (j + 1) / segments
            ring = []
            for (xx, rr), a in (((x0, r0), a0), ((x0, r0), a1), ((x1, r1), a1), ((x1, r1), a0)):
                p = (sign * xx, rr * math.sin(a), rr * math.cos(a))
                n = (nx, nr * math.sin(a), nr * math.cos(a))
                ring.append((p, n, (a / (2.0 * math.pi), (xx - profile[0][0]) / 0.2),
                             (0.0, 0.0, 1.0, 1.0)))
            # Élimine les triangles dégénérés quand un bout du quad est sur l'axe.
            if r0 == 0.0:
                ring = [ring[0], ring[2], ring[3]]
            elif r1 == 0.0:
                ring = [ring[0], ring[1], ring[2]]
            part.add(ring)


def build_wheel(part, side, segments=24):
    """Une roue monobloc : jante + boudin, profil fermé, boudin CÔTÉ VOIE."""
    revolve(part, [
        (WHEEL_BACK, 0.0),                  # centre de la face interne
        (WHEEL_BACK, FLANGE_R),             # face interne de la jante
        (WHEEL_FLANGE_OUT, FLANGE_R),       # sommet du boudin
        (WHEEL_FLANGE_OUT, WHEEL_R),        # descente du boudin sur la table
        (WHEEL_FRONT, WHEEL_R),             # table de roulement
        (WHEEL_FRONT, 0.0),                 # centre de la face externe
    ], side, segments)


def build_axle(part, segments=12):
    # M56 — L'essieu s'arrêtait 2,5 mm avant la roue ET n'avait AUCUN fond : entre les
    # deux roues on voyait un tuyau ouvert sur le néant. Il pénètre désormais de 2 cm
    # DANS la jante et il est BOUCHÉ — les deux fonds sont noyés dans le moyeu, donc
    # invisibles, et aucune face n'est coplanaire avec la toile de roue.
    shaft_r, shaft_x = 0.085, WHEEL_BACK + 0.02
    for j in range(segments):
        a0 = 2.0 * math.pi * j / segments
        a1 = 2.0 * math.pi * (j + 1) / segments
        n0 = (0.0, math.sin(a0), math.cos(a0))
        n1 = (0.0, math.sin(a1), math.cos(a1))
        tg = (1.0, 0.0, 0.0, 1.0)
        part.add([((-shaft_x, shaft_r * math.sin(a0), shaft_r * math.cos(a0)), n0, (0, 0), tg),
                  ((shaft_x, shaft_r * math.sin(a0), shaft_r * math.cos(a0)), n0, (1, 0), tg),
                  ((shaft_x, shaft_r * math.sin(a1), shaft_r * math.cos(a1)), n1, (1, 1), tg),
                  ((-shaft_x, shaft_r * math.sin(a1), shaft_r * math.cos(a1)), n1, (0, 1), tg)])
    # Fonds de l'arbre (éventail depuis le centre) : la normale sort de l'essieu.
    for sx in (-1.0, 1.0):
        cen = (sx * shaft_x, 0.0, 0.0)
        nx = (sx, 0.0, 0.0)
        for j in range(segments):
            a0 = 2.0 * math.pi * j / segments
            a1 = 2.0 * math.pi * (j + 1) / segments
            e0 = (sx * shaft_x, shaft_r * math.sin(a0), shaft_r * math.cos(a0))
            e1 = (sx * shaft_x, shaft_r * math.sin(a1), shaft_r * math.cos(a1))
            part.add([(cen, nx, (0.5, 0.5), (0, 0, 1, 1)),
                      (e0, nx, (0, 0), (0, 0, 1, 1)),
                      (e1, nx, (1, 0), (0, 0, 1, 1))])
    for side in (-1.0, 1.0):
        build_wheel(part, side)


def build_bogie(out):
    parts = [Part() for _ in MATERIALS]
    parts[MAT_BOGIE].add_box(-0.45, WHEEL_R + 0.10, -1.9, 0.45, WHEEL_R + 0.85, 1.9)
    build_axle(parts[MAT_AXLE_A])   # l'app le place à z = -1.5
    build_axle(parts[MAT_AXLE_B])   # l'app le place à z = +1.5
    write_glb(out, parts, "E235_bogie")


# ==============================================================================
# SÉRIALISATION GLTF BINAIRE (.GLB) — une primitive par part non vide
# ==============================================================================
def align4(n):
    return (n + 3) & ~3


def write_glb(path, parts, node_name):
    used = [(i, p) for i, p in enumerate(parts) if p.positions]
    flipped = sum(p.orient() for _, p in used)
    if flipped:
        print(f"  {path} : {flipped} triangle(s) réorientés (winding glTF/CCW)")
    blocks, part_blocks = [], []
    for _, p in used:
        b = (b"".join(struct.pack("<fff", *v) for v in p.positions),
             b"".join(struct.pack("<fff", *v) for v in p.normals),
             b"".join(struct.pack("<ff", *v) for v in p.uvs),
             b"".join(struct.pack("<ffff", *v) for v in p.tangents),
             b"".join(struct.pack("<I", i) for i in p.indices))
        part_blocks.append(b)
        blocks.extend(b)
    offsets, cur = [], 0
    for blk in blocks:
        offsets.append(cur)
        cur = align4(cur + len(blk))
    total = cur
    bin_data = bytearray(total)
    for off, blk in zip(offsets, blocks):
        bin_data[off:off + len(blk)] = blk

    accessors, buffer_views, primitives = [], [], []
    for slot, (mat_idx, p) in enumerate(used):
        base = 5 * slot
        pmin = [min(v[k] for v in p.positions) for k in range(3)]
        pmax = [max(v[k] for v in p.positions) for k in range(3)]
        accessors += [
            {"bufferView": base, "componentType": 5126, "count": len(p.positions), "type": "VEC3",
             "min": pmin, "max": pmax},
            {"bufferView": base + 1, "componentType": 5126, "count": len(p.normals), "type": "VEC3"},
            {"bufferView": base + 2, "componentType": 5126, "count": len(p.uvs), "type": "VEC2"},
            {"bufferView": base + 3, "componentType": 5126, "count": len(p.tangents), "type": "VEC4"},
            {"bufferView": base + 4, "componentType": 5125, "count": len(p.indices), "type": "SCALAR"},
        ]
        targets = [34962, 34962, 34962, 34962, 34963]
        for k in range(5):
            buffer_views.append({"buffer": 0, "byteOffset": offsets[base + k],
                                 "byteLength": len(part_blocks[slot][k]), "target": targets[k]})
        primitives.append({"attributes": {"POSITION": base, "NORMAL": base + 1,
                                          "TEXCOORD_0": base + 2, "TANGENT": base + 3},
                           "indices": base + 4, "material": mat_idx})
    # Matériaux DÉDUPLIQUÉS : les 16 battants de porte partagent UN matériau
    # (le pool de descriptor sets du renderer est limité à 64 — M30.5).
    # L'animation repose sur les indices de PRIMITIVES, pas sur les matériaux.
    materials, mat_remap = [], {}
    # Images/textures DÉDUPLIQUÉES par URI : les battants et la carrosserie partagent
    # le même inox, il ne doit être déclaré qu'une fois (et donc décodé et téléversé
    # qu'une fois côté moteur — le cache du chargeur est indexé par image glTF).
    images, textures, tex_remap = [], [], {}

    def tex_slot(uri):
        if uri not in tex_remap:
            tex_remap[uri] = len(textures)
            images.append({"uri": uri})
            textures.append({"source": len(images) - 1, "sampler": 0})
        return tex_remap[uri]

    def mat_slot(mat_idx):
        mat_def = MATERIALS[mat_idx]
        key = (mat_def["name"], tuple(mat_def["factor"]), mat_def["metallic"],
               mat_def["roughness"], mat_def.get("blend"), mat_def.get("doubleSided"),
               tuple(mat_def["emissive"]) if mat_def.get("emissive") else None,
               mat_def.get("diff"), mat_def.get("arm"), mat_def.get("nor"))
        if key not in mat_remap:
            mat_remap[key] = len(materials)
            pbr = {"baseColorFactor": mat_def["factor"],
                   "metallicFactor": mat_def["metallic"],
                   "roughnessFactor": mat_def["roughness"]}
            if mat_def.get("diff"):
                pbr["baseColorTexture"] = {"index": tex_slot(mat_def["diff"])}
            if mat_def.get("arm"):
                # _arm = AO/roughness/metallic en R/G/B : exactement la convention
                # glTF metallic-roughness (G = rough, B = metal). Aucun repack.
                pbr["metallicRoughnessTexture"] = {"index": tex_slot(mat_def["arm"])}
            m = {"name": mat_def["name"], "pbrMetallicRoughness": pbr}
            if mat_def.get("nor"):
                m["normalTexture"] = {"index": tex_slot(mat_def["nor"])}
            # Facteur émissif HDR (valeurs > 1 tolérées par le loader, M48).
            if mat_def.get("emissive"):
                m["emissiveFactor"] = mat_def["emissive"]
            if mat_def.get("blend"):
                m["alphaMode"] = "BLEND"
                m["doubleSided"] = True
            elif mat_def.get("doubleSided"):
                m["doubleSided"] = True
            materials.append(m)
        return mat_remap[key]

    for prim in primitives:
        prim["material"] = mat_slot(prim["material"])

    gltf = {"asset": {"version": "2.0", "generator": "noire-metro-e235 (CC0)"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": node_name}],
            "meshes": [{"primitives": primitives}], "materials": materials,
            "accessors": accessors, "bufferViews": buffer_views, "buffers": [{"byteLength": total}]}
    if textures:
        # Filtrage linéaire + REPEAT : les UV sont métriques, elles dépassent
        # allègrement 1 sur une caisse de 20 m — sans REPEAT, tout serait étiré au
        # dernier texel du bord.
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497}]
        gltf["images"] = images
        gltf["textures"] = textures
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))
    bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)
    glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, glb_len))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_pad), 0x004E4942))
        f.write(bin_pad)
    nv = sum(len(p.positions) for _, p in used)
    print(f"{path} : {nv} sommets, {len(used)} primitives, {glb_len} o ({node_name})")


def _default_models_dir():
    """M56 — Par défaut, on écrit DANS assets/models, pas dans le répertoire courant.

    Le README documente `python3 tools/gen_metro.py` comme la façon de régénérer les
    modèles ; avec l'ancien défaut (« . ») la commande déposait les .glb à la racine du
    dépôt et le jeu continuait de charger les anciens. Un générateur dont la sortie par
    défaut n'est pas l'endroit où le moteur va lire est un piège, pas une commodité."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "models")


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else _default_models_dir()
    build_motrice(f"{outdir}/metro_motrice.glb")
    build_voiture(f"{outdir}/metro_voiture.glb")
    build_bogie(f"{outdir}/metro_bogie.glb")
