#!/usr/bin/env python3
"""Sculpte une motrice TGV stylisée V2, entièrement par équations, au format .glb (M20).

PRINCIPE — surface LOFTÉE, pas un assemblage de boîtes :
  * une SECTION transversale paramétrée par un angle t (superellipse : entre l'ellipse
    et le rectangle, c'est la forme d'une caisse ferroviaire) ;
  * un TOIT BOMBÉ : la moitié haute est rehaussée d'un cambrage qui ne s'annule qu'au
    point le plus large du flanc — raccord lisse, gabarit latéral intact ;
  * dont la largeur, le plancher et le toit varient le long de z selon des COURBES DE
    BÉZIER cubiques — c'est ce qui sculpte le nez plongeant (8 m, museau-lame à 1.55 m) ;
  * des CANAUX DE VITRAGE CREUSÉS DANS LA TÔLE : la surface de base est rentrée le long
    de sa normale dans les rectangles de baies (rampes douces), et le verre est posé
    DANS le canal. Creuser le verre seul derrière une tôle pleine le rendrait invisible.
  * les normales sont dérivées par différences centrées sur la surface FINALE (canaux
    compris) et partagées : la lumière s'enroule sur le nez et dans les rampes de baies.

DÉTAILS ARCHITECTURAUX : jupe de bas de caisse gris mat enroulée sous le plancher,
filet bleu nuit, carénages de toit sinusoïdaux au droit des bogies et du pantographe.

Repère = repère LOCAL de la caisse du moteur Noire :
  x = droite, y = haut, z = arrière (l'app le dessine via body_position * body_orientation).
  origine = centre de la caisse ; le PLAN DE ROULEMENT est à y = -body_height (= -2.20 m).
Aucune dépendance externe (struct/json/math de la stdlib)."""
import struct, json, math, sys

# --- Cotes réelles d'une motrice TGV, en mètres --------------------------------
RAIL = -2.20            # plan de roulement dans le repère caisse (= -body_height)
LENGTH = 22.15          # longueur hors tampons
HALF_W = 1.45           # 2.90 m de large (gabarit UIC)
CAMBER = 0.28           # flèche du toit bombé : le centre touche 4.10 m, pas les rives
ROOF = RAIL + 4.10 - CAMBER  # section de base ; ROOF + CAMBER = 4.10 m au-dessus du rail
FLOOR = RAIL + 1.05     # bas de caisse
NOSE_LEN = 8.00         # nez long, façon Shinkansen (l'avant est en -Z)
TIP_Y = RAIL + 1.55     # hauteur du museau : une lame basse, signature TGV

Z_TAIL = LENGTH / 2.0
Z_TIP = -LENGTH / 2.0
Z_NOSE = Z_TIP + NOSE_LEN  # abscisse où le nez commence à se former

# Exposant de la superellipse : 2 = ellipse, +inf = rectangle. 4 donne le rectangle
# arrondi d'une caisse ferroviaire.
SECTION_N = 4.0
T_SEGMENTS = 40         # facettes de base autour de la section
BODY_STEPS = 6          # stations de base le long du corps droit
NOSE_STEPS = 30         # stations de base dans le nez

# --- Canaux de vitrage creusés dans la tôle -------------------------------------
# (z0, z1, t0, t1, profondeur, rampe_z, rampe_t). Le rectangle [z0,z1]x[t0,t1] descend
# en rampe douce jusqu'à -profondeur ; le verre est ensuite posé dans le canal (à fleur,
# +8 mm anti z-fighting). t = 0 : flanc droit ; pi : flanc gauche ; pi/2 : toit.
WIN_RZ, WIN_RT = 0.30, 0.10
GLASS_DEPTH = 0.030
CHANNELS = [
    # Pare-brise : dans la face plongeante du nez, en travers du toit. Son bord haut
    # s'arrête AVANT le début du corps droit (Z_NOSE) : le verre ne doit pas déborder
    # sur le toit de la caisse — sur le vrai matériel, la baie finit au « sourcil ».
    (Z_TIP + 3.20, Z_NOSE - 0.80, math.pi / 2 - 0.66, math.pi / 2 + 0.66, 0.045, 0.35, 0.12),
    # Vitres latérales de cabine (haut du flanc, juste derrière le pare-brise).
    (Z_NOSE + 0.50, Z_NOSE + 2.90, 0.30, 0.75, 0.035, WIN_RZ, WIN_RT),
    (Z_NOSE + 0.50, Z_NOSE + 2.90, math.pi - 0.75, math.pi - 0.30, 0.035, WIN_RZ, WIN_RT),
    # Bandeau longitudinal de la motrice (équipements), les deux flancs.
    (Z_NOSE + 3.60, Z_TAIL - 1.60, -0.02, 0.30, GLASS_DEPTH, WIN_RZ, WIN_RT),
    (Z_NOSE + 3.60, Z_TAIL - 1.60, math.pi - 0.30, math.pi + 0.02, GLASS_DEPTH, WIN_RZ, WIN_RT),
]

# --- Matériaux PBR (convention glTF metallic-roughness) ------------------------
# Rappel : metallic = 1 => la baseColor est la couleur de RÉFLEXION (F0), pas un pigment.
#          metallic = 0 => F0 = 0.04 fixe, la baseColor est le pigment diffus.
# Les FACTEURS glTF sont LINÉAIRES.
MATERIALS = [
    # Carrosserie argent : metallic 0.55 est volontairement NON physique (un vrai
    # matériau est 0 ou 1). C'est la triche classique de la peinture métallisée, dont
    # les paillettes d'aluminium se comportent en métal sans que la surface entière en
    # soit une. C'est elle qui fera courir le ciel le long du nez.
    {"name": "peinture", "factor": [0.82, 0.83, 0.86, 1.0], "metallic": 0.55, "roughness": 0.24},
    # Vitrage : diélectrique, surtout PAS métallique — un baseColor noir + metallic élevé
    # donnerait F0 ~ 0.008, une surface qui ne réfléchit RIEN (l'inverse du verre).
    {"name": "vitrage", "factor": [0.015, 0.020, 0.028, 1.0], "metallic": 0.0, "roughness": 0.05},
    # Filet bleu nuit (livrée inOui) sous les vitres.
    {"name": "accent", "factor": [0.045, 0.075, 0.16, 1.0], "metallic": 0.35, "roughness": 0.32},
    # Jupe de bas de caisse : gris MAT et rugueux (plastique peint, poussière de frein).
    {"name": "jupe", "factor": [0.24, 0.25, 0.27, 1.0], "metallic": 0.0, "roughness": 0.72},
    # Caoutchouc sombre (soufflets d'intercirculation — utilisé par la voiture).
    {"name": "soufflet", "factor": [0.05, 0.05, 0.055, 1.0], "metallic": 0.0, "roughness": 0.85},
]
MAT_PAINT, MAT_GLASS, MAT_ACCENT, MAT_SKIRT, MAT_BELLOWS = range(5)


# --- Petite algèbre vectorielle ------------------------------------------------
def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 0.0, 1.0)


def bez(p0, p1, p2, p3, s):
    """Bézier cubique SCALAIRE, paramétrée directement par s. Le contrôle P1 aligné sur
    P0 donne une tangente nulle au départ : c'est ce qui raccorde le nez au toit droit
    sans cassure visible."""
    u = 1.0 - s
    return u * u * u * p0 + 3.0 * u * u * s * p1 + 3.0 * u * s * s * p2 + s * s * s * p3


def smooth01(s):
    """Rampe C1 : 0 en deçà de 0, 1 au-delà de 1, dérivée nulle aux deux bouts."""
    s = max(0.0, min(1.0, s))
    return s * s * (3.0 - 2.0 * s)


# --- Le profil longitudinal : c'est ici qu'est sculpté le nez ------------------
def section(z):
    """(demi-largeur, plancher, toit de base) à l'abscisse z. Constant sur le corps,
    gouverné par trois Béziers dans le nez."""
    if z >= Z_NOSE:
        return HALF_W, FLOOR, ROOF
    s = (Z_NOSE - z) / NOSE_LEN  # 0 à la base du nez, 1 au museau
    # Toit : plat au départ (P1 = P0 => raccord lisse), plongée franche, puis
    # aplatissement sur le museau (P2 proche de P3).
    roof = bez(ROOF, ROOF, TIP_Y + 0.85, TIP_Y, s)
    # Plancher : remonte à peine — le dessous du nez reste presque horizontal, c'est
    # ce qui donne la « lame » plutôt qu'une proue de bateau.
    floor = bez(FLOOR, FLOOR, FLOOR + 0.08, FLOOR + 0.25, s)
    # Largeur : tient très longtemps puis s'effile. Le museau n'est PAS une pointe
    # (0.13) mais une lame arrondie, comme le vrai.
    half_w = HALF_W * bez(1.0, 1.0, 0.60, 0.13, s)
    return half_w, floor, roof


def surf_base(z, t):
    """Point de la surface SANS les canaux de vitrage. La superellipse :
    |x/a|^n + |y/b|^n = 1, paramétrée par t. Le cambrage du toit ne dépend que de x :
    il est NUL au point le plus large du flanc (st = 0), donc le raccord flanc/toit
    reste lisse et le gabarit latéral intact."""
    half_w, floor, roof = section(z)
    cy = (roof + floor) * 0.5
    hy = (roof - floor) * 0.5
    ct, st = math.cos(t), math.sin(t)
    e = 2.0 / SECTION_N
    x = half_w * math.copysign(abs(ct) ** e, ct)
    y = cy + hy * math.copysign(abs(st) ** e, st)
    if st > 0.0:
        y += CAMBER * (1.0 - (x / half_w) ** 2) * (st ** (2.0 / SECTION_N))
    return (x, y, z)


def base_normal(z, t):
    """Normale de la surface de base (différences centrées : la superellipse a une
    dérivée singulière aux 4 « coins », une formule fermée y exploserait)."""
    h = 1e-3
    tu = sub(surf_base(z, t + h), surf_base(z, t - h))
    tv = sub(surf_base(min(z + h, Z_TAIL), t), surf_base(max(z - h, Z_TIP), t))
    n = norm(cross(tu, tv))
    # Orientation : la section de base est CONVEXE, « à l'opposé du centre » est valide.
    half_w, floor, roof = section(z)
    center = (0.0, (roof + floor) * 0.5, z)
    if dot(n, sub(surf_base(z, t), center)) < 0.0:
        n = mul(n, -1.0)
    return n


def channel_offset(z, t):
    """Profondeur de vitrage creusée en (z, t) : somme des canaux (ils ne se recouvrent
    pas). La périodicité en t (2*pi) est gérée : un canal à cheval sur t = 0 est écrit
    avec des bornes négatives."""
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
    vitrage. Toute la géométrie dérivée (normales, bandeaux, verre) lit CETTE surface :
    le verre posé dessus suit le canal gratuitement."""
    return sub(surf_base(z, t), mul(base_normal(z, t), channel_offset(z, t)))


def surf_normal(z, t):
    """Normale de la surface FINALE (canaux compris : les rampes de baies doivent
    rattraper la lumière). Différences centrées, réorientée vers l'extérieur — la
    concavité d'un canal (3 cm sur une caisse de 3 m) ne trompe pas le test."""
    h = 1e-3
    tu = sub(surf(z, t + h), surf(z, t - h))
    tv = sub(surf(min(z + h, Z_TAIL), t), surf(max(z - h, Z_TIP), t))
    n = norm(cross(tu, tv))
    half_w, floor, roof = section(z)
    center = (0.0, (roof + floor) * 0.5, z)
    if dot(n, sub(surf(z, t), center)) < 0.0:
        n = mul(n, -1.0)
    return n


def surf_tangent(z, t, n):
    """Tangente glTF : direction des u croissants (= t croissant), + handedness.
    La bitangente reconstruite par cross(N,T)*w doit suivre les v croissants, et v croît
    vers le MUSEAU (donc vers les z décroissants)."""
    h = 1e-3
    tan = norm(sub(surf(z, t + h), surf(z, t - h)))
    dv = mul(sub(surf(min(z + h, Z_TAIL), t), surf(max(z - h, Z_TIP), t)), -1.0)
    w = -1.0 if dot(cross(n, tan), dv) < 0.0 else 1.0
    return (tan[0], tan[1], tan[2], w)


class Part:
    """Un tampon de sommets par matériau : les index glTF sont locaux à la primitive."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

    def add_quad(self, verts):
        """verts = 4 tuples (position, normale, uv, tangente), dans l'ordre du contour."""
        base = len(self.positions)
        for p, n, uv, tg in verts:
            self.positions.append(p)
            self.normals.append(n)
            self.uvs.append(uv)
            self.tangents.append(tg)
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def stations():
    """Abscisses d'échantillonnage : espacées sur le corps droit, denses dans le nez,
    PLUS les bords et pieds de rampe de chaque canal de vitrage (sinon la grille
    échantillonnerait le canal au hasard et les baies seraient déchiquetées)."""
    out = [Z_TAIL + (Z_NOSE - Z_TAIL) * i / BODY_STEPS for i in range(BODY_STEPS + 1)]
    out += [Z_NOSE + (Z_TIP - Z_NOSE) * i / NOSE_STEPS for i in range(1, NOSE_STEPS + 1)]
    for (z0, z1, _, _, _, rz, _) in CHANNELS:
        out += [z0, z0 + rz, (z0 + z1) / 2.0, z1 - rz, z1]
    return sorted({round(z, 6) for z in out if Z_TIP <= z <= Z_TAIL}, reverse=True)


def t_columns():
    """Colonnes angulaires : base uniforme + bords et pieds de rampe des canaux."""
    cols = {round(2.0 * math.pi * j / T_SEGMENTS, 6) for j in range(T_SEGMENTS)}
    two_pi = 2.0 * math.pi
    for (_, _, t0, t1, _, _, rt) in CHANNELS:
        for tt in (t0, t0 + rt, (t0 + t1) / 2.0, t1 - rt, t1):
            cols.add(round(tt % two_pi, 6))
    return sorted(cols)


def build_body(part):
    """Loft de la carrosserie. Grille INDEXÉE à normales partagées : deux quads voisins
    lisent la même normale au sommet commun, donc l'éclairage est continu sur le nez.
    La dernière colonne (t = 2pi) duplique la première GÉOMÉTRIQUEMENT mais porte u = 1 :
    sans elle, la couture UV ferait un saut de 1 à 0 en plein milieu d'un quad."""
    zs = stations()
    cols = t_columns()
    two_pi = 2.0 * math.pi
    grid = []
    for z in zs:
        row = []
        for j, t in enumerate(cols + [two_pi]):
            p = surf(z, t)
            n = surf_normal(z, t)
            tg = surf_tangent(z, t, n)
            uv = (t / two_pi, (Z_TAIL - z) / LENGTH)
            row.append((p, n, uv, tg))
        grid.append(row)

    for i in range(len(zs) - 1):
        for j in range(len(cols)):
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])
    return grid


def build_cap(part, z, normal_z):
    """Obture une extrémité (la queue, qui s'attelle au reste de la rame, et le museau).
    Éventail autour du centre de section."""
    half_w, floor, roof = section(z)
    center = (0.0, (roof + floor) * 0.5, z)
    n = (0.0, 0.0, normal_z)
    tg = (1.0, 0.0, 0.0, 1.0)
    for j in range(T_SEGMENTS):
        t0 = 2.0 * math.pi * j / T_SEGMENTS
        t1 = 2.0 * math.pi * (j + 1) / T_SEGMENTS
        a, b = surf(z, t0), surf(z, t1)
        if normal_z < 0.0:
            a, b = b, a  # garde un winding cohérent des deux côtés
        base = len(part.positions)
        for p in (center, a, b):
            part.positions.append(p)
            part.normals.append(n)
            part.uvs.append((0.5 + p[0] / (4.0 * HALF_W), 0.5 + p[1] / (4.0 * HALF_W)))
            part.tangents.append(tg)
        part.indices.extend([base, base + 1, base + 2])


def build_band(part, z0, z1, t0, t1, nz, nt, offset):
    """Bandeau plaqué sur la surface FINALE, décalé de `offset` le long de la normale.
    Posé sur un canal de vitrage (offset +8 mm), il épouse le creusement : c'est ça,
    le bandeau de verre encastré. Posé sur la tôle, il s'y plaque sans z-fighting."""
    grid = []
    for i in range(nz + 1):
        z = z0 + (z1 - z0) * i / nz
        row = []
        for j in range(nt + 1):
            t = t0 + (t1 - t0) * j / nt
            n = surf_normal(z, t)
            p = add(surf(z, t), mul(n, offset))
            tg = surf_tangent(z, t, n)
            row.append((p, n, (j / nt, i / nz), tg))
        grid.append(row)
    for i in range(nz):
        for j in range(nt):
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])


def build_blister(part, z0, z1, t0, t1, height, nz=16, nt=8):
    """Carénage de toit : une bosse à profil sinusoïdal plaquée sur la carrosserie.
    L'offset suit sin(pi*u) sur les DEUX axes : nul aux 4 bords (fusion douce avec le
    toit, sans marche), maximal au centre. Les normales sont redérivées de la surface
    bosselée — garder celles du toit aplatirait la bosse à la lumière."""

    def point(z, t):
        uz = (z - z0) / (z1 - z0)
        ut = (t - t0) / (t1 - t0)
        off = height * math.sin(math.pi * uz) * math.sin(math.pi * ut)
        return add(surf(z, t), mul(surf_normal(z, t), off))

    def pnormal(z, t):
        h = 1e-3
        tu = sub(point(z, t + h), point(z, t - h))
        tv = sub(point(min(z + h, Z_TAIL), t), point(max(z - h, Z_TIP), t))
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


# --- Assemblage ---------------------------------------------------------------
parts = [Part() for _ in MATERIALS]

# 1) Carrosserie loftée à toit bombé, canaux de vitrage creusés, bouts obturés.
build_body(parts[MAT_PAINT])
build_cap(parts[MAT_PAINT], Z_TAIL, 1.0)
build_cap(parts[MAT_PAINT], Z_TIP, -1.0)

# 2) Verre posé DANS les canaux (8 mm au-dessus du fond : anti z-fighting). Le verre
#    couvre AUSSI les rampes : sur le vrai matériel, le bandeau entier est noir
#    (verre + bordure de sérigraphie), aucune lèvre peinte n'apparaît.
#    Échantillonnage DENSE (8 cm) : la corde d'un quad de verre trop grand couperait la
#    rampe lisse du canal et dépasserait de la tôle aux bords des baies.
for (z0, z1, t0, t1, _, _, _) in CHANNELS:
    nz = max(8, int(math.ceil((z1 - z0) / 0.08)))
    nt = max(4, int(math.ceil((t1 - t0) * 1.5 / 0.08)))
    build_band(parts[MAT_GLASS], z0, z1, t0, t1, nz, nt, 0.008)

# 3) Filet bleu nuit sous le bandeau, plaqué (pas de creusement : c'est de la peinture).
for lo, hi in ((-0.16, -0.04), (math.pi + 0.04, math.pi + 0.16)):
    build_band(parts[MAT_ACCENT], Z_NOSE + 0.20, Z_TAIL - 0.20, lo, hi, 16, 1, 0.012)

# 4) Jupe de bas de caisse : enroulée sous le plancher (t = 3pi/2 est le bas de caisse),
#    du museau à la queue. Elle suit le relèvement du plancher dans le nez => tablier.
build_band(parts[MAT_SKIRT], Z_TIP + 0.35, Z_TAIL - 0.10,
           math.pi + 0.85, 2.0 * math.pi - 0.85, 40, 4, 0.014)

# 5) Carénages de toit : au droit des DEUX bogies (essieux à ±5 et ±8 m => centres
#    ±6.5 m) et du pantographe (arrière). Blisters sinusoïdaux, matière peinte.
for zc in (-6.5, 6.5):
    build_blister(parts[MAT_PAINT], zc - 2.3, zc + 2.3,
                  math.pi / 2 - 0.55, math.pi / 2 + 0.55, 0.26)
build_blister(parts[MAT_PAINT], Z_TAIL - 5.8, Z_TAIL - 2.6,
              math.pi / 2 - 0.50, math.pi / 2 + 0.50, 0.32)

# 6) PLUS DE BOGIES DANS LA CAISSE (M17.6). Un bogie ne tangue JAMAIS avec la caisse : il
#    reste plaqué sur la voie. On les dessine donc SÉPARÉMENT (modèle tgv_bogie.glb, placé
#    par la physique aux positions des bogies avec l'orientation de la VOIE). Les intégrer
#    ici les faisait tanguer avec la caisse, d'où des roues qui décollaient.


# --- Sérialisation glTF binaire -----------------------------------------------
def align4(n):
    return (n + 3) & ~3


# On ne sérialise QUE les parts non vides. `used` mappe chaque primitive à son matériau
# d'origine : les index glTF des matériaux suivent l'ordre des parts écrites.
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
    base = 5 * slot  # POSITION, NORMAL, TEXCOORD_0, TANGENT, indices
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
    primitives.append({
        "attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2,
                       "TANGENT": base + 3},
        "indices": base + 4, "material": slot})

materials = [{"name": MATERIALS[mat_idx]["name"],
              "pbrMetallicRoughness": {"baseColorFactor": MATERIALS[mat_idx]["factor"],
                                       "metallicFactor": MATERIALS[mat_idx]["metallic"],
                                       "roughnessFactor": MATERIALS[mat_idx]["roughness"]}}
             for mat_idx, _ in used]

gltf = {
    "asset": {"version": "2.0", "generator": "noire-tgv-procedural-v2 (CC0)"},
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
print(f"{out} : motrice TGV V2, {LENGTH:.2f} x {HALF_W * 2:.2f} m, "
      f"toit bombé à {ROOF + CAMBER - RAIL:.2f} m au-dessus du rail")
print("  " + ", ".join(f"{m['name']}={len(p.positions)}v" for m, p in zip(MATERIALS, parts)))
print(f"  nez {NOSE_LEN:.1f} m (museau à {TIP_Y - RAIL:.2f} m du rail), "
      f"cambrage {CAMBER * 100:.0f} cm, {len(CHANNELS)} canaux de vitrage")
print(f"  {nv} sommets, {ni // 3} triangles, {glb_len} o")
