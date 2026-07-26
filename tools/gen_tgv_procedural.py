#!/usr/bin/env python3
"""Motrice TGV V3.0 — REBOOT TOTAL (M30), construction MODULAIRE par panneaux.

Plus de tube ni de loft : la caisse est un ASSEMBLAGE de pièces distinctes,
chacune construite par une fonction dédiée :
  * build_chassis()      : dalle de plancher plate + jupe de bas de caisse ;
  * build_body_walls()   : murs latéraux voyageurs avec baies DÉCOUPÉES (trous
                           physiques : piliers pleine hauteur entre les baies +
                           bandeaux sous l'appui et au-dessus du linteau) ;
  * build_roof()         : toit INDÉPENDANT, bombé en 3 pans, posé sur les murs ;
  * build_cab()          : murs, cloison, plafond et piliers A de la cabine —
                           un VIDE PHYSIQUE réel entre plancher, toit et nez ;
  * build_nose()         : nez profilé SÉPARÉ (sections de largeur/hauteur
                           décroissantes vers la pointe), raccordé sous le
                           pare-brise ;
  * build_windshield()   : primitive PARE-BRISE distincte, verre PBR
                           (baseColorFactor [0.1,0.1,0.1,0.3], alphaMode BLEND,
                           doubleSided) — rien d'autre n'obstrue la vue depuis
                           la tête du conducteur (0, 1.20, -2.50) ;
  * build_dashboard()    : pupitre (console, panneau incliné, 3 écrans,
                           leviers traction/frein, bouton d'urgence) ;
  * build_driver_seat()  : siège conducteur.

Repère caisse : x = droite, y = haut, z = arrière (l'avant est en -z).
Plan de roulement : y = -2.20. L'app applique une matrice identité et dessine
les bogies séparément (tgv_bogie.glb) : la motrice ne porte pas de roues.

Aucune dépendance externe (stdlib seule). Sortie : un .glb."""

import math
import json
import struct
import sys

# --- Cotes réelles (mètres) -----------------------------------------------------
RAIL = -2.20            # plan de roulement dans le repère caisse
LENGTH = 22.15          # longueur totale de caisse
HALF_W = 1.45           # demi-largeur (2.90 m, gabarit UIC)
FLOOR_Y = 0.00          # plancher de caisse (2.20 m au-dessus du rail)
ROOF_Y = 1.85           # hauteur des murs (base du toit)
CAMBER = 0.22           # flèche du toit bombé
WALL = 0.12             # épaisseur des murs latéraux

Z_TAIL = LENGTH / 2.0   # +11.075 (arrière, z+ = arrière)
Z_TIP = -LENGTH / 2.0   # -11.075 (pointe du nez, avant en -z)
# Cabine à l'AVANT : du pare-brise (z ≈ -3.45/-3.70) vers l'ARRIÈRE (z croissant)
# jusqu'à la cloison (z = -1.80, derrière le siège). La caisse voyageurs suit.
Z_CAB_REAR = -1.80      # cloison arrière de la cabine
Z_WS_TOP = -3.45        # pare-brise : bord haut (raccord toit)
Z_WS_BOT = -3.70        # pare-brise : bord bas (raccord capot)
WS_Y_BOT = 0.66         # hauteur du bord bas du pare-brise
WS_HALF_TOP = 0.90      # demi-largeur du pare-brise en haut
WS_HALF_BOT = 0.88      # demi-largeur du pare-brise en bas

# --- Matériaux PBR glTF ---------------------------------------------------------
MATERIALS = [
    # 0: peinture — carrosserie blanc métallisé inOui
    {"name": "peinture", "factor": [0.85, 0.86, 0.88, 1.0], "metallic": 0.45, "roughness": 0.25},
    # 1: parebrise — verre PBR, transparence absolue (cahier des charges M30)
    {"name": "parebrise", "factor": [0.1, 0.1, 0.1, 0.3], "metallic": 0.0, "roughness": 0.02,
     "blend": True, "doubleSided": True},
    # 2: vitrage — fenêtres latérales, même verre
    {"name": "vitrage", "factor": [0.1, 0.1, 0.1, 0.3], "metallic": 0.0, "roughness": 0.02,
     "blend": True, "doubleSided": True},
    # 3: accent — filet bleu nuit inOui
    {"name": "accent", "factor": [0.04, 0.07, 0.18, 1.0], "metallic": 0.30, "roughness": 0.30},
    # 4: jupe — bas de caisse gris anthracite mat
    {"name": "jupe", "factor": [0.20, 0.21, 0.23, 1.0], "metallic": 0.05, "roughness": 0.70},
    # 5: soufflet — attelage / intercirculation noir
    {"name": "soufflet", "factor": [0.05, 0.05, 0.05, 1.0], "metallic": 0.0, "roughness": 0.85},
    # 6: interieur — panneaux de cabine gris mat, double face
    {"name": "interieur", "factor": [0.45, 0.46, 0.48, 1.0], "metallic": 0.0, "roughness": 0.80,
     "doubleSided": True},
    # 7: pupitre — tableau de bord gris sombre mat
    {"name": "pupitre", "factor": [0.12, 0.13, 0.15, 1.0], "metallic": 0.0, "roughness": 0.75},
    # 8: ecran — écrans KVB / tachymètre / ATESS bleuté brillant
    {"name": "ecran", "factor": [0.05, 0.15, 0.30, 1.0], "metallic": 0.0, "roughness": 0.10},
    # 9: bouton — commandes et voyants
    {"name": "bouton", "factor": [0.85, 0.20, 0.15, 1.0], "metallic": 0.30, "roughness": 0.30},
    # 10: siege — velours bleu nuit
    {"name": "siege", "factor": [0.08, 0.10, 0.25, 1.0], "metallic": 0.0, "roughness": 0.90},
]
(MAT_PAINT, MAT_WS, MAT_GLASS, MAT_ACCENT, MAT_SKIRT, MAT_BELLOWS,
 MAT_INTERIOR, MAT_PUPITRE, MAT_ECRAN, MAT_BOUTON, MAT_SIEGE) = range(11)


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
        """Ajoute un polygone convexe (3 ou 4 sommets), triangulé en éventail.
        verts = [(position, normale, uv, tangente), ...]."""
        base = len(self.positions)
        for p, n, uv, tg in verts:
            self.positions.append(p)
            self.normals.append(n)
            self.uvs.append(uv)
            self.tangents.append(tg)
        for k in range(1, len(verts) - 1):
            self.indices.extend([base, base + k, base + k + 1])

    def add_quad(self, p0, p1, p2, p3, n=None):
        """Quad plan, normale et tangente calculées si non fournies."""
        if n is None:
            n = norm(cross(sub(p1, p0), sub(p2, p0)))
        tan = norm(sub(p1, p0))
        tg = (tan[0], tan[1], tan[2], 1.0)
        self.add([(p0, n, (0, 0), tg), (p1, n, (1, 0), tg),
                  (p2, n, (1, 1), tg), (p3, n, (0, 1), tg)])

    def add_tri(self, p0, p1, p2, n=None):
        if n is None:
            n = norm(cross(sub(p1, p0), sub(p2, p0)))
        tan = norm(sub(p1, p0))
        tg = (tan[0], tan[1], tan[2], 1.0)
        self.add([(p0, n, (0, 0), tg), (p1, n, (1, 0), tg), (p2, n, (0.5, 1), tg)])

    def add_box(self, x0, y0, z0, x1, y1, z1,
                top=True, bot=True, front=True, back=True, left=True, right=True):
        """Pavé droit axis-aligned, normales extérieures, faces optionnelles."""
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
    """Quad plan dont on FORCE l'orientation : si la normale géométrique ne
    pointe pas du côté de `want`, le winding est inversé."""
    n = norm(cross(sub(b, a), sub(c, a)))
    if dot(n, want) < 0.0:
        b, d = d, b
        n = mul(n, -1.0)
    part.add_quad(a, b, c, d, n)


def wall_with_openings(part, xa, xb, z0, z1, y0, y1, openings):
    """Un mur latéral plan AVEC TROUS PHYSIQUES : le plan (z, y) est découpé en
    dalles sur les bords des ouvertures ; toute dalle dont le centre tombe dans
    une ouverture est omise. openings = [(z0, z1, y0, y1), ...]."""
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


parts = [Part() for _ in MATERIALS]

# ==============================================================================
# 1. CHÂSSIS — dalle de plancher plate + jupe
# ==============================================================================
def build_chassis():
    # Dalle de plancher : du recul du capot (bord bas du pare-brise) à la queue.
    parts[MAT_PAINT].add_box(-HALF_W, FLOOR_Y - 0.15, Z_WS_BOT, HALF_W, FLOOR_Y, Z_TAIL)
    # Jupe inférieure (tablier gris mat sous la caisse).
    parts[MAT_SKIRT].add_box(-HALF_W + 0.05, RAIL + 0.40, Z_WS_BOT + 0.50,
                             HALF_W - 0.05, FLOOR_Y - 0.15, Z_TAIL - 0.20)


# ==============================================================================
# 2. MURS LATÉRAUX VOYAGEURS — baies DÉCOUPÉES, toit et obturation arrière
# ==============================================================================
def window_bays(z_start, z_end, sill, lintel, bay_w=1.30, pitch=1.65):
    """Liste des baies (z0, z1, y0, y1) régulières entre z_start et z_end."""
    bays = []
    z = z_start
    while z + bay_w <= z_end:
        bays.append((z, z + bay_w, sill, lintel))
        z += pitch
    return bays


def build_body_walls():
    bays = window_bays(Z_CAB_REAR + 0.55, Z_TAIL - 0.55, 0.80, 1.50)
    for side in (-1.0, 1.0):
        x_out = side * HALF_W
        x_in = side * (HALF_W - WALL)
        xa, xb = sorted((x_out, x_in))
        wall_with_openings(parts[MAT_PAINT], xa, xb, Z_CAB_REAR, Z_TAIL, FLOOR_Y, ROOF_Y, bays)
        # Verre posé DANS chaque baie (à fleur du mur).
        for (z0, z1, y0, y1) in bays:
            parts[MAT_GLASS].add_box(xa - 0.005, y0, z0, xb + 0.005, y1, z1,
                                     top=False, bot=False, front=False, back=False)
    # Filet bleu nuit plaqué sous le bandeau vitré.
    for side in (-1.0, 1.0):
        x0, x1 = sorted((side * HALF_W, side * (HALF_W + 0.012)))
        parts[MAT_ACCENT].add_box(x0, 0.55, Z_CAB_REAR + 0.10, x1, 0.70, Z_TAIL - 0.10)
    # Obturation arrière (queue), pleine hauteur toit compris.
    parts[MAT_PAINT].add_box(-HALF_W, FLOOR_Y - 0.15, Z_TAIL - 0.06,
                             HALF_W, ROOF_Y + CAMBER, Z_TAIL)
    # Soufflet d'attelage arrière.
    parts[MAT_BELLOWS].add_box(-1.10, -0.70, Z_TAIL, 1.10, 1.50, Z_TAIL + 0.55)


# ==============================================================================
# 3. TOIT — pièce indépendante, bombée en 3 pans, posée sur les murs
# ==============================================================================
def build_roof(z0, z1, close_front=False):
    xs = HALF_W * 0.62   # ~0.899 : raccord pile au-dessus du pare-brise
    paint = parts[MAT_PAINT]
    quad_orient(paint, (-HALF_W, ROOF_Y, z1), (-xs, ROOF_Y + CAMBER, z1),
                (-xs, ROOF_Y + CAMBER, z0), (-HALF_W, ROOF_Y, z0), (0, 1, 0))
    quad_orient(paint, (-xs, ROOF_Y + CAMBER, z1), (xs, ROOF_Y + CAMBER, z1),
                (xs, ROOF_Y + CAMBER, z0), (-xs, ROOF_Y + CAMBER, z0), (0, 1, 0))
    quad_orient(paint, (xs, ROOF_Y + CAMBER, z1), (HALF_W, ROOF_Y, z1),
                (HALF_W, ROOF_Y, z0), (xs, ROOF_Y + CAMBER, z0), (0, 1, 0))
    if close_front:
        # Linteau au-dessus du pare-brise (plane z = z0) + écoinçons au-dessus
        # des piliers A : la face avant du toit est ENTIÈREMENT fermée.
        quad_orient(paint, (-WS_HALF_TOP, ROOF_Y, z0), (WS_HALF_TOP, ROOF_Y, z0),
                    (xs, ROOF_Y + CAMBER, z0), (-xs, ROOF_Y + CAMBER, z0), (0, 0.3, -1.0))
        for s in (-1.0, 1.0):
            a = (s * WS_HALF_TOP, ROOF_Y, z0)
            b = (s * HALF_W, ROOF_Y, z0)
            c = (s * xs, ROOF_Y + CAMBER, z0)
            n = norm(cross(sub(b, a), sub(c, a)))
            if n[2] > 0.0:
                b, c = c, b
                n = mul(n, -1.0)
            parts[MAT_PAINT].add_tri(a, b, c, n)


# ==============================================================================
# 4. CABINE — murs, cloison, plafond, piliers A : un VIDE PHYSIQUE réel
# ==============================================================================
def build_cab():
    # Cloison arrière de la cabine (séparation voyageurs / poste de conduite).
    parts[MAT_INTERIOR].add_box(-HALF_W + 0.10, FLOOR_Y, Z_CAB_REAR - 0.08,
                                HALF_W - 0.10, ROOF_Y, Z_CAB_REAR)
    # Plafond de cabine : AFFLEURANT au bord haut du pare-brise (y = ROOF_Y) —
    # plus bas, il masquerait le haut du verre vu depuis la tête du conducteur.
    parts[MAT_INTERIOR].add_box(-1.30, ROOF_Y, Z_WS_TOP + 0.02,
                                1.30, ROOF_Y + 0.03, Z_CAB_REAR - 0.08)
    parts[MAT_INTERIOR].add_box(-1.30, ROOF_Y - 0.05, Z_CAB_REAR,
                                1.30, ROOF_Y, Z_TAIL - 0.10)

    cab_window = (Z_WS_BOT + 0.15, Z_CAB_REAR - 0.25, 0.85, 1.45)
    for side in (-1.0, 1.0):
        x_out = side * HALF_W
        x_in = side * (HALF_W - WALL)
        xa, xb = sorted((x_out, x_in))
        # Mur bas : plein, prolongé jusqu'au bord bas du pare-brise.
        parts[MAT_PAINT].add_box(xa, FLOOR_Y, Z_WS_BOT, xb, WS_Y_BOT, Z_CAB_REAR)
        # Mur haut : percé de la fenêtre latérale de cabine, recule au bord haut.
        wall_with_openings(parts[MAT_PAINT], xa, xb, Z_WS_TOP, Z_CAB_REAR,
                           WS_Y_BOT, ROOF_Y, [cab_window])
        # Verre de la fenêtre latérale.
        z0, z1, y0, y1 = cab_window
        parts[MAT_GLASS].add_box(xa - 0.005, y0, z0, xb + 0.005, y1, z1,
                                 top=False, bot=False, front=False, back=False)
        # Écoinçon : ferme le triangle entre le front du mur haut (z=Z_WS_TOP)
        # et le chant du pilier A (de (WS_Y_BOT, Z_WS_BOT) à (ROOF_Y, Z_WS_TOP)).
        a = (x_out, WS_Y_BOT, Z_WS_BOT)
        b = (x_out, WS_Y_BOT, Z_WS_TOP)
        c = (x_out, ROOF_Y, Z_WS_TOP)
        n = (side, 0.0, 0.0)
        if side > 0.0:
            parts[MAT_PAINT].add_tri(a, b, c, n)
        else:
            parts[MAT_PAINT].add_tri(a, c, b, n)

    # Piliers A : pans latéraux COPLANAIRES avec le pare-brise, entre le verre
    # (|x| < 0.90) et les murs (|x| = HALF_W).
    for side in (-1.0, 1.0):
        x0, x1 = sorted((side * WS_HALF_TOP, side * HALF_W))
        x2, x3 = sorted((side * WS_HALF_BOT, side * HALF_W))
        quad_orient(parts[MAT_PAINT],
                    (x0, ROOF_Y, Z_WS_TOP), (x1, ROOF_Y, Z_WS_TOP),
                    (x3, WS_Y_BOT, Z_WS_BOT), (x2, WS_Y_BOT, Z_WS_BOT),
                    (0.0, 0.2, -1.0))

    # Toit au-dessus de la cabine (face avant fermée : linteau + écoinçons).
    build_roof(Z_WS_TOP, Z_CAB_REAR, close_front=True)


# ==============================================================================
# 5. NEZ PROFILÉ — pièce séparée : sections décroissantes vers la pointe
# ==============================================================================
# (z, demi-largeur, y haut, y bas). La première section se raccorde pile sous
# le bord bas du pare-brise ; la dernière est le museau.
NOSE_SECTIONS = [
    (Z_WS_BOT, 1.45, 0.65, -0.15),
    (-6.00,    1.20, 0.30, -0.60),
    (-8.50,    0.85, -0.20, -1.10),
    (-10.50,   0.45, -0.75, -1.60),
    (Z_TIP,    0.30, -0.90, -1.60),
]


def build_nose():
    paint, skirt = parts[MAT_PAINT], parts[MAT_SKIRT]
    for (z0, w0, ht0, hb0), (z1, w1, ht1, hb1) in zip(NOSE_SECTIONS, NOSE_SECTIONS[1:]):
        # Capot (pan supérieur plongeant).
        quad_orient(paint, (-w0, ht0, z0), (w0, ht0, z0),
                    (w1, ht1, z1), (-w1, ht1, z1), (0, 1, 0))
        # Flancs du nez.
        quad_orient(paint, (-w0, hb0, z0), (-w0, ht0, z0),
                    (-w1, ht1, z1), (-w1, hb1, z1), (-1, 0, 0))
        quad_orient(paint, (w0, ht0, z0), (w0, hb0, z0),
                    (w1, hb1, z1), (w1, ht1, z1), (1, 0, 0))
        # Pan ventral (sous le nez).
        quad_orient(skirt, (-w0, hb0, z1), (w0, hb0, z1),
                    (w1, hb1, z0), (-w1, hb1, z0), (0, -1, 0))
    # Museau / tablier avant (ferme la pointe).
    z, w, ht, hb = NOSE_SECTIONS[-1]
    quad_orient(skirt, (-w, hb, z), (w, hb, z), (w, ht, z), (-w, ht, z), (0, 0, -1))
    # Filet bleu nuit sur les flancs du nez (pan plaqué, décalé de 8 mm).
    for (z0, w0, ht0, hb0), (z1, w1, ht1, hb1) in zip(NOSE_SECTIONS[:2], NOSE_SECTIONS[1:3]):
        y0, y1 = ht0 - 0.10, ht1 - 0.10
        for side in (-1.0, 1.0):
            x0, x1 = side * (w0 + 0.008), side * (w1 + 0.008)
            quad_orient(parts[MAT_ACCENT], (x0, y0 - 0.12, z0), (x0, y0, z0),
                        (x1, y1, z1), (x1, y1 - 0.12, z1), (side, 0, 0))


# ==============================================================================
# 6. PARE-BRISE — primitive DISTINCTE en verre PBR, seule géométrie devant les
#    yeux du conducteur : aucune obstruction entre la tête (0, 1.20, -2.50) et
#    la voie.
# ==============================================================================
def build_windshield():
    quad_orient(parts[MAT_WS],
                (-WS_HALF_TOP, ROOF_Y, Z_WS_TOP), (WS_HALF_TOP, ROOF_Y, Z_WS_TOP),
                (WS_HALF_BOT, WS_Y_BOT, Z_WS_BOT), (-WS_HALF_BOT, WS_Y_BOT, Z_WS_BOT),
                (0.0, 0.2, -1.0))


# ==============================================================================
# 7. POSTE DE CONDUITE — pupitre, écrans, leviers, posés sur le plancher
# ==============================================================================
def build_dashboard():
    pup = parts[MAT_PUPITRE]
    # Console principale : posée sur le plancher (y=0), sous le pare-brise.
    pup.add_box(-0.92, FLOOR_Y, -3.55, 0.92, 0.62, -2.90)
    # Panneau d'instrumentation incliné, face au conducteur.
    quad_orient(pup, (-0.88, 0.62, -3.00), (0.88, 0.62, -3.00),
                (0.88, 0.80, -3.25), (-0.88, 0.80, -3.25), (0.0, 0.7, 0.7))
    pup.add_box(-0.88, 0.62, -3.25, 0.88, 0.80, -3.00, top=False)
    # Écrans : KVB (gauche), tachymètre (centre), ATESS (droite).
    for x0, x1 in ((-0.65, -0.35), (-0.15, 0.15), (0.35, 0.65)):
        parts[MAT_ECRAN].add_box(x0, 0.66, -3.13, x1, 0.77, -3.09)
    # Leviers : traction (gauche) et frein (droite).
    for x in (-0.56, 0.56):
        parts[MAT_BOUTON].add_box(x - 0.04, 0.62, -2.95, x + 0.04, 0.65, -2.85)
        parts[MAT_BOUTON].add_box(x - 0.01, 0.65, -2.92, x + 0.01, 0.76, -2.90)
    # Bouton coup de poing d'urgence (centre).
    parts[MAT_BOUTON].add_box(-0.04, 0.62, -2.88, 0.04, 0.68, -2.80)


# ==============================================================================
# 8. SIÈGE CONDUCTEUR — centré x=0, face à la voie (-z)
# ==============================================================================
def build_driver_seat():
    parts[MAT_PUPITRE].add_box(-0.25, FLOOR_Y, -2.65, 0.25, 0.45, -2.15)   # socle
    parts[MAT_SIEGE].add_box(-0.28, 0.45, -2.70, 0.28, 0.55, -2.10)        # assise
    parts[MAT_SIEGE].add_box(-0.26, 0.55, -2.12, 0.26, 1.15, -2.02)        # dossier
    parts[MAT_SIEGE].add_box(-0.18, 1.15, -2.12, 0.18, 1.30, -2.00)        # appui-tête
    parts[MAT_PUPITRE].add_box(-0.35, 0.70, -2.55, -0.28, 0.75, -2.25)     # accoudoir G
    parts[MAT_PUPITRE].add_box(0.28, 0.70, -2.55, 0.35, 0.75, -2.25)       # accoudoir D


# ==============================================================================
# Assemblage
# ==============================================================================
build_chassis()
build_body_walls()
build_roof(Z_CAB_REAR, Z_TAIL)          # toit de la caisse voyageurs
build_cab()
build_nose()
build_windshield()
build_dashboard()
build_driver_seat()


# ==============================================================================
# SÉRIALISATION GLTF BINAIRE (.GLB) — une primitive par part non vide
# ==============================================================================
def align4(n):
    return (n + 3) & ~3


used = [(i, p) for i, p in enumerate(parts) if len(p.positions) > 0]

part_blocks, blocks = [], []
for mat_idx, p in used:
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
        {"bufferView": base, "componentType": 5126, "count": len(p.positions), "type": "VEC3", "min": pmin, "max": pmax},
        {"bufferView": base + 1, "componentType": 5126, "count": len(p.normals), "type": "VEC3"},
        {"bufferView": base + 2, "componentType": 5126, "count": len(p.uvs), "type": "VEC2"},
        {"bufferView": base + 3, "componentType": 5126, "count": len(p.tangents), "type": "VEC4"},
        {"bufferView": base + 4, "componentType": 5125, "count": len(p.indices), "type": "SCALAR"},
    ]
    targets = [34962, 34962, 34962, 34962, 34963]
    for k in range(5):
        buffer_views.append({"buffer": 0, "byteOffset": offsets[base + k],
                             "byteLength": len(part_blocks[slot][k]), "target": targets[k]})
    primitives.append({
        "attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2, "TANGENT": base + 3},
        "indices": base + 4, "material": slot
    })

materials = []
for mat_idx, _ in used:
    mat_def = MATERIALS[mat_idx]
    m = {
        "name": mat_def["name"],
        "pbrMetallicRoughness": {
            "baseColorFactor": mat_def["factor"],
            "metallicFactor": mat_def["metallic"],
            "roughnessFactor": mat_def["roughness"]
        }
    }
    if mat_def.get("blend"):
        m["alphaMode"] = "BLEND"
        m["doubleSided"] = True
    elif mat_def.get("doubleSided"):
        m["doubleSided"] = True
    materials.append(m)

gltf = {
    "asset": {"version": "2.0", "generator": "noire-tgv-v3-modulaire (CC0)"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "TGV_motrice"}],
    "meshes": [{"primitives": primitives}],
    "materials": materials,
    "accessors": accessors,
    "bufferViews": buffer_views,
    "buffers": [{"byteLength": total}],
}

json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))
bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)

out = sys.argv[1] if len(sys.argv) > 1 else "tgv_procedural.glb"
glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
with open(out, "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, glb_len))
    f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
    f.write(json_bytes)
    f.write(struct.pack("<II", len(bin_pad), 0x004E4942))
    f.write(bin_pad)

nv = sum(len(p.positions) for p in parts)
ni = sum(len(p.indices) for p in parts)
print(f"{out} : Motrice TGV V3 modulaire, {LENGTH:.2f} x {HALF_W * 2:.2f} m")
print("  " + ", ".join(f"{m['name']}={len(p.positions)}v" for m, p in zip(MATERIALS, parts)))
print(f"  {nv} sommets, {ni // 3} triangles, {glb_len} octets")
