#!/usr/bin/env python3
"""Sculpte une VOITURE VOYAGEURS de TGV V2 et un BOGIE JACOBS, au format .glb (M20).

La voiture reprend l'architecture loftée de la motrice (tools/gen_tgv_procedural.py),
mais SANS nez : la section superellipse (n=4) est CONSTANTE sur toute la longueur —
c'est une caisse-tube. Elle partage avec la motrice les raffinements du M20 :
  * TOIT BOMBÉ (cambrage de 28 cm, nul au point le plus large : gabarit intact) ;
  * bandeau de vitres CREUSÉ dans la tôle (canal à rampes douces dans la surface de
    base, verre posé dedans à +8 mm — creuser le verre derrière une tôle pleine le
    rendrait invisible) ;
  * filet bleu nuit, jupe de bas de caisse gris mat, carénages de toit sinusoïdaux
    (climatisation + au-dessus des bogies Jacobs aux deux bouts) ;
  * SOUFFLETS d'intercirculation aux extrémités (bandes sombres + anneaux).

Le bogie Jacobs est généré à part : c'est l'organe PARTAGÉ entre deux caisses, positionné
par le Consist à l'articulation. Son origine locale est au PLAN DE ROULEMENT (y = 0), ce
qui colle à Bogie::position() (échantillon sur le rail).

M21 : la caisse est désormais HABITABLE —
  * une PORTE par flanc près du bout avant : trou rectangulaire dans le loft (les bords
    sont injectés comme stations/colonnes de la grille, le trou tombe donc PILE sur les
    quads — même mécanisme que les canaux de vitrage), feuillures d'embrasure entre la
    coque et l'intérieur ;
  * un CAISSON INTÉRIEUR rudimentaire (plancher, murs, plafond) à normales RENTRANTES ;
  * les deux BATTANTS sont des patchs de surface émis comme parts SÉPARÉES, EN TOUT
    DERNIER (droite puis gauche) : ce sont les 2 dernières primitives du GLB, convention
    que l'app utilise pour leur appliquer leur propre matrice d'animation (porte
    coulissante à bouchon : sortie latérale puis glissement le long de la caisse).

M22 : l'habitacle devient HABITÉ et le vitrage transparent —
  * vitrage en alphaMode BLEND (verre teinté, alpha 0.35) : on voit l'intérieur ;
  * SIÈGES procéduraux en L, rangées de 4 (2+2 de part et d'autre d'une allée centrale) ;
  * COULOIR D'INTERCIRCULATION : les fonds de caisson sont percés d'une porte et un
    tunnel (plancher/murs/plafond) court sous les anneaux du soufflet — l'illusion du
    passage d'une voiture à l'autre, sans voir le vide ni les rails ;
  * battants de porte AFFLEURANTS : patchs de la surface de coque exacte (plus des
    panneaux plats), rentrés de 4 mm — l'emboîtement fermé est au millimètre ;
  * bogie : VRAIS ESSIEUX (roues Ø920 mm, voie 1435 mm, boudins) émis en 2 primitives
    séparées centrées sur leur axe — l'app leur applique la rotation de roulement.

Repère caisse : x = droite, y = haut, z = arrière ; rail à y = -body_height = -2.20.
Aucune dépendance externe (stdlib seule). Sortie : deux .glb."""
import struct, json, math, sys

RAIL = -2.20
BODY_LEN = 23.60        # « légèrement plus longue » que la motrice (22.15 m)
HALF_W = 1.45           # même gabarit UIC (2.90 m)
CAMBER = 0.28           # flèche du toit bombé (comme la motrice)
ROOF = RAIL + 4.10 - CAMBER  # section de base ; ROOF + CAMBER = 4.10 m au-dessus du rail
FLOOR = RAIL + 1.05
SECTION_N = 4.0         # superellipse : rectangle arrondi d'une caisse ferroviaire
T_SEGMENTS = 40
BODY_STEPS = 10         # stations de base (la section est constante)
Z_TAIL = BODY_LEN / 2.0
Z_HEAD = -BODY_LEN / 2.0

# --- Canaux de vitrage creusés dans la tôle (cf. motrice) -----------------------
# (z0, z1, t0, t1, profondeur, rampe_z, rampe_t). t = 0 : flanc droit ; pi : gauche.
WIN_RZ, WIN_RT = 0.30, 0.10
CHANNELS = [
    # Bandeau voyageurs pleine longueur, les deux flancs.
    (Z_HEAD + 2.0, Z_TAIL - 2.0, -0.30, 0.35, 0.035, WIN_RZ, WIN_RT),
    (Z_HEAD + 2.0, Z_TAIL - 2.0, math.pi - 0.35, math.pi + 0.30, 0.035, WIN_RZ, WIN_RT),
]

# --- Matériaux PBR (identiques à la motrice, + roue/bogie pour le Jacobs) -------
MATERIALS = [
    {"name": "peinture", "factor": [0.82, 0.83, 0.86, 1.0], "metallic": 0.55, "roughness": 0.24},
    # Vitrage (M22) : VERRE, plus du noir opaque. alphaMode BLEND (clé "blend" =>
    # write_glb émet alphaMode/doubleSided) : le moteur le trie avec les autres
    # transparents, de loin vers la caméra, SANS écriture de profondeur. Teinte très
    # sombre + alpha 0.35 : on voit les sièges en transparence, le reflet du ciel
    # (roughness 0.05) reste dominant, comme sur le vrai bandeau.
    {"name": "vitrage", "factor": [0.06, 0.08, 0.11, 0.35], "metallic": 0.0,
     "roughness": 0.05, "blend": True},
    {"name": "accent", "factor": [0.045, 0.075, 0.16, 1.0], "metallic": 0.35, "roughness": 0.32},
    {"name": "jupe", "factor": [0.24, 0.25, 0.27, 1.0], "metallic": 0.0, "roughness": 0.72},
    {"name": "soufflet", "factor": [0.05, 0.05, 0.055, 1.0], "metallic": 0.0, "roughness": 0.85},
    {"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65},
    # Habitacle (M21) : gris clair MAT (mélaminé), éclairé par la seule ambiante.
    {"name": "interieur", "factor": [0.42, 0.43, 0.46, 1.0], "metallic": 0.0, "roughness": 0.85},
    # Sièges (M22) : velours bleu nuit inOui, très rugueux (tissu).
    {"name": "siege", "factor": [0.070, 0.090, 0.24, 1.0], "metallic": 0.0, "roughness": 0.95},
    # Battants de porte (M21), en DEUX exemplaires du MÊME matériau : chaque battant est
    # une part séparée (cf. write_glb : une part = une primitive = un slot matériau), et
    # l'app anime les 2 dernières primitives indépendamment de la caisse.
    {"name": "porte", "factor": [0.82, 0.83, 0.86, 1.0], "metallic": 0.55, "roughness": 0.24},
    {"name": "porte", "factor": [0.82, 0.83, 0.86, 1.0], "metallic": 0.55, "roughness": 0.24},
    # Essieux (M22), en DEUX exemplaires : chaque essieu (2 roues Ø920 + arbre) est une
    # part séparée CENTRÉE SUR SON AXE, et les 2 dernières primitives du GLB bogie —
    # l'app leur applique la rotation de roulement (v = omega * r) via Bogie::wheel_angle.
    # Acier NU, poli par le roulement : plus clair et plus lisse que le châssis.
    {"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
    {"name": "essieu", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
]
(MAT_PAINT, MAT_GLASS, MAT_ACCENT, MAT_SKIRT, MAT_BELLOWS, MAT_BOGIE,
 MAT_INTERIOR, MAT_SEAT, MAT_DOOR_R, MAT_DOOR_L, MAT_AXLE_A, MAT_AXLE_B) = range(12)

# --- Portes voyageurs (M21) : trous dans la tôle + battants séparés --------------
# Une porte par flanc, près du bout AVANT. Le rectangle [z0,z1]x[t0,t1] est calé pour
# ne toucher ni les soufflets (qui finissent à Z_HEAD + 0.85) ni le bandeau vitré (qui
# commence à Z_HEAD + 2.00) : un montant de tôle de 10 cm sépare porte et première baie.
DOOR_Z0, DOOR_Z1 = Z_HEAD + 1.00, Z_HEAD + 1.90
# t = 0 : flanc droit. -0.80 => seuil à ~1,26 m au-dessus du rail ; 0.17 => linteau à
# ~3,00 m. Le flanc gauche est le miroir autour de pi.
DOOR_T_LO, DOOR_T_HI = -0.80, 0.17
DOORS = [
    (DOOR_Z0, DOOR_Z1, DOOR_T_LO, DOOR_T_HI),                       # flanc droit
    (DOOR_Z0, DOOR_Z1, math.pi - DOOR_T_HI, math.pi - DOOR_T_LO),   # flanc gauche
]

# --- Habitacle rudimentaire (M21) -----------------------------------------------
IN_HALF_W = 1.32        # murs intérieurs : 13 cm de tôle + isolation derrière le flanc
IN_FLOOR = RAIL + 1.20  # plancher voyageurs, juste SOUS le seuil de porte (~1,26 m)
IN_CEIL = RAIL + 3.25   # plafond, juste AU-DESSUS du linteau (~3,00 m)

# --- Couloir d'intercirculation (M22) ---------------------------------------------
# Les fonds de caisson ET les bouts de caisse sont percés d'une porte, et un tunnel
# (plancher/murs/plafond) DÉPASSE le bout de caisse sous les anneaux du soufflet (qui
# couvrent [zend-0.85, zend-0.02]). Deux voitures attelées voient leurs couloirs se
# rejoindre : plus de vide ni de rails visibles entre les caisses.
CORR_HALF_W = 0.58            # demi-largeur du passage
CORR_CEIL = IN_FLOOR + 2.05   # plafond du couloir (~2,05 m de hauteur libre)
CORR_LEN = 0.92               # dépassement au-delà du bout de caisse

# --- Sièges (M22) -----------------------------------------------------------------
SEAT_W, SEAT_D = 0.46, 0.48   # assise : largeur x profondeur
SEAT_H = 0.45                 # hauteur de l'assise au-dessus du plancher
AISLE_HALF = 0.30             # allée centrale de 60 cm


# --- Algèbre ------------------------------------------------------------------
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 0.0, 1.0)


def smooth01(s):
    """Rampe C1 : 0 en deçà de 0, 1 au-delà de 1, dérivée nulle aux deux bouts."""
    s = max(0.0, min(1.0, s))
    return s * s * (3.0 - 2.0 * s)


# --- Section CONSTANTE (aucun nez), toit bombé ---------------------------------
def surf_base(z, t):
    """Superellipse |x/a|^n + |y/b|^n = 1, invariante en z (caisse-tube). Le cambrage
    ne dépend que de x : nul au point le plus large du flanc => gabarit intact."""
    cy = (ROOF + FLOOR) * 0.5
    hy = (ROOF - FLOOR) * 0.5
    ct, st = math.cos(t), math.sin(t)
    e = 2.0 / SECTION_N
    x = HALF_W * math.copysign(abs(ct) ** e, ct)
    y = cy + hy * math.copysign(abs(st) ** e, st)
    if st > 0.0:
        y += CAMBER * (1.0 - (x / HALF_W) ** 2) * (st ** (2.0 / SECTION_N))
    return (x, y, z)


def base_normal(z, t):
    """Normale de la surface de base (différences centrées : la superellipse est
    singulière aux coins). Section constante => tangente longitudinale (0,0,1)."""
    h = 1e-3
    tu = sub(surf_base(z, t + h), surf_base(z, t - h))
    tv = (0.0, 0.0, 1.0)
    n = norm(cross(tu, tv))
    center = (0.0, (ROOF + FLOOR) * 0.5, z)
    if dot(n, sub(surf_base(z, t), center)) < 0.0:
        n = mul(n, -1.0)
    return n


def channel_offset(z, t):
    """Profondeur de vitrage creusée en (z, t). La périodicité en t (2*pi) est gérée :
    un canal à cheval sur t = 0 est écrit avec des bornes négatives."""
    if not CHANNELS:
        return 0.0
    total = 0.0
    tm = t % (2.0 * math.pi)
    for (z0, z1, t0, t1, depth, rz, rt) in CHANNELS:
        if z < z0 or z > z1:
            continue
        sz = smooth01(min(z - z0, z1 - z) / rz)
        if sz <= 0.0:
            continue
        for tt in (tm, tm - 2.0 * math.pi):
            if t0 <= tt <= t1:
                total += depth * sz * smooth01(min(tt - t0, t1 - tt) / rt)
                break
    return total


def surf(z, t):
    """Surface FINALE = tôle de base rentrée le long de sa normale dans les canaux de
    vitrage. Le verre posé dessus suit le canal gratuitement."""
    return sub(surf_base(z, t), mul(base_normal(z, t), channel_offset(z, t)))


def surf_normal(z, t):
    """Normale de la surface FINALE (canaux compris), réorientée vers l'extérieur."""
    h = 1e-3
    tu = sub(surf(z, t + h), surf(z, t - h))
    tv = (0.0, 0.0, 1.0)
    n = norm(cross(tu, tv))
    center = (0.0, (ROOF + FLOOR) * 0.5, z)
    if dot(n, sub(surf(z, t), center)) < 0.0:
        n = mul(n, -1.0)
    return n


def surf_tangent(z, t, n):
    h = 1e-3
    tan = norm(sub(surf(z, t + h), surf(z, t - h)))
    dv = (0.0, 0.0, -1.0)  # v croît quand z décroît (vers l'avant)
    w = -1.0 if dot(cross(n, tan), dv) < 0.0 else 1.0
    return (tan[0], tan[1], tan[2], w)


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

    def add_quad(self, verts):
        base = len(self.positions)
        for p, n, uv, tg in verts:
            self.positions.append(p); self.normals.append(n)
            self.uvs.append(uv); self.tangents.append(tg)
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def stations():
    """Base uniforme + bords et pieds de rampe des canaux (sinon les baies seraient
    échantillonnées au hasard de la grille et déchiquetées) + bords des portes (M21 :
    le trou doit tomber PILE sur des lignes de la grille)."""
    out = [Z_TAIL + (Z_HEAD - Z_TAIL) * i / BODY_STEPS for i in range(BODY_STEPS + 1)]
    for (z0, z1, _, _, _, rz, _) in CHANNELS:
        out += [z0, z0 + rz, (z0 + z1) / 2.0, z1 - rz, z1]
    for (z0, z1, _, _) in DOORS:
        out += [z0, z1]
    return sorted({round(z, 6) for z in out if Z_HEAD <= z <= Z_TAIL}, reverse=True)


def t_columns():
    """Colonnes angulaires : base uniforme + bords et pieds de rampe des canaux +
    bords des portes."""
    cols = {round(2.0 * math.pi * j / T_SEGMENTS, 6) for j in range(T_SEGMENTS)}
    two_pi = 2.0 * math.pi
    for (_, _, t0, t1, _, _, rt) in CHANNELS:
        for tt in (t0, t0 + rt, (t0 + t1) / 2.0, t1 - rt, t1):
            cols.add(round(tt % two_pi, 6))
    for (_, _, t0, t1) in DOORS:
        cols.add(round(t0 % two_pi, 6))
        cols.add(round(t1 % two_pi, 6))
    return sorted(cols)


def in_door(z, t):
    """Le centre (z, t) d'un quad tombe-t-il dans l'embrasure d'une porte ? La
    périodicité en t est gérée comme pour les canaux (la porte droite chevauche t = 0).
    Les bords de porte étant des lignes de la grille, un centre est TOUJOURS
    strictement dedans ou strictement dehors : le trou est net."""
    tm = t % (2.0 * math.pi)
    for (z0, z1, t0, t1) in DOORS:
        if z0 < z < z1:
            for tt in (tm, tm - 2.0 * math.pi):
                if t0 < tt < t1:
                    return True
    return False


def build_body(part):
    """Grille INDEXÉE à normales partagées (éclairage continu). La dernière colonne
    (t = 2pi) duplique la première GÉOMÉTRIQUEMENT mais porte u = 1 (couture UV).
    M21 : les quads dont le centre tombe dans une embrasure de porte sont SAUTÉS —
    c'est le trou dans la tôle."""
    zs = stations()
    cols = t_columns()
    two_pi = 2.0 * math.pi
    grid = []
    for z in zs:
        row = []
        for t in cols + [two_pi]:
            n = surf_normal(z, t)
            row.append((surf(z, t), n, (t / two_pi, (Z_TAIL - z) / BODY_LEN),
                        surf_tangent(z, t, n)))
        grid.append(row)
    for i in range(len(zs) - 1):
        for j in range(len(cols)):
            zc = (zs[i] + zs[i + 1]) * 0.5
            t_hi = cols[j + 1] if j + 1 < len(cols) else two_pi
            tc = (cols[j] + t_hi) * 0.5
            if in_door(zc, tc):
                continue
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])


def build_cap(part, z, normal_z):
    """Obture un bout de caisse, PERCÉ de la porte d'intercirculation (M22) : au lieu
    d'un éventail plein, un ANNEAU plat entre le contour superellipse et le rectangle
    du passage. Tout est dans le même plan => les quads sont parfaitement plats, et le
    raccord angulaire contour/rectangle ne peut pas se plier."""
    yc = (IN_FLOOR + CORR_CEIL) * 0.5
    hx, hy = CORR_HALF_W, (CORR_CEIL - IN_FLOOR) * 0.5

    def hole_point(t):
        """Point du rectangle de passage dans la direction t (rayon depuis le centre
        du rectangle) : le bord intérieur de l'anneau suit le contour, quad par quad."""
        ct, st = math.cos(t), math.sin(t)
        d = min(hx / max(abs(ct), 1e-9), hy / max(abs(st), 1e-9))
        return (d * ct, yc + d * st, z)

    n = (0.0, 0.0, normal_z)
    tg = (1.0, 0.0, 0.0, 1.0)
    for j in range(T_SEGMENTS):
        t0 = 2.0 * math.pi * j / T_SEGMENTS
        t1 = 2.0 * math.pi * (j + 1) / T_SEGMENTS
        o0, o1 = surf(z, t0), surf(z, t1)
        r0, r1 = hole_point(t0), hole_point(t1)
        quad = [o0, o1, r1, r0]
        # Winding forcé du côté de la normale voulue (l'anneau est plan : le test est fiable).
        if cross(sub(o1, o0), sub(r1, o0))[2] * normal_z < 0.0:
            quad.reverse()
        base = len(part.positions)
        for p in quad:
            part.positions.append(p); part.normals.append(n)
            part.uvs.append((0.5 + p[0] / (4.0 * HALF_W), 0.5 + p[1] / (4.0 * HALF_W)))
            part.tangents.append(tg)
        part.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def build_band(part, z0, z1, t0, t1, nz, nt, offset, flip=False):
    """Bandeau plaqué sur la surface FINALE, décalé de `offset` le long de la normale
    (anti z-fighting). Posé sur un canal de vitrage, il épouse le creusement.
    flip=True inverse winding et normales : peau INTÉRIEURE d'un panneau (M22, battants
    de porte lus depuis l'habitacle)."""
    grid = []
    for i in range(nz + 1):
        z = z0 + (z1 - z0) * i / nz
        row = []
        for j in range(nt + 1):
            t = t0 + (t1 - t0) * j / nt
            n = surf_normal(z, t)
            p = add(surf(z, t), mul(n, offset))
            row.append((p, n, (j / nt, i / nz), surf_tangent(z, t, n)))
        grid.append(row)
    for i in range(nz):
        for j in range(nt):
            quad = [grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]]
            if flip:
                quad = [(p, mul(n, -1.0), uv, (-tg[0], -tg[1], -tg[2], tg[3]))
                        for p, n, uv, tg in reversed(quad)]
            part.add_quad(quad)


def build_blister(part, z0, z1, t0, t1, height, nz=16, nt=8):
    """Carénage de toit : bosse sinusoïdale (nulle aux 4 bords => fusion douce avec le
    toit). Les normales sont redérivées de la surface bosselée."""

    def point(z, t):
        uz = (z - z0) / (z1 - z0)
        ut = (t - t0) / (t1 - t0)
        off = height * math.sin(math.pi * uz) * math.sin(math.pi * ut)
        return add(surf(z, t), mul(surf_normal(z, t), off))

    def pnormal(z, t):
        h = 1e-3
        tu = sub(point(z, t + h), point(z, t - h))
        tv = sub(point(min(z + h, Z_TAIL), t), point(max(z - h, Z_HEAD), t))
        n = norm(cross(tu, tv))
        if dot(n, surf_normal(z, t)) < 0.0:
            n = mul(n, -1.0)
        return n

    grid = []
    for i in range(nz + 1):
        z = z0 + (z1 - z0) * i / nz
        row = []
        for j in range(nt + 1):
            t = t0 + (t1 - t0) * j / nt
            n = pnormal(z, t)
            row.append((point(z, t), n, (j / nt, i / nz), surf_tangent(z, t, n)))
        grid.append(row)
    for i in range(nz):
        for j in range(nt):
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])


def build_box(part, x0, y0, z0, x1, y1, z1):
    c = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    faces = [((4, 5, 6, 7), (0, 0, 1)), ((1, 0, 3, 2), (0, 0, -1)),
             ((5, 1, 2, 6), (1, 0, 0)), ((0, 4, 7, 3), (-1, 0, 0)),
             ((3, 7, 6, 2), (0, 1, 0)), ((0, 1, 5, 4), (0, -1, 0))]
    for idx, n in faces:
        t = (0.0, 0.0, 1.0) if abs(n[2]) < 0.9 else (1.0, 0.0, 0.0)
        tg = (t[0], t[1], t[2], 1.0)
        base = len(part.positions)
        for k in idx:
            part.positions.append(c[k]); part.normals.append(n)
            part.uvs.append((0.0, 0.0)); part.tangents.append(tg)
        part.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


# --- Habitacle et embrasures (M21) ----------------------------------------------
def quad_orient(part, a, b, c, d, want):
    """Un quad plan (a,b,c,d) dont on FORCE l'orientation : si la normale géométrique
    ne pointe pas du côté de `want`, le winding est inversé. Sert au caisson intérieur
    (normales RENTRANTES, visibles de l'intérieur) et aux feuillures (normales tournées
    vers le passage de porte)."""
    n = norm(cross(sub(b, a), sub(c, a)))
    if dot(n, want) < 0.0:
        b, d = d, b
        n = mul(n, -1.0)
    t3 = norm(sub(c, b))
    tg = (t3[0], t3[1], t3[2], 1.0)
    part.add_quad([(a, n, (0.0, 0.0), tg), (b, n, (1.0, 0.0), tg),
                   (c, n, (1.0, 1.0), tg), (d, n, (0.0, 1.0), tg)])


def door_heights():
    """Hauteurs (seuil, linteau) de l'embrasure en mètres, évaluées sur la géométrie
    RÉELLE de la section (surf_base) aux bords angulaires de la porte. Identiques sur
    les deux flancs : sin(pi - t) = sin(t)."""
    return surf_base(0.0, DOOR_T_LO)[1], surf_base(0.0, DOOR_T_HI)[1]


def build_interior(part):
    """Caisson intérieur rudimentaire à normales RENTRANTES : plancher, plafond, murs
    latéraux, fonds aux deux bouts (rentrés de 5 cm pour ne pas z-fighter avec les
    caps). Le long de chaque flanc, le mur est OUVERT au droit de la porte : deux
    segments pleine hauteur (avant / après la porte) + deux bandeaux (sous le seuil,
    au-dessus du linteau) laissent un passage rectangulaire aligné sur le trou de la
    coque."""
    seuil, linteau = door_heights()
    zA, zB = Z_HEAD + 0.05, Z_TAIL - 0.05
    w = IN_HALF_W
    # Plancher et plafond (toute la longueur, toute la largeur).
    quad_orient(part, (-w, IN_FLOOR, zA), (-w, IN_FLOOR, zB),
                (w, IN_FLOOR, zB), (w, IN_FLOOR, zA), (0.0, 1.0, 0.0))
    quad_orient(part, (-w, IN_CEIL, zA), (w, IN_CEIL, zA),
                (w, IN_CEIL, zB), (-w, IN_CEIL, zB), (0.0, -1.0, 0.0))
    # Fonds aux deux bouts, PERCÉS de la porte d'intercirculation (M22) : deux segments
    # pleine hauteur + un linteau au-dessus du passage, alignés pile sur le trou du cap.
    for ze, want in ((zA, (0.0, 0.0, 1.0)), (zB, (0.0, 0.0, -1.0))):
        for xa, xb in ((-w, -CORR_HALF_W), (CORR_HALF_W, w)):
            quad_orient(part, (xa, IN_FLOOR, ze), (xb, IN_FLOOR, ze),
                        (xb, IN_CEIL, ze), (xa, IN_CEIL, ze), want)
        quad_orient(part, (-CORR_HALF_W, CORR_CEIL, ze), (CORR_HALF_W, CORR_CEIL, ze),
                    (CORR_HALF_W, IN_CEIL, ze), (-CORR_HALF_W, IN_CEIL, ze), want)
    # Murs latéraux percés au droit des portes.
    for sx, want in ((1.0, (-1.0, 0.0, 0.0)), (-1.0, (1.0, 0.0, 0.0))):
        x = sx * w
        for za, zb in ((zA, DOOR_Z0), (DOOR_Z1, zB)):  # segments pleine hauteur
            quad_orient(part, (x, IN_FLOOR, za), (x, IN_FLOOR, zb),
                        (x, IN_CEIL, zb), (x, IN_CEIL, za), want)
        # bandeaux sous le seuil et au-dessus du linteau
        quad_orient(part, (x, IN_FLOOR, DOOR_Z0), (x, IN_FLOOR, DOOR_Z1),
                    (x, seuil, DOOR_Z1), (x, seuil, DOOR_Z0), want)
        quad_orient(part, (x, linteau, DOOR_Z0), (x, linteau, DOOR_Z1),
                    (x, IN_CEIL, DOOR_Z1), (x, IN_CEIL, DOOR_Z0), want)


def build_door(part, side):
    """Battant AFFLEURANT (M22) : un patch de la surface de coque EXACTE, échantillonné
    pile sur le rectangle de l'embrasure (même superellipse, même cambrage) et rentré de
    4 mm sous la tôle pour ne pas z-fighter avec les bords du trou. Fermé, l'emboîtement
    est au millimètre — aucun jour visible ; ouvert, l'app translate le battant (bouchon
    puis coulissement) et, la section étant CONSTANTE sur toute la longueur, le patch
    reste affleurant sur toute la course. Une peau intérieure à -20 mm (winding inversé)
    habille le battant vu de l'habitacle.
    side = +1 (flanc droit) ou -1 (flanc gauche)."""
    t0, t1 = ((DOOR_T_LO, DOOR_T_HI) if side > 0.0
              else (math.pi - DOOR_T_HI, math.pi - DOOR_T_LO))
    build_band(part, DOOR_Z0, DOOR_Z1, t0, t1, 10, 12, -0.004)
    build_band(part, DOOR_Z0, DOOR_Z1, t0, t1, 10, 12, -0.020, flip=True)


def build_jambs(part):
    """Feuillures d'embrasure (M21) : 4 quads par porte reliant le bord du trou de la
    coque (surface extérieure) au mur intérieur — sans elles on verrait le vide entre
    coque et caisson quand la porte est ouverte. Matériau PEINTURE : l'embrasure est de
    la tôle emboutie, peinte comme la caisse, et c'est elle qu'on voit de l'extérieur
    quand le battant a coulissé."""
    seuil, linteau = door_heights()
    for (z0, z1, t0, t1) in DOORS:
        side = 1.0 if math.cos((t0 + t1) * 0.5) > 0.0 else -1.0
        xw = side * IN_HALF_W
        # seuil (normale vers le haut) et linteau (vers le bas)
        quad_orient(part, surf(z0, t0), surf(z1, t0),
                    (xw, seuil, z1), (xw, seuil, z0), (0.0, 1.0, 0.0))
        quad_orient(part, surf(z0, t1), surf(z1, t1),
                    (xw, linteau, z1), (xw, linteau, z0), (0.0, -1.0, 0.0))
        # montants avant / arrière (normales vers l'intérieur du passage, en z)
        quad_orient(part, surf(z0, t0), surf(z0, t1),
                    (xw, linteau, z0), (xw, seuil, z0), (0.0, 0.0, 1.0))
        quad_orient(part, surf(z1, t0), surf(z1, t1),
                    (xw, linteau, z1), (xw, seuil, z1), (0.0, 0.0, -1.0))


def build_corridor(part):
    """Couloir d'intercirculation (M22) : un tunnel à normales RENTRANTES à chaque bout,
    du fond de caisson (rentré de 5 cm) jusqu'à CORR_LEN AU-DELÀ du bout de caisse, sous
    les anneaux du soufflet. Vu de l'intérieur, on enchaîne sur la voiture suivante sans
    jamais voir le vide ; vu de l'extérieur, le tunnel est masqué par les anneaux."""
    w = CORR_HALF_W
    for zend, sgn in ((Z_TAIL, 1.0), (Z_HEAD, -1.0)):
        z0 = zend - sgn * 0.05            # fond de caisson (zA / zB)
        z1 = zend + sgn * CORR_LEN        # ras de l'anneau extérieur du soufflet
        # Plancher (normale vers le haut) et plafond (vers le bas).
        quad_orient(part, (-w, IN_FLOOR, z0), (-w, IN_FLOOR, z1),
                    (w, IN_FLOOR, z1), (w, IN_FLOOR, z0), (0.0, 1.0, 0.0))
        quad_orient(part, (-w, CORR_CEIL, z0), (w, CORR_CEIL, z0),
                    (w, CORR_CEIL, z1), (-w, CORR_CEIL, z1), (0.0, -1.0, 0.0))
        # Murs (normales vers l'axe du couloir).
        quad_orient(part, (w, IN_FLOOR, z0), (w, IN_FLOOR, z1),
                    (w, CORR_CEIL, z1), (w, CORR_CEIL, z0), (-1.0, 0.0, 0.0))
        quad_orient(part, (-w, IN_FLOOR, z0), (-w, IN_FLOOR, z1),
                    (-w, CORR_CEIL, z1), (-w, CORR_CEIL, z0), (1.0, 0.0, 0.0))


def build_seat(part, xc, zc, facing):
    """Un siège en L (M22) : pied central, assise, dossier. facing = -1 => le voyageur
    regarde vers l'avant (-z) et le dossier est du côté +z ; +1 => l'inverse."""
    build_box(part, xc - 0.05, IN_FLOOR, zc - 0.05,
              xc + 0.05, IN_FLOOR + SEAT_H - 0.09, zc + 0.05)
    y = IN_FLOOR + SEAT_H
    build_box(part, xc - SEAT_W / 2.0, y - 0.09, zc - SEAT_D / 2.0,
              xc + SEAT_W / 2.0, y, zc + SEAT_D / 2.0)
    # Dossier (9 cm d'épaisseur), plaqué du côté ARRIÈRE de l'assise.
    z_back = zc - facing * (SEAT_D / 2.0)
    z0, z1 = sorted((z_back, z_back + facing * 0.09))
    build_box(part, xc - SEAT_W / 2.0, y, z0, xc + SEAT_W / 2.0, y + 0.62, z1)


def build_seats(part):
    """Rangées de 4 sièges (2+2 de part et d'autre de l'allée centrale), pas de 95 cm.
    La moitié avant de la voiture regarde vers l'avant, la moitié arrière vers l'arrière
    — disposition réelle, les voyageurs se font face au milieu. Les rangées commencent
    APRÈS la plateforme des portes et s'arrêtent avant les fonds d'intercirculation."""
    pitch = 0.95
    x_in = AISLE_HALF + SEAT_W / 2.0                      # siège côté allée
    x_out = AISLE_HALF + SEAT_W + 0.04 + SEAT_W / 2.0     # siège côté fenêtre
    z = Z_HEAD + 2.70
    while z <= Z_TAIL - 1.60:
        facing = -1.0 if z < 0.0 else 1.0
        for sx in (1.0, -1.0):
            for xoff in (x_in, x_out):
                build_seat(part, sx * xoff, z, facing)
        z += pitch


# --- Bogie Jacobs (organe partagé) --------------------------------------------
# Cotes RÉELLES SNCF (M22) : roues de 920 mm de diamètre, voie normale de 1435 mm.
WHEEL_R = 0.46          # Ø 920 mm
GAUGE_HALF = 0.7175     # 1435 mm entre les faces internes des rails
FLANGE_R = 0.505        # boudin : il plonge SOUS le niveau du champignon, entre les rails
AXLE_Z = (-1.5, 1.5)    # empattement 3 m (convention partagée avec l'app C++)


def build_wheel(part, cx, segments=24):
    """Roue réelle centrée sur l'AXE de l'essieu (moyeu à l'ORIGINE locale, axe = x) :
    bande de roulement à WHEEL_R, flancs en éventail, et boudin (FLANGE_R) du côté
    INTÉRIEUR (vers le milieu de la voie). Centrée à l'origine pour que l'app puisse
    appliquer la rotation de roulement autour de X sans translation parasite."""
    inner = -1.0 if cx > 0.0 else 1.0   # sens x de la face INTÉRIEURE de la roue
    xo = cx + inner * 0.0675            # face extérieure (bande de roulement de 135 mm)
    xi = cx - inner * 0.0675            # face intérieure
    xf = xi - inner * 0.025             # boudin : 25 mm au-delà de la face intérieure

    def pt(x, r, a):
        return (x, r * math.sin(a), r * math.cos(a))

    for j in range(segments):
        a0 = 2.0 * math.pi * j / segments
        a1 = 2.0 * math.pi * (j + 1) / segments
        n0 = (0.0, math.sin(a0), math.cos(a0))
        n1 = (0.0, math.sin(a1), math.cos(a1))
        # Bande de roulement (rayon WHEEL_R, de la face ext. à la face int.).
        part.add_quad([(pt(xo, WHEEL_R, a0), n0, (0, 0), (1, 0, 0, 1)),
                       (pt(xi, WHEEL_R, a0), n0, (1, 0), (1, 0, 0, 1)),
                       (pt(xi, WHEEL_R, a1), n1, (1, 1), (1, 0, 0, 1)),
                       (pt(xo, WHEEL_R, a1), n1, (0, 1), (1, 0, 0, 1))])
        # Boudin (rayon FLANGE_R, de la face int. vers l'intérieur).
        part.add_quad([(pt(xi, FLANGE_R, a0), n0, (0, 0), (1, 0, 0, 1)),
                       (pt(xf, FLANGE_R, a0), n0, (1, 0), (1, 0, 0, 1)),
                       (pt(xf, FLANGE_R, a1), n1, (1, 1), (1, 0, 0, 1)),
                       (pt(xi, FLANGE_R, a1), n1, (0, 1), (1, 0, 0, 1))])
        # Raccord bande -> boudin : anneau sur la face intérieure (normale vers -inner).
        na = (-inner, 0.0, 0.0)
        quad = [(pt(xi, WHEEL_R, a0), na, (0, 0), (0, 0, 1, 1)),
                (pt(xi, WHEEL_R, a1), na, (1, 0), (0, 0, 1, 1)),
                (pt(xi, FLANGE_R, a1), na, (1, 1), (0, 0, 1, 1)),
                (pt(xi, FLANGE_R, a0), na, (0, 1), (0, 0, 1, 1))]
        if inner < 0.0:
            quad.reverse()
        part.add_quad(quad)
        # Flancs : éventails vers le centre (extérieur à rayon WHEEL_R, intérieur à
        # rayon FLANGE_R pour fermer le boudin).
        for fx, fr, nx in ((xo, WHEEL_R, inner), (xf, FLANGE_R, -inner)):
            cen = (fx, 0.0, 0.0)
            e0 = pt(fx, fr, a0)
            e1 = pt(fx, fr, a1)
            a, b = (e0, e1) if nx > 0 else (e1, e0)
            base = len(part.positions)
            for p in (cen, a, b):
                part.positions.append(p); part.normals.append((nx, 0.0, 0.0))
                part.uvs.append((0.0, 0.0)); part.tangents.append((0.0, 0.0, 1.0, 1.0))
            part.indices.extend([base, base + 1, base + 2])


def build_axle(part, segments=12):
    """Essieu complet, CENTRÉ À L'ORIGINE (axe = x local) : arbre + 2 roues Ø920 à
    boudins. L'app place chaque essieu à (0, WHEEL_R, ±1.5) dans le repère bogie et lui
    applique la rotation de roulement (v = omega * r, cf. Bogie::wheel_angle)."""
    # Arbre : cylindre de 17 cm entre les deux roues.
    shaft_r, shaft_x = 0.085, GAUGE_HALF - 0.07
    for j in range(segments):
        a0 = 2.0 * math.pi * j / segments
        a1 = 2.0 * math.pi * (j + 1) / segments
        n0 = (0.0, math.sin(a0), math.cos(a0))
        n1 = (0.0, math.sin(a1), math.cos(a1))
        part.add_quad([((-shaft_x, shaft_r * math.sin(a0), shaft_r * math.cos(a0)), n0, (0, 0), (1, 0, 0, 1)),
                       ((shaft_x, shaft_r * math.sin(a0), shaft_r * math.cos(a0)), n0, (1, 0), (1, 0, 0, 1)),
                       ((shaft_x, shaft_r * math.sin(a1), shaft_r * math.cos(a1)), n1, (1, 1), (1, 0, 0, 1)),
                       ((-shaft_x, shaft_r * math.sin(a1), shaft_r * math.cos(a1)), n1, (0, 1), (1, 0, 0, 1))])
    for cx in (-GAUGE_HALF, GAUGE_HALF):
        build_wheel(part, cx)


# --- Sérialisation glTF binaire (mutualisée) ----------------------------------
def align4(n): return (n + 3) & ~3


def write_glb(path, parts, node_name):
    used = [(i, p) for i, p in enumerate(parts) if p.positions]
    blocks, part_blocks = [], []
    for _, p in used:
        b = (b"".join(struct.pack("<fff", *v) for v in p.positions),
             b"".join(struct.pack("<fff", *v) for v in p.normals),
             b"".join(struct.pack("<ff", *v) for v in p.uvs),
             b"".join(struct.pack("<ffff", *v) for v in p.tangents),
             b"".join(struct.pack("<I", i) for i in p.indices))
        part_blocks.append(b); blocks.extend(b)
    offsets, cur = [], 0
    for blk in blocks:
        offsets.append(cur); cur = align4(cur + len(blk))
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
        materials.append(m)

    gltf = {"asset": {"version": "2.0", "generator": "noire-tgv-voiture-v2 (CC0)"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": node_name}],
            "meshes": [{"primitives": primitives}], "materials": materials,
            "accessors": accessors, "bufferViews": buffer_views, "buffers": [{"byteLength": total}]}
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))
    bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)
    glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, glb_len))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A)); f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_pad), 0x004E4942)); f.write(bin_pad)
    nv = sum(len(p.positions) for _, p in used)
    print(f"{path} : {nv} sommets, {glb_len} o ({node_name})")


# --- Voiture voyageurs V2 -------------------------------------------------------
def build_car(out):
    parts = [Part() for _ in MATERIALS]
    # 1) Caisse-tube loftée à toit bombé, canaux de vitrage creusés, bouts obturés.
    build_body(parts[MAT_PAINT])
    build_cap(parts[MAT_PAINT], Z_TAIL, 1.0)
    build_cap(parts[MAT_PAINT], Z_HEAD, -1.0)
    # 2) Verre posé DANS les canaux (+8 mm). Il couvre aussi les rampes : le bandeau
    #    réel est entièrement noir (verre + sérigraphie de bordure). Échantillonnage
    #    dense (8 cm) pour que les cordes des quads suivent les rampes sans dépasser.
    for (z0, z1, t0, t1, _, _, _) in CHANNELS:
        nz = max(8, int(math.ceil((z1 - z0) / 0.08)))
        nt = max(4, int(math.ceil((t1 - t0) * 1.5 / 0.08)))
        build_band(parts[MAT_GLASS], z0, z1, t0, t1, nz, nt, 0.008)
    # 3) Filet bleu nuit sous le bandeau, plaqué.
    for lo, hi in ((-0.46, -0.33), (math.pi + 0.33, math.pi + 0.46)):
        build_band(parts[MAT_ACCENT], Z_HEAD + 1.2, Z_TAIL - 1.2, lo, hi, 24, 1, 0.012)
    # 4) Jupe de bas de caisse enroulée sous le plancher (t = 3pi/2 = bas de caisse).
    build_band(parts[MAT_SKIRT], Z_HEAD + 1.0, Z_TAIL - 1.0,
               math.pi + 0.85, 2.0 * math.pi - 0.85, 40, 4, 0.014)
    # 5) Carénages de toit : climatisation au centre, et au-dessus des bogies Jacobs
    #    (aux deux bouts de la caisse — c'est là qu'elle porte sur l'articulation).
    build_blister(parts[MAT_PAINT], -6.0, 6.0, math.pi / 2 - 0.55, math.pi / 2 + 0.55, 0.24)
    for zend in (Z_HEAD, Z_TAIL):
        z0, z1 = (zend + 0.6, zend + 3.4) if zend < 0 else (zend - 3.4, zend - 0.6)
        build_blister(parts[MAT_PAINT], z0, z1, math.pi / 2 - 0.50, math.pi / 2 + 0.50, 0.22)
    # 6) Soufflets d'intercirculation : fond sombre + 3 anneaux saillants à chaque bout.
    for zend, sgn in ((Z_TAIL, 1.0), (Z_HEAD, -1.0)):
        build_band(parts[MAT_BELLOWS], zend - sgn * 0.85, zend - sgn * 0.02,
                   0.0, 2.0 * math.pi, 4, 40, 0.010)
        for k in range(3):
            zc = zend - sgn * (0.18 + k * 0.26)
            build_band(parts[MAT_BELLOWS], zc - 0.05, zc + 0.05,
                       0.0, 2.0 * math.pi, 1, 40, 0.035)
    # 7) Habitacle rudimentaire (M21) : caisson intérieur à normales RENTRANTES (plancher,
    #    plafond, murs latéraux percés au droit des portes, fonds avant/arrière).
    build_interior(parts[MAT_INTERIOR])
    # 7b) Sièges voyageurs (M22) : rangées de 4 (2+2) de part et d'autre de l'allée centrale.
    build_seats(parts[MAT_SEAT])
    # 7c) Couloirs d'intercirculation (M22) : tunnel sous les soufflets, à chaque bout.
    build_corridor(parts[MAT_INTERIOR])
    # 8) Feuillures d'embrasure (M21) : relient le bord du trou de coque au mur intérieur.
    #    Matériau peinture (tôle emboutie, même teinte que la carrosserie).
    build_jambs(parts[MAT_PAINT])
    # 9) Battants de porte SÉPARÉS (M21) — DOIVENT être les deux DERNIÈRES parts sérialisées.
    #    L'app C++ anime `voiture_model->primitives[N-2]` (porte droite) et
    #    `voiture_model->primitives[N-1]` (porte gauche) avec leur propre matrice de DrawItem
    #    (translation bouchon + coulissement), indépendamment de la matrice caisse.
    build_door(parts[MAT_DOOR_R], side=+1.0)   # flanc droit  (N-2)
    build_door(parts[MAT_DOOR_L], side=-1.0)   # flanc gauche (N-1)
    write_glb(out, parts, "TGV_voiture")



# --- Bogie Jacobs (organe partagé) --------------------------------------------
# M22 : le châssis est une part, et les deux ESSIEUX sont des parts SÉPARÉES centrées
# à l'ORIGINE locale (axe = x) — l'app C++ les place à (0, WHEEL_R, ±1.5) dans le
# repère bogie et leur applique la rotation de roulement (v = omega * r, cf.
# Bogie::wheel_angle). Les 2 dernières primitives du GLB sont les essieux, convention
# identique aux battants de porte de la voiture.
def build_bogie(out):
    parts = [Part() for _ in MATERIALS]
    # Châssis : au-dessus des roues, plus étroit que la voie (les roues doivent dépasser).
    build_box(parts[MAT_BOGIE], -0.45, WHEEL_R + 0.10, -1.9, 0.45, WHEEL_R + 0.85, 1.9)
    # Essieu A (z = -1.5) et B (z = +1.5), chacun centré à l'origine : l'app le translate
    # à (0, WHEEL_R, ±1.5) et applique la rotation. Convention : les 2 DERNIÈRES parts
    # sérialisées sont les essieux.
    build_axle(parts[MAT_AXLE_A])
    build_axle(parts[MAT_AXLE_B])
    write_glb(out, parts, "TGV_bogie_jacobs")


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    build_car(f"{outdir}/tgv_voiture.glb")
    build_bogie(f"{outdir}/tgv_bogie.glb")
