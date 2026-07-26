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
    face. Les 16 battants sont les 16 DERNIÈRES primitives du GLB (embrasures
    0-3 flanc droit x>0, 4-7 flanc gauche ; paire = vantail A puis B), convention
    que l'app anime (bouchon latéral puis coulissement).
  * metro_bogie.glb : bogie, origine au plan de roulement ; les 2 dernières
    primitives sont les essieux centrés à l'origine (roues Ø920, voie 1435 mm).

Intérieur de métro : BANQUETTES LONGITUDINALES le long des murs, grand espace
vide au centre, vrai sol, vrai plafond.

Carrosserie : ACIER INOXYDABLE BROSSÉ (métallique) + bande verte Yamanote.

Repère caisse : x = droite, y = haut, z = arrière ; rail à y = -2.20.
Aucune dépendance externe (stdlib seule)."""

import struct
import json
import math
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

# Pare-brise de la motrice : énorme trou carré dans la face avant.
WS_X, WS_Y0, WS_Y1 = 1.20, -0.20, 1.10

# Cabine : tête conducteur (0, 0.25, -8.55), cloison à z = -8.0.
Z_CLOISON = -8.0

# --- Matériaux PBR --------------------------------------------------------------
MATERIALS = [
    # 0: acier — inoxydable brossé (carrosserie métallique)
    {"name": "acier", "factor": [0.72, 0.74, 0.77, 1.0], "metallic": 0.90, "roughness": 0.38},
    # 1: vitrage — verre PBR, transparence absolue (cahier des charges M30)
    {"name": "vitrage", "factor": [0.1, 0.1, 0.1, 0.3], "metallic": 0.0, "roughness": 0.02,
     "blend": True, "doubleSided": True},
    # 2: bande — vert vif Yamanote (#6CBB5A)
    {"name": "bande", "factor": [0.42, 0.73, 0.35, 1.0], "metallic": 0.20, "roughness": 0.40},
    # 3: jupe — équipement sous caisse, gris foncé mat
    {"name": "jupe", "factor": [0.16, 0.17, 0.18, 1.0], "metallic": 0.10, "roughness": 0.75},
    # 4: interieur — mélamine claire, double face
    {"name": "interieur", "factor": [0.80, 0.80, 0.78, 1.0], "metallic": 0.0, "roughness": 0.85,
     "doubleSided": True},
    # 5: banquette — moquette bleue
    {"name": "banquette", "factor": [0.10, 0.16, 0.45, 1.0], "metallic": 0.0, "roughness": 0.95},
    # 6: pupitre — console gris foncé mat
    {"name": "pupitre", "factor": [0.14, 0.15, 0.17, 1.0], "metallic": 0.0, "roughness": 0.75},
    # 7: ecran — écrans de conduite bleutés
    {"name": "ecran", "factor": [0.05, 0.15, 0.30, 1.0], "metallic": 0.0, "roughness": 0.10},
    # 8: commande — manipulateur / boutons
    {"name": "commande", "factor": [0.75, 0.20, 0.15, 1.0], "metallic": 0.40, "roughness": 0.35},
    # 9: phare — bloc optique avant, blanc chaud quasi émissif
    {"name": "phare", "factor": [1.0, 0.95, 0.80, 1.0], "metallic": 0.0, "roughness": 0.20},
]
MAT_ACIER, MAT_GLASS, MAT_BANDE, MAT_JUPE, MAT_INTERIOR = 0, 1, 2, 3, 4
MAT_BENCH, MAT_PUPITRE, MAT_ECRAN, MAT_COMMANDE, MAT_PHARE = 5, 6, 7, 8, 9
# 10..25 : portes — 16 exemplaires du MÊME matériau, car chaque battant est une
# part séparée (une part = une primitive = un slot matériau).
for _ in range(16):
    MATERIALS.append({"name": "porte", "factor": [0.72, 0.74, 0.77, 1.0],
                      "metallic": 0.90, "roughness": 0.38})
MAT_DOOR0 = 10
# 26..28 : bogie + 2 essieux (parts séparées, les 2 dernières du GLB bogie).
MATERIALS.append({"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65})
MATERIALS.append({"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30})
MATERIALS.append({"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30})
MAT_BOGIE, MAT_AXLE_A, MAT_AXLE_B = 26, 27, 28

# Embrasures de portes (z0, z1, y0, y1) — une face, l'autre est symétrique.
DOORWAYS = [(zc - DOOR_HALF, zc + DOOR_HALF, DOOR_Y0, DOOR_Y1) for zc in DOOR_CENTERS]

# Baies vitrées latérales : une fenêtre entre chaque paire de portes et aux bouts.
def side_openings():
    """Ouvertures d'un mur latéral : portes + fenêtres entre les portes."""
    openings = list(DOORWAYS)
    spans = [(Z_HEAD + 0.5, DOOR_CENTERS[0] - DOOR_HALF)]
    for a, b in zip(DOOR_CENTERS, DOOR_CENTERS[1:]):
        spans.append((a + DOOR_HALF, b - DOOR_HALF))
    spans.append((DOOR_CENTERS[-1] + DOOR_HALF, Z_TAIL - 0.5))
    for za, zb in spans:
        if zb - za > 0.3:
            openings.append((za + 0.15, zb - 0.15, WIN_SILL, WIN_LINTEL))
    return openings


OPENINGS = side_openings()
WINDOWS = [o for o in OPENINGS if o[2] == WIN_SILL]


# --- Algèbre --------------------------------------------------------------------
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def mul(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 1.0, 0.0)


class Part:
    """Une part = un matériau = une primitive glTF."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

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
        tan = norm(sub(p1, p0))
        tg = (tan[0], tan[1], tan[2], 1.0)
        self.add([(p0, n, (0, 0), tg), (p1, n, (1, 0), tg),
                  (p2, n, (1, 1), tg), (p3, n, (0, 1), tg)])

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
    """Plancher + jupe d'équipement sous caisse."""
    parts[MAT_JUPE].add_box(-HALF_W, BODY_BOT - 0.15, Z_HEAD + 0.2,
                            HALF_W, IN_FLOOR, Z_TAIL - 0.2)
    parts[MAT_JUPE].add_box(-HALF_W + 0.10, RAIL + 0.35, Z_HEAD + 1.2,
                            HALF_W - 0.10, BODY_BOT - 0.15, Z_TAIL - 1.2)


def build_side_walls(parts):
    """Murs latéraux avec fenêtres et portes DÉCOUPÉES, verre dans les baies."""
    for side in (-1.0, 1.0):
        xa, xb = sorted((side * HALF_W, side * (HALF_W - WALL)))
        wall_with_openings(parts[MAT_ACIER], xa, xb, Z_HEAD, Z_TAIL,
                           BODY_BOT, ROOF_Y, OPENINGS)
        for (z0, z1, y0, y1) in WINDOWS:
            parts[MAT_GLASS].add_box(xa - 0.005, y0, z0, xb + 0.005, y1, z1,
                                     top=False, bot=False, front=False, back=False)
        # Bande verte Yamanote plaquée sous le bandeau vitré.
        x0, x1 = sorted((side * HALF_W, side * (HALF_W + 0.012)))
        parts[MAT_BANDE].add_box(x0, WIN_SILL - 0.22, Z_HEAD + 0.3,
                                 x1, WIN_SILL - 0.04, Z_TAIL - 0.3)


def build_roof(parts):
    """Toit plat indépendant + climatiseurs."""
    parts[MAT_ACIER].add_box(-HALF_W, ROOF_Y, Z_HEAD, HALF_W, ROOF_Y + 0.10, Z_TAIL)
    for zc in (-5.0, 0.0, 5.0):
        parts[MAT_JUPE].add_box(-0.80, ROOF_Y + 0.10, zc - 1.2, 0.80, ROOF_Y + 0.38, zc + 1.2)


def build_interior(parts):
    """Sol, plafond, et BANQUETTES LONGITUDINALES le long des murs (grand espace
    vide au centre — c'est un métro)."""
    parts[MAT_INTERIOR].add_box(-HALF_W + WALL, IN_FLOOR - 0.02, Z_HEAD + 0.1,
                                HALF_W - WALL, IN_FLOOR, Z_TAIL - 0.1)
    parts[MAT_INTERIOR].add_box(-HALF_W + WALL, IN_CEIL, Z_HEAD + 0.1,
                                HALF_W - WALL, IN_CEIL + 0.05, Z_TAIL - 0.1)
    # Banquettes : assise à 45 cm du sol, adossée au mur, entre les portes.
    spans = [(Z_HEAD + 0.3, DOOR_CENTERS[0] - DOOR_HALF)]
    for a, b in zip(DOOR_CENTERS, DOOR_CENTERS[1:]):
        spans.append((a + DOOR_HALF, b - DOOR_HALF))
    spans.append((DOOR_CENTERS[-1] + DOOR_HALF, Z_TAIL - 0.3))
    for side in (-1.0, 1.0):
        x_wall = side * (HALF_W - WALL)
        x_edge = side * (HALF_W - WALL - 0.45)
        xa, xb = sorted((x_wall, x_edge))
        for za, zb in spans:
            if zb - za < 0.4:
                continue
            parts[MAT_BENCH].add_box(xa, IN_FLOOR, za + 0.1, xb, IN_FLOOR + 0.45, zb - 0.1)
            # dossier contre le mur
            xd0, xd1 = sorted((x_wall, x_wall - side * 0.12))
            parts[MAT_BENCH].add_box(xd0, IN_FLOOR + 0.45, za + 0.1,
                                     xd1, IN_FLOOR + 0.95, zb - 0.1)
    # Barres verticales au centre de chaque embrasure (poteaux d'accès).
    for zc in DOOR_CENTERS:
        parts[MAT_INTERIOR].add_box(-0.03, IN_FLOOR, zc - 0.03, 0.03, IN_CEIL, zc + 0.03)


def build_end_face(parts, zend, with_windshield):
    """Face d'extrémité. Motrice : ÉNORME trou carré de pare-brise + phares."""
    z0, z1 = sorted((zend, zend + (0.10 if zend < 0 else -0.10)))
    if not with_windshield:
        parts[MAT_ACIER].add_box(-HALF_W, BODY_BOT, z0, HALF_W, ROOF_Y, z1)
        return
    # Piliers latéraux, bande basse, bande haute : le trou est RÉEL.
    parts[MAT_ACIER].add_box(-HALF_W, BODY_BOT, z0, -WS_X, ROOF_Y, z1)
    parts[MAT_ACIER].add_box(WS_X, BODY_BOT, z0, HALF_W, ROOF_Y, z1)
    parts[MAT_ACIER].add_box(-WS_X, BODY_BOT, z0, WS_X, WS_Y0, z1)
    parts[MAT_ACIER].add_box(-WS_X, WS_Y1, z0, WS_X, ROOF_Y, z1)
    # Verre du pare-brise, à fleur de la face (z = zend).
    parts[MAT_GLASS].add_box(-WS_X, WS_Y0, zend - 0.01, WS_X, WS_Y1, zend + 0.01,
                             top=False, bot=False, left=False, right=False)
    # Phares : deux blocs sur les piliers, HORS de l'ouverture (|x| > WS_X).
    for sx in (-1.0, 1.0):
        parts[MAT_PHARE].add_box(sx * 1.34 - 0.10, WS_Y0 + 0.05, zend - 0.03,
                                 sx * 1.34 + 0.10, WS_Y0 + 0.30, zend + 0.01)


def build_static_doors(parts):
    """Battants FERMÉS affleurants (motrice : portes voyageurs derrière la cabine,
    jamais animées par l'app)."""
    for side in (-1.0, 1.0):
        x_out = side * (HALF_W - 0.004)
        x_in = side * (HALF_W - 0.084)
        xa, xb = sorted((x_out, x_in))
        for (z0, z1, y0, y1) in DOORWAYS:
            zc = 0.5 * (z0 + z1)
            parts[MAT_ACIER].add_box(xa, y0 + 0.004, z0 + 0.004, xb, y1 - 0.004, zc)
            parts[MAT_ACIER].add_box(xa, y0 + 0.004, zc, xb, y1 - 0.004, z1 - 0.004)


# ==============================================================================
# MOTRICE — cabine + pupitre japonais (T-handle) + siège
# ==============================================================================
def build_cab(parts):
    """Cabine : cloison, pupitre avec manipulateur T à gauche, écrans, siège.
    Le volume entre le sol (y=-1.00), le plafond (y=1.10), la face avant et la
    cloison (z=-8.0) est VIDE — seule la vitre du pare-brise ferme l'avant."""
    # Cloison arrière de cabine (avec une petite fenêtre de vigilance).
    parts[MAT_INTERIOR].add_box(-HALF_W + WALL, IN_FLOOR, Z_CLOISON,
                                HALF_W - WALL, IN_CEIL, Z_CLOISON + 0.08)
    # Pupitre : console posée sur le sol, sous le pare-brise.
    parts[MAT_PUPITRE].add_box(-1.10, IN_FLOOR, -9.65, 1.10, -0.30, -8.95)
    # Panneau d'instruments incliné, face au conducteur.
    quad_orient(parts[MAT_PUPITRE], (-1.05, -0.30, -8.98), (1.05, -0.30, -8.98),
                (1.05, -0.02, -9.22), (-1.05, -0.02, -9.22), (0.0, 0.7, 0.7))
    parts[MAT_PUPITRE].add_box(-1.05, -0.30, -9.22, 1.05, -0.02, -8.98, top=False)
    # Deux écrans de conduite (vitesse / ATS) sur le panneau.
    for x0, x1 in ((-0.45, -0.10), (0.10, 0.45)):
        parts[MAT_ECRAN].add_box(x0, -0.24, -9.13, x1, -0.06, -9.09)
    # MANIPULATEUR UNIQUE (T-handle) à GAUCHE : traction poussé, frein tiré.
    parts[MAT_COMMANDE].add_box(-0.72, -0.30, -9.05, -0.58, -0.24, -8.85)  # embase
    parts[MAT_COMMANDE].add_box(-0.665, -0.24, -8.96, -0.635, -0.02, -8.94)  # tige
    parts[MAT_COMMANDE].add_box(-0.77, -0.04, -8.97, -0.56, 0.00, -8.93)    # barre en T
    # Siège conducteur (tête à (0, 0.25, -8.55)).
    parts[MAT_PUPITRE].add_box(-0.25, IN_FLOOR, -8.75, 0.25, -0.55, -8.30)   # socle
    parts[MAT_BENCH].add_box(-0.28, -0.55, -8.80, 0.28, -0.45, -8.25)        # assise
    parts[MAT_BENCH].add_box(-0.26, -0.45, -8.27, 0.26, 0.15, -8.17)         # dossier


def build_motrice(out):
    parts = [Part() for _ in MATERIALS]
    build_floor(parts)
    build_side_walls(parts)
    build_roof(parts)
    build_end_face(parts, Z_HEAD, with_windshield=True)
    build_end_face(parts, Z_TAIL, with_windshield=False)
    build_interior(parts)
    build_static_doors(parts)
    build_cab(parts)
    write_glb(out, parts, "E235_motrice")


# ==============================================================================
# VOITURE — 4 doubles portes par face, battants = 16 dernières primitives
# ==============================================================================
def build_door_leaf(part, side, z0, z1):
    """Un vantail coulissant, AFFLEURANT fermé (rentré de 4 mm)."""
    x_out = side * (HALF_W - 0.004)
    x_in = side * (HALF_W - 0.084)
    part.add_box(min(x_out, x_in), DOOR_Y0 + 0.004, z0 + 0.004,
                 max(x_out, x_in), DOOR_Y1 - 0.004, z1 - 0.004)


def build_voiture(out):
    parts = [Part() for _ in MATERIALS]
    build_floor(parts)
    build_side_walls(parts)
    build_roof(parts)
    build_end_face(parts, Z_HEAD, with_windshield=False)
    build_end_face(parts, Z_TAIL, with_windshield=False)
    build_interior(parts)
    # LES 16 DERNIÈRES PARTS : battants. Embrasures 0-3 = flanc droit (x>0),
    # 4-7 = flanc gauche ; paire = vantail A (coulisse vers -z) puis B (vers +z).
    for d in range(8):
        side = 1.0 if d < 4 else -1.0
        z0, z1, _, _ = DOORWAYS[d % 4]
        zc = 0.5 * (z0 + z1)
        build_door_leaf(parts[MAT_DOOR0 + 2 * d], side, z0, zc)       # vantail A
        build_door_leaf(parts[MAT_DOOR0 + 2 * d + 1], side, zc, z1)   # vantail B
    write_glb(out, parts, "E235_voiture")


# ==============================================================================
# BOGIE — origine au plan de roulement, 2 essieux en dernières primitives
# ==============================================================================
WHEEL_R = 0.46          # Ø 920 mm
GAUGE_HALF = 0.7175     # 1435 mm
FLANGE_R = 0.505


def build_wheel(part, cx, segments=24):
    inner = -1.0 if cx > 0.0 else 1.0
    xo = cx + inner * 0.0675
    xi = cx - inner * 0.0675
    xf = xi - inner * 0.025

    def pt(x, r, a):
        return (x, r * math.sin(a), r * math.cos(a))

    for j in range(segments):
        a0 = 2.0 * math.pi * j / segments
        a1 = 2.0 * math.pi * (j + 1) / segments
        n0 = (0.0, math.sin(a0), math.cos(a0))
        n1 = (0.0, math.sin(a1), math.cos(a1))
        tg = (1.0, 0.0, 0.0, 1.0)
        part.add([(pt(xo, WHEEL_R, a0), n0, (0, 0), tg), (pt(xi, WHEEL_R, a0), n0, (1, 0), tg),
                  (pt(xi, WHEEL_R, a1), n1, (1, 1), tg), (pt(xo, WHEEL_R, a1), n1, (0, 1), tg)])
        part.add([(pt(xi, FLANGE_R, a0), n0, (0, 0), tg), (pt(xf, FLANGE_R, a0), n0, (1, 0), tg),
                  (pt(xf, FLANGE_R, a1), n1, (1, 1), tg), (pt(xi, FLANGE_R, a1), n1, (0, 1), tg)])
        na = (-inner, 0.0, 0.0)
        ring = [(pt(xi, WHEEL_R, a0), na, (0, 0), (0, 0, 1, 1)),
                (pt(xi, WHEEL_R, a1), na, (1, 0), (0, 0, 1, 1)),
                (pt(xi, FLANGE_R, a1), na, (1, 1), (0, 0, 1, 1)),
                (pt(xi, FLANGE_R, a0), na, (0, 1), (0, 0, 1, 1))]
        if inner < 0.0:
            ring.reverse()
        part.add(ring)
        for fx, fr, nx in ((xo, WHEEL_R, inner), (xf, FLANGE_R, -inner)):
            cen = (fx, 0.0, 0.0)
            e0, e1 = pt(fx, fr, a0), pt(fx, fr, a1)
            a, b = (e0, e1) if nx > 0 else (e1, e0)
            part.add([(cen, (nx, 0, 0), (0, 0), (0, 0, 1, 1)),
                      (a, (nx, 0, 0), (1, 0), (0, 0, 1, 1)),
                      (b, (nx, 0, 0), (0, 1), (0, 0, 1, 1))])


def build_axle(part, segments=12):
    shaft_r, shaft_x = 0.085, GAUGE_HALF - 0.07
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
    for cx in (-GAUGE_HALF, GAUGE_HALF):
        build_wheel(part, cx)


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
                           "indices": base + 4, "material": slot})
    materials = []
    for mat_idx, _ in used:
        mat_def = MATERIALS[mat_idx]
        m = {"name": mat_def["name"],
             "pbrMetallicRoughness": {"baseColorFactor": mat_def["factor"],
                                      "metallicFactor": mat_def["metallic"],
                                      "roughnessFactor": mat_def["roughness"]}}
        if mat_def.get("blend"):
            m["alphaMode"] = "BLEND"
            m["doubleSided"] = True
        elif mat_def.get("doubleSided"):
            m["doubleSided"] = True
        materials.append(m)

    gltf = {"asset": {"version": "2.0", "generator": "noire-metro-e235 (CC0)"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": node_name}],
            "meshes": [{"primitives": primitives}], "materials": materials,
            "accessors": accessors, "bufferViews": buffer_views, "buffers": [{"byteLength": total}]}
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


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    build_motrice(f"{outdir}/metro_motrice.glb")
    build_voiture(f"{outdir}/metro_voiture.glb")
    build_bogie(f"{outdir}/metro_bogie.glb")
