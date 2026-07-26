#!/usr/bin/env python3
"""Voiture voyageurs TGV V3.0 et bogie Jacobs — REBOOT TOTAL (M30), panneaux.

Même philosophie modulaire que la motrice (tools/gen_tgv_procedural.py) :
plus de caisse-tube loftée, un ASSEMBLAGE de pièces distinctes :
  * build_floor()        : dalle de plancher + jupe de bas de caisse ;
  * build_side_walls()   : murs latéraux avec baies vitrées ET porte DÉCOUPÉES
                           (trous physiques dans la tôle) ;
  * build_roof()         : toit bombé indépendant (3 pans) ;
  * build_end_caps()     : obturations des bouts + soufflets d'intercirculation ;
  * build_interior()     : plancher, plafond et murs intérieurs ;
  * build_seats()        : rangées de sièges 2+2 de part et d'autre du couloir
                           central ;
  * build_door()         : battants AFFLEURANTS, parts SÉPARÉES émises EN TOUT
                           DERNIER (droite puis gauche) : ce sont les 2 dernières
                           primitives du GLB, convention que l'app C++ utilise
                           pour leur appliquer leur matrice d'animation (bouchon
                           latéral puis coulissement longitudinal).

Le bogie Jacobs est généré à part, origine locale au PLAN DE ROULEMENT (y=0) :
  * châssis au-dessus des roues ;
  * 2 ESSIEUX en parts séparées (les 2 DERNIÈRES primitives), chacun CENTRÉ À
    L'ORIGINE (axe = x) : l'app les place à (0, 0.46, ±1.5) et leur applique la
    rotation de roulement (v = ω·r, cf. Bogie::wheel_angle). Roues Ø 920 mm,
    écartement 1435 mm, boudins côté intérieur.

Repère caisse : x = droite, y = haut, z = arrière ; rail à y = -2.20.
Aucune dépendance externe (stdlib seule). Sortie : deux .glb."""

import struct
import json
import math
import sys

# --- Cotes réelles (mètres) -----------------------------------------------------
RAIL = -2.20
BODY_LEN = 23.60
HALF_W = 1.45
WALL = 0.12
BODY_BOT = -1.15        # bas de caisse (1.05 m au-dessus du rail)
ROOF_Y = 1.62           # hauteur des murs (base du toit)
CAMBER = 0.22           # flèche du toit (ROOF_Y + CAMBER ≈ 4.04 m au-dessus du rail)
IN_FLOOR = -1.00        # plancher intérieur voyageurs
IN_CEIL = 1.05          # plafond intérieur
IN_HALF_W = 1.32        # murs intérieurs

Z_HEAD = -BODY_LEN / 2.0    # -11.80 (avant)
Z_TAIL = BODY_LEN / 2.0     # +11.80 (arrière)

# Portes : une embrasure par flanc, près de l'avant. Seuil ~1,26 m et linteau
# ~3,00 m au-dessus du rail.
DOOR_Z0, DOOR_Z1 = -10.80, -9.90
DOOR_Y0, DOOR_Y1 = -0.94, 0.80

# Bandeau vitré voyageurs.
WIN_SILL, WIN_LINTEL = -0.35, 0.55

# --- Matériaux PBR --------------------------------------------------------------
MATERIALS = [
    {"name": "peinture", "factor": [0.85, 0.86, 0.88, 1.0], "metallic": 0.45, "roughness": 0.25},
    # Vitrage : verre PBR strict, transparence absolue (cahier des charges M30).
    {"name": "vitrage", "factor": [0.1, 0.1, 0.1, 0.3], "metallic": 0.0, "roughness": 0.02,
     "blend": True, "doubleSided": True},
    {"name": "accent", "factor": [0.04, 0.07, 0.18, 1.0], "metallic": 0.30, "roughness": 0.30},
    {"name": "jupe", "factor": [0.20, 0.21, 0.23, 1.0], "metallic": 0.05, "roughness": 0.70},
    {"name": "soufflet", "factor": [0.05, 0.05, 0.05, 1.0], "metallic": 0.0, "roughness": 0.85},
    {"name": "interieur", "factor": [0.45, 0.46, 0.48, 1.0], "metallic": 0.0, "roughness": 0.80,
     "doubleSided": True},
    {"name": "siege", "factor": [0.08, 0.10, 0.25, 1.0], "metallic": 0.0, "roughness": 0.90},
    # Battants de porte : DEUX exemplaires du même matériau, car chaque battant
    # est une part séparée — les 2 DERNIÈRES primitives du GLB voiture.
    {"name": "porte", "factor": [0.85, 0.86, 0.88, 1.0], "metallic": 0.45, "roughness": 0.25},
    {"name": "porte", "factor": [0.85, 0.86, 0.88, 1.0], "metallic": 0.45, "roughness": 0.25},
    # Bogie : châssis puis DEUX essieux (parts séparées, les 2 dernières du GLB).
    {"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65},
    {"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
    {"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
]
(MAT_PAINT, MAT_GLASS, MAT_ACCENT, MAT_SKIRT, MAT_BELLOWS, MAT_INTERIOR,
 MAT_SEAT, MAT_DOOR_R, MAT_DOOR_L, MAT_BOGIE, MAT_AXLE_A, MAT_AXLE_B) = range(12)


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
        """Polygone convexe (3-4 sommets), triangulé en éventail."""
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
    """Quad plan à orientation FORCÉE (winding inversé si besoin)."""
    n = norm(cross(sub(b, a), sub(c, a)))
    if dot(n, want) < 0.0:
        b, d = d, b
        n = mul(n, -1.0)
    part.add_quad(a, b, c, d, n)


def wall_with_openings(part, xa, xb, z0, z1, y0, y1, openings):
    """Mur plan AVEC TROUS PHYSIQUES : découpe (z, y) en dalles sur les bords
    des ouvertures, les dalles couvertes sont omises."""
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


def window_bays(z_start, z_end):
    """Baies vitrées régulières (1,30 m, pas de 1,65 m) hors plateforme porte."""
    bays = []
    z = z_start
    while z + 1.30 <= z_end:
        bays.append((z, z + 1.30, WIN_SILL, WIN_LINTEL))
        z += 1.65
    return bays


BAYS = window_bays(DOOR_Z1 + 0.25, Z_TAIL - 0.60)
DOOR_OPENING = (DOOR_Z0, DOOR_Z1, DOOR_Y0, DOOR_Y1)


# ==============================================================================
# VOITURE VOYAGEURS
# ==============================================================================
def build_floor(parts):
    """Dalle de plancher + jupe de bas de caisse."""
    parts[MAT_SKIRT].add_box(-HALF_W, BODY_BOT - 0.15, Z_HEAD,
                             HALF_W, IN_FLOOR, Z_TAIL)
    parts[MAT_SKIRT].add_box(-HALF_W + 0.05, RAIL + 0.40, Z_HEAD + 1.0,
                             HALF_W - 0.05, BODY_BOT - 0.15, Z_TAIL - 1.0)


def build_side_walls(parts):
    """Murs latéraux avec baies vitrées et embrasure de porte DÉCOUPÉES."""
    openings = BAYS + [DOOR_OPENING]
    for side in (-1.0, 1.0):
        xa, xb = sorted((side * HALF_W, side * (HALF_W - WALL)))
        wall_with_openings(parts[MAT_PAINT], xa, xb, Z_HEAD, Z_TAIL,
                           BODY_BOT, ROOF_Y, openings)
        for (z0, z1, y0, y1) in BAYS:
            parts[MAT_GLASS].add_box(xa - 0.005, y0, z0, xb + 0.005, y1, z1,
                                     top=False, bot=False, front=False, back=False)
        # Filet bleu nuit sous le bandeau vitré (interrompu à la porte).
        x0, x1 = sorted((side * HALF_W, side * (HALF_W + 0.012)))
        for za, zb in ((Z_HEAD + 0.4, DOOR_Z0), (DOOR_Z1, Z_TAIL - 0.4)):
            parts[MAT_ACCENT].add_box(x0, WIN_SILL - 0.18, za, x1, WIN_SILL - 0.03, zb)
        # Feuillures d'embrasure : seuil, linteau et montants de la porte.
        quad_orient(parts[MAT_PAINT],
                    (xa, DOOR_Y0, DOOR_Z0), (xa, DOOR_Y0, DOOR_Z1),
                    (xb, DOOR_Y0, DOOR_Z1), (xb, DOOR_Y0, DOOR_Z0), (0, 1, 0))
        quad_orient(parts[MAT_PAINT],
                    (xa, DOOR_Y1, DOOR_Z0), (xa, DOOR_Y1, DOOR_Z1),
                    (xb, DOOR_Y1, DOOR_Z1), (xb, DOOR_Y1, DOOR_Z0), (0, -1, 0))
        for z, want in ((DOOR_Z0, (0, 0, 1)), (DOOR_Z1, (0, 0, -1))):
            quad_orient(parts[MAT_PAINT],
                        (xa, DOOR_Y0, z), (xa, DOOR_Y1, z),
                        (xb, DOOR_Y1, z), (xb, DOOR_Y0, z), want)


def build_roof(parts):
    """Toit bombé indépendant (3 pans), posé sur les murs."""
    xs = HALF_W * 0.62
    paint = parts[MAT_PAINT]
    quad_orient(paint, (-HALF_W, ROOF_Y, Z_TAIL), (-xs, ROOF_Y + CAMBER, Z_TAIL),
                (-xs, ROOF_Y + CAMBER, Z_HEAD), (-HALF_W, ROOF_Y, Z_HEAD), (0, 1, 0))
    quad_orient(paint, (-xs, ROOF_Y + CAMBER, Z_TAIL), (xs, ROOF_Y + CAMBER, Z_TAIL),
                (xs, ROOF_Y + CAMBER, Z_HEAD), (-xs, ROOF_Y + CAMBER, Z_HEAD), (0, 1, 0))
    quad_orient(paint, (xs, ROOF_Y + CAMBER, Z_TAIL), (HALF_W, ROOF_Y, Z_TAIL),
                (HALF_W, ROOF_Y, Z_HEAD), (xs, ROOF_Y + CAMBER, Z_HEAD), (0, 1, 0))


def build_end_caps(parts):
    """Obturations des bouts + soufflets d'intercirculation."""
    for zend, sgn in ((Z_TAIL, 1.0), (Z_HEAD, -1.0)):
        parts[MAT_PAINT].add_box(-HALF_W, BODY_BOT - 0.15, zend - sgn * 0.06,
                                 HALF_W, ROOF_Y + CAMBER, zend)
        parts[MAT_BELLOWS].add_box(-1.15, -0.80, zend,
                                   1.15, 1.45, zend + sgn * 0.55)


def build_interior(parts):
    """Caisson intérieur : plancher, plafond, murs percés au droit des portes."""
    parts[MAT_INTERIOR].add_box(-IN_HALF_W, IN_FLOOR - 0.03, Z_HEAD + 0.05,
                                IN_HALF_W, IN_FLOOR, Z_TAIL - 0.05)
    parts[MAT_INTERIOR].add_box(-IN_HALF_W, IN_CEIL, Z_HEAD + 0.05,
                                IN_HALF_W, IN_CEIL + 0.03, Z_TAIL - 0.05)
    openings = [(DOOR_Z0, DOOR_Z1, DOOR_Y0, DOOR_Y1)] + BAYS
    for side in (-1.0, 1.0):
        xa, xb = sorted((side * IN_HALF_W, side * (IN_HALF_W - 0.03)))
        wall_with_openings(parts[MAT_INTERIOR], xa, xb, Z_HEAD + 0.05, Z_TAIL - 0.05,
                           IN_FLOOR, IN_CEIL, openings)
    # Fonds aux deux bouts.
    for zend in (Z_HEAD + 0.05, Z_TAIL - 0.05):
        parts[MAT_INTERIOR].add_box(-IN_HALF_W, IN_FLOOR, zend - 0.03,
                                    IN_HALF_W, IN_CEIL, zend + 0.03)


def build_seat(parts, xc, zc, facing):
    """Un siège en L : pied, assise, dossier. facing = -1 => face à l'avant."""
    parts[MAT_SEAT].add_box(xc - 0.05, IN_FLOOR, zc - 0.05,
                            xc + 0.05, IN_FLOOR + 0.36, zc + 0.05)
    y = IN_FLOOR + 0.45
    parts[MAT_SEAT].add_box(xc - 0.23, y - 0.09, zc - 0.24,
                            xc + 0.23, y, zc + 0.24)
    z_back = zc - facing * 0.24
    z0, z1 = sorted((z_back, z_back + facing * 0.09))
    parts[MAT_SEAT].add_box(xc - 0.23, y, z0, xc + 0.23, y + 0.62, z1)


def build_seats(parts):
    """Rangées de 4 sièges (2+2) de part et d'autre du couloir central (60 cm),
    pas de 95 cm. La moitié avant regarde vers l'avant, la moitié arrière vers
    l'arrière — les voyageurs se font face au milieu."""
    pitch = 0.95
    x_in = 0.30 + 0.23            # siège côté allée
    x_out = 0.30 + 0.46 + 0.04 + 0.23   # siège côté fenêtre
    z = DOOR_Z1 + 0.90
    while z <= Z_TAIL - 1.30:
        facing = -1.0 if z < 0.0 else 1.0
        for sx in (1.0, -1.0):
            for xoff in (x_in, x_out):
                build_seat(parts, sx * xoff, z, facing)
        z += pitch


def build_door(part, side):
    """Battant AFFLEURANT dans son embrasure (rentré de 4 mm pour éviter le
    z-fighting), épaisseur 10 cm. Fermé, aucun jour visible ; ouvert, l'app
    translate le battant (bouchon latéral puis coulissement)."""
    x_out = side * (HALF_W - 0.004)
    x_in = side * (HALF_W - 0.104)
    part.add_box(min(x_out, x_in), DOOR_Y0 + 0.004, DOOR_Z0 + 0.004,
                 max(x_out, x_in), DOOR_Y1 - 0.004, DOOR_Z1 - 0.004)


def build_car(out):
    parts = [Part() for _ in MATERIALS]
    build_floor(parts)
    build_side_walls(parts)
    build_roof(parts)
    build_end_caps(parts)
    build_interior(parts)
    build_seats(parts)
    # LES 2 DERNIÈRES PARTS : battants de porte (droite N-2, gauche N-1).
    build_door(parts[MAT_DOOR_R], side=+1.0)
    build_door(parts[MAT_DOOR_L], side=-1.0)
    write_glb(out, parts, "TGV_voiture")


# ==============================================================================
# BOGIE JACOBS — origine au plan de roulement (y = 0)
# ==============================================================================
WHEEL_R = 0.46          # Ø 920 mm
GAUGE_HALF = 0.7175     # 1435 mm entre les faces internes des rails
FLANGE_R = 0.505        # boudin plongeant sous le champignon


def build_wheel(part, cx, segments=24):
    """Roue centrée sur l'AXE de l'essieu (moyeu à l'origine locale, axe = x) :
    bande de roulement à WHEEL_R, flancs en éventail, boudin côté INTÉRIEUR."""
    inner = -1.0 if cx > 0.0 else 1.0
    xo = cx + inner * 0.0675    # face extérieure (bande de 135 mm)
    xi = cx - inner * 0.0675    # face intérieure
    xf = xi - inner * 0.025     # boudin : 25 mm au-delà

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
    """Essieu complet CENTRÉ À L'ORIGINE (axe = x local) : arbre + 2 roues Ø920
    à boudins. L'app le translate à (0, WHEEL_R, ±1.5) et applique la rotation."""
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
    # Châssis : au-dessus des roues, plus étroit que la voie.
    parts[MAT_BOGIE].add_box(-0.45, WHEEL_R + 0.10, -1.9, 0.45, WHEEL_R + 0.85, 1.9)
    # LES 2 DERNIÈRES PARTS : essieux centrés à l'origine (A à z=-1.5, B à +1.5).
    build_axle(parts[MAT_AXLE_A])
    build_axle(parts[MAT_AXLE_B])
    write_glb(out, parts, "TGV_bogie_jacobs")


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

    gltf = {"asset": {"version": "2.0", "generator": "noire-tgv-v3-modulaire (CC0)"},
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
    build_car(f"{outdir}/tgv_voiture.glb")
    build_bogie(f"{outdir}/tgv_bogie.glb")
