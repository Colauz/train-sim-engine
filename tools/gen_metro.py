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

M50 : 1 bloc de climatisation gris clair par voiture (4,0 x 2,0 x 0,4 m, centré
sur le toit ; décalé à z = +5 sur la motrice, où le pantographe occupe déjà le
milieu). M48 : pantographe en
losange sur la motrice, phares blancs et feux rouges ÉMISSIFS HDR aux DEUX
extrémités de la motrice (rame réversible). Toute cette géométrie vit dans des
parts dont le matériau précède MAT_DOOR0 : les 32 battants de porte restent les
32 DERNIÈRES primitives, et les 2 essieux les 2 dernières du bogie.

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

# Climatisation de toit (M50) : un bloc par voiture, 4,0 x 2,0 x 0,4 m.
CLIM_HALF_L = 2.0       # 4,0 m dans le sens de la marche
CLIM_HALF_W = 1.0       # 2,0 m en travers (toit large de 2,95 m)
CLIM_H = 0.40           # hauteur au-dessus de la tôle de toiture
CLIM_Z_MOTRICE = 5.0    # décalé vers l'arrière : le pantographe tient le milieu

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
    # 9: phare — bloc optique avant, blanc chaud ÉMISSIF HDR (M48, facteur > 1 :
    # le loader accepte les valeurs hors spec, cf. asset_types.hpp)
    {"name": "phare", "factor": [1.0, 0.95, 0.80, 1.0], "metallic": 0.0, "roughness": 0.20,
     "emissive": [4.0, 3.8, 3.2]},
    # 10: feu_ar — feu rouge arrière ÉMISSIF HDR (M48)
    {"name": "feu_ar", "factor": [0.30, 0.02, 0.02, 1.0], "metallic": 0.0, "roughness": 0.30,
     "emissive": [4.0, 0.10, 0.05]},
    # 11: clim — boîtiers de climatisation de toit, gris clair mat (M48)
    {"name": "clim", "factor": [0.70, 0.71, 0.72, 1.0], "metallic": 0.05, "roughness": 0.80},
    # 12: panto — pantographe, acier sombre métallique (M48)
    {"name": "panto", "factor": [0.10, 0.10, 0.11, 1.0], "metallic": 0.85, "roughness": 0.40},
]
MAT_ACIER, MAT_GLASS, MAT_BANDE, MAT_JUPE, MAT_INTERIOR = 0, 1, 2, 3, 4
MAT_BENCH, MAT_PUPITRE, MAT_ECRAN, MAT_COMMANDE, MAT_PHARE = 5, 6, 7, 8, 9
MAT_FEU_AR, MAT_CLIM, MAT_PANTO = 10, 11, 12
# 13..28 : portes — 16 slots pour que chaque battant soit une part (= une
# primitive) séparée ; write_glb les DÉDUPLIQUE en un seul matériau glTF.
for _ in range(16):
    MATERIALS.append({"name": "porte", "factor": [0.72, 0.74, 0.77, 1.0],
                      "metallic": 0.90, "roughness": 0.38})
MAT_DOOR0 = 13
# 29..44 : bande verte sur les 16 battants de porte (M36)
for _ in range(16):
    MATERIALS.append({"name": "bande", "factor": [0.42, 0.73, 0.35, 1.0],
                      "metallic": 0.20, "roughness": 0.40})
MAT_DOOR_BANDE0 = 29
# 45..47 : bogie + 2 essieux (parts séparées, les 3 dernières du GLB bogie).
MATERIALS.append({"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65})
MATERIALS.append({"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30})
MATERIALS.append({"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30})
MAT_BOGIE, MAT_AXLE_A, MAT_AXLE_B = 45, 46, 47

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
    Utilisée pour le pantographe (M48)."""
    d = norm(sub(b, a))
    ref = (0.0, 1.0, 0.0) if abs(d[1]) < 0.9 else (1.0, 0.0, 0.0)
    u = mul(norm(cross(d, ref)), w * 0.5)
    v = mul(norm(cross(d, norm(cross(d, ref)))), w * 0.5)
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
    parts[MAT_INTERIOR].add_box(-slab, IN_FLOOR - 0.015, Z_HEAD + 0.1,
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


def build_end_face(parts, zend, with_windshield):
    """Face d'extrémité, plaquée 10 cm VERS L'EXTÉRIEUR de la caisse : sa face
    intérieure (z = zend) est dos-à-dos avec les faces de bout des murs et du
    toit (normales opposées, jamais rasterisées ensemble) au lieu d'être
    coplanaire avec elles (anti z-fighting, M30.5). Les faces latérales et
    supérieures des boîtes sont SUPPRIMÉES quand elles tomberaient pile dans le
    plan des murs latéraux ou du toit. Motrice : ÉNORME trou carré de pare-brise."""
    z0, z1 = sorted((zend, zend + (-0.10 if zend < 0 else 0.10)))
    if not with_windshield:
        parts[MAT_ACIER].add_box(-HALF_W, BODY_BOT, z0, HALF_W, ROOF_Y, z1,
                                 top=False, left=False, right=False)
        return
    # Piliers latéraux, bande basse, bande haute : le trou est RÉEL.
    parts[MAT_ACIER].add_box(-HALF_W, BODY_BOT, z0, -WS_X, ROOF_Y, z1,
                             top=False, left=False)
    parts[MAT_ACIER].add_box(WS_X, BODY_BOT, z0, HALF_W, ROOF_Y, z1,
                             top=False, right=False)
    parts[MAT_ACIER].add_box(-WS_X, BODY_BOT, z0, WS_X, WS_Y0, z1,
                             left=False, right=False)
    parts[MAT_ACIER].add_box(-WS_X, WS_Y1, z0, WS_X, ROOF_Y, z1,
                             top=False, left=False, right=False)
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
    # Panneau d'instruments incliné, face au conducteur (faces cachées omises :
    # le fond du panneau serait coplanaire avec le dessus de la console, M30.5).
    quad_orient(parts[MAT_PUPITRE], (-1.05, -0.30, -8.98), (1.05, -0.30, -8.98),
                (1.05, -0.02, -9.22), (-1.05, -0.02, -9.22), (0.0, 0.7, 0.7))
    parts[MAT_PUPITRE].add_box(-1.05, -0.30, -9.22, 1.05, -0.02, -8.98,
                               top=False, bot=False)
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
    """LES 32 DERNIÈRES PARTS du GLB : battants (16 acier + 16 bande verte M36).
    Embrasures 0-3 = flanc droit (x>0), 4-7 = flanc gauche ; paire = vantail A (coulisse vers -z)
    puis B (vers +z). Convention INDEX que l'app C++ anime."""
    for d in range(8):
        side = 1.0 if d < 4 else -1.0
        z0, z1, _, _ = doorways[d % 4]
        zc = 0.5 * (z0 + z1)
        # Vantail A
        build_door_leaf(parts[MAT_DOOR0 + 2 * d], parts[MAT_DOOR_BANDE0 + 2 * d], side, z0, zc)
        # Vantail B
        build_door_leaf(parts[MAT_DOOR0 + 2 * d + 1], parts[MAT_DOOR_BANDE0 + 2 * d + 1], side, zc, z1)


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
                           "indices": base + 4, "material": mat_idx})
    # Matériaux DÉDUPLIQUÉS : les 16 battants de porte partagent UN matériau
    # (le pool de descriptor sets du renderer est limité à 64 — M30.5).
    # L'animation repose sur les indices de PRIMITIVES, pas sur les matériaux.
    materials, mat_remap = [], {}

    def mat_slot(mat_idx):
        mat_def = MATERIALS[mat_idx]
        key = (mat_def["name"], tuple(mat_def["factor"]), mat_def["metallic"],
               mat_def["roughness"], mat_def.get("blend"), mat_def.get("doubleSided"),
               tuple(mat_def["emissive"]) if mat_def.get("emissive") else None)
        if key not in mat_remap:
            mat_remap[key] = len(materials)
            m = {"name": mat_def["name"],
                 "pbrMetallicRoughness": {"baseColorFactor": mat_def["factor"],
                                          "metallicFactor": mat_def["metallic"],
                                          "roughnessFactor": mat_def["roughness"]}}
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
