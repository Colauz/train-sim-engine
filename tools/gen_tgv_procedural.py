#!/usr/bin/env python3
"""Sculpte une motrice TGV stylisée, entièrement par équations, au format .glb.

PRINCIPE — surface LOFTÉE, pas un assemblage de boîtes :
  * une SECTION transversale paramétrée par un angle t (superellipse : entre l'ellipse
    et le rectangle, c'est la forme d'une caisse ferroviaire) ;
  * dont la largeur, le plancher et le toit varient le long de z selon des COURBES DE
    BÉZIER cubiques — c'est ce qui sculpte le nez plongeant caractéristique ;
  * les normales sont dérivées ANALYTIQUEMENT (produit vectoriel des deux tangentes de
    la surface) et PARTAGÉES entre facettes : la lumière s'enroule sur le nez au lieu
    de le facetter.

Repère = repère LOCAL de la caisse du moteur Noire :
  x = droite, y = haut, z = arrière (l'app le dessine via body_position * body_orientation).
  origine = centre de la caisse ; le PLAN DE ROULEMENT est à y = -body_height (= -2.20 m).
Aucune dépendance externe (struct/json/math de la stdlib)."""
import struct, json, math, sys

# --- Cotes réelles d'une motrice TGV, en mètres --------------------------------
RAIL = -2.20            # plan de roulement dans le repère caisse (= -body_height)
LENGTH = 22.15          # longueur hors tampons
HALF_W = 1.45           # 2.90 m de large (gabarit UIC)
ROOF = RAIL + 4.10      # 4.10 m au-dessus du rail
FLOOR = RAIL + 1.05     # bas de caisse
NOSE_LEN = 6.00         # longueur du nez profilé (l'avant est en -Z)
TIP_Y = RAIL + 1.70     # hauteur du museau : très bas, c'est la signature du TGV

Z_TAIL = LENGTH / 2.0
Z_TIP = -LENGTH / 2.0
Z_NOSE = Z_TIP + NOSE_LEN  # abscisse où le nez commence à se former

# Exposant de la superellipse : 2 = ellipse, +inf = rectangle. 4 donne le rectangle
# arrondi d'une caisse ferroviaire.
SECTION_N = 4.0
T_SEGMENTS = 28         # facettes autour de la section
BODY_STEPS = 6          # stations le long du corps droit (rien n'y varie)
NOSE_STEPS = 26         # stations dans le nez : c'est là qu'est toute la courbure

# --- Roues et bogies ----------------------------------------------------------
WHEEL_R = 0.46          # roue de 920 mm : le bas touche donc exactement le rail
WHEEL_W = 0.14
WHEEL_SEGMENTS = 20
GAUGE_HALF = 0.7175     # 1.435 / 2 : la roue est centrée sur le rail
AXLES_Z = (-8.0, -5.0, 5.0, 8.0)  # 2 bogies de 2 essieux, empattement 3 m

# --- Matériaux PBR (convention glTF metallic-roughness) ------------------------
# Rappel : metallic = 1 => la baseColor est la couleur de RÉFLEXION (F0), pas un pigment.
#          metallic = 0 => F0 = 0.04 fixe, la baseColor est le pigment diffus.
# Les FACTEURS glTF sont LINÉAIRES.
MATERIALS = [
    # Peinture métallisée : metallic 0.35 est volontairement NON physique (un vrai
    # matériau est 0 ou 1). C'est la triche classique pour une peinture métallisée, dont
    # les paillettes d'aluminium se comportent en métal sans que la surface entière en
    # soit un. C'est elle qui fera courir le ciel le long du nez.
    {"name": "peinture", "factor": [0.78, 0.79, 0.82, 1.0], "metallic": 0.35, "roughness": 0.28},
    # Vitrage : diélectrique, surtout PAS métallique — un baseColor noir + metallic élevé
    # donnerait F0 ~ 0.008, une surface qui ne réfléchit RIEN (l'inverse du verre).
    {"name": "vitrage", "factor": [0.02, 0.025, 0.03, 1.0], "metallic": 0.0, "roughness": 0.05},
    # Roues : acier NU, poli par le roulement. Volontairement plus clair et plus lisse
    # que le bogie — sans ce contraste, la roue disparaît dans le châssis : même teinte,
    # même réflexion, donc aucune silhouette (constaté à l'image, la géométrie était
    # pourtant juste). Le vrai matériel est contrasté de la même façon.
    {"name": "roue", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
    # Châssis de bogie : PEINT, donc diélectrique et mat. Sombre (poussière de frein).
    {"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65},
]
MAT_PAINT, MAT_GLASS, MAT_WHEEL, MAT_BOGIE = 0, 1, 2, 3


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


# --- Le profil longitudinal : c'est ici qu'est sculpté le nez ------------------
def section(z):
    """(demi-largeur, plancher, toit) à l'abscisse z. Constant sur le corps, gouverné par
    trois Béziers dans le nez."""
    if z >= Z_NOSE:
        return HALF_W, FLOOR, ROOF
    s = (Z_NOSE - z) / NOSE_LEN  # 0 à la base du nez, 1 au museau
    # Toit : plat au départ (P1 = P0 => raccord lisse), puis plongée franche, puis
    # aplatissement sur le museau (P2 proche de P3).
    roof = bez(ROOF, ROOF, TIP_Y + 0.55, TIP_Y, s)
    # Plancher : remonte à peine — le dessous du nez reste presque horizontal.
    floor = bez(FLOOR, FLOOR, FLOOR + 0.10, FLOOR + 0.40, s)
    # Largeur : tient longtemps puis s'effile. Le museau n'est PAS une pointe (0.17)
    # mais une lame arrondie, comme le vrai.
    half_w = HALF_W * bez(1.0, 0.99, 0.66, 0.17, s)
    return half_w, floor, roof


def surf(z, t):
    """Point de la surface. La superellipse : |x/a|^n + |y/b|^n = 1, paramétrée par t."""
    half_w, floor, roof = section(z)
    cy = (roof + floor) * 0.5
    hy = (roof - floor) * 0.5
    ct, st = math.cos(t), math.sin(t)
    e = 2.0 / SECTION_N
    x = half_w * math.copysign(abs(ct) ** e, ct)
    y = cy + hy * math.copysign(abs(st) ** e, st)
    return (x, y, z)


def surf_normal(z, t):
    """Normale analytique : produit vectoriel des deux tangentes de la surface. Calculée
    par différences centrées — la superellipse a une dérivée singulière aux 4 « coins »
    (t = 0, pi/2, ...), une formule fermée y exploserait."""
    h = 1e-3
    tu = sub(surf(z, t + h), surf(z, t - h))              # le long de la section
    z1 = min(z + h, Z_TAIL)
    z0 = max(z - h, Z_TIP)
    tv = sub(surf(z1, t), surf(z0, t))                    # le long du train
    n = norm(cross(tu, tv))
    # Orientation : la section est CONVEXE, donc « à l'opposé du centre » est un test
    # VALIDE ici — contrairement au profil en I des rails, qui est concave et où cette
    # même astuce inverse la face supérieure du patin (cf. track_mesh.cpp).
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
    z1 = min(z + h, Z_TAIL)
    z0 = max(z - h, Z_TIP)
    dv = mul(sub(surf(z1, t), surf(z0, t)), -1.0)  # v croît quand z décroît
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
    """Abscisses d'échantillonnage : espacées sur le corps droit (rien n'y varie), denses
    dans le nez (toute la courbure y est)."""
    out = [Z_TAIL + (Z_NOSE - Z_TAIL) * i / BODY_STEPS for i in range(BODY_STEPS + 1)]
    out += [Z_NOSE + (Z_TIP - Z_NOSE) * i / NOSE_STEPS for i in range(1, NOSE_STEPS + 1)]
    return out


def build_body(part):
    """Loft de la carrosserie. Grille INDEXÉE à normales partagées : deux quads voisins
    lisent la même normale au sommet commun, donc l'éclairage est continu sur le nez.
    La colonne t = T_SEGMENTS duplique la première GÉOMÉTRIQUEMENT mais porte u = 1 :
    sans elle, la couture UV ferait un saut de 1 à 0 en plein milieu d'un quad."""
    zs = stations()
    grid = []
    for z in zs:
        row = []
        for j in range(T_SEGMENTS + 1):
            t = 2.0 * math.pi * j / T_SEGMENTS
            p = surf(z, t)
            n = surf_normal(z, t)
            tg = surf_tangent(z, t, n)
            uv = (j / T_SEGMENTS, (Z_TAIL - z) / LENGTH)
            row.append((p, n, uv, tg))
        grid.append(row)

    for i in range(len(zs) - 1):
        for j in range(T_SEGMENTS):
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])
    return grid


def build_cap(part, z, normal_z):
    """Obture une extrémité (la queue, qui s'attelle au reste de la rame). Éventail
    autour du centre de section."""
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
    """Bandeau plaqué SUR la carrosserie, décalé de `offset` le long de la normale. Il
    épouse donc exactement la courbure — un quad plat traverserait le nez. Le décalage
    est le seul garde-fou contre le z-fighting : aucune face n'est coplanaire, les deux
    surfaces sont parallèles à `offset` l'une de l'autre."""
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


def build_box(part, x0, y0, z0, x1, y1, z1):
    c = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    faces = [((4, 5, 6, 7), (0, 0, 1)), ((1, 0, 3, 2), (0, 0, -1)),
             ((5, 1, 2, 6), (1, 0, 0)), ((0, 4, 7, 3), (-1, 0, 0)),
             ((3, 7, 6, 2), (0, 1, 0)), ((0, 1, 5, 4), (0, -1, 0))]
    for idx, n in faces:
        # Tangente : n'importe quel vecteur du plan de la face fait l'affaire ici (pas de
        # normal map sur ces pièces), mais il doit être ORTHOGONAL à la normale.
        t = (0.0, 0.0, 1.0) if abs(n[2]) < 0.9 else (1.0, 0.0, 0.0)
        tg = (t[0], t[1], t[2], 1.0)
        part.add_quad([(c[idx[k]], n, ((k in (1, 2)) * 1.0, (k >= 2) * 1.0), tg)
                       for k in range(4)])


def build_wheel(part, cx, z, radius, half_width, segments):
    """Roue = cylindre d'axe X (l'essieu). Bande de roulement + les deux flasques.
    Les normales de la bande sont RADIALES et partagées : la roue est ronde à la
    lumière, pas seulement à la silhouette."""
    cy = RAIL + radius  # le bas de la roue touche EXACTEMENT le plan de roulement
    ring = []
    for j in range(segments + 1):
        a = 2.0 * math.pi * j / segments
        ring.append((math.cos(a), math.sin(a)))

    # Bande de roulement
    for j in range(segments):
        (c0, s0), (c1, s1) = ring[j], ring[j + 1]
        n0 = (0.0, c0, s0)
        n1 = (0.0, c1, s1)
        p00 = (cx - half_width, cy + radius * c0, z + radius * s0)
        p10 = (cx + half_width, cy + radius * c0, z + radius * s0)
        p11 = (cx + half_width, cy + radius * c1, z + radius * s1)
        p01 = (cx - half_width, cy + radius * c1, z + radius * s1)
        tg = (1.0, 0.0, 0.0, 1.0)  # le long de l'axe
        u0, u1 = j / segments, (j + 1) / segments
        part.add_quad([(p00, n0, (u0, 0.0), tg), (p10, n0, (u0, 1.0), tg),
                       (p11, n1, (u1, 1.0), tg), (p01, n1, (u1, 0.0), tg)])

    # Flasques
    for side in (-1.0, 1.0):
        n = (side, 0.0, 0.0)
        tg = (0.0, 0.0, 1.0, 1.0)
        center = (cx + side * half_width, cy, z)
        for j in range(segments):
            (c0, s0), (c1, s1) = ring[j], ring[j + 1]
            a = (cx + side * half_width, cy + radius * c0, z + radius * s0)
            b = (cx + side * half_width, cy + radius * c1, z + radius * s1)
            if side < 0.0:
                a, b = b, a
            base = len(part.positions)
            for p in (center, a, b):
                part.positions.append(p)
                part.normals.append(n)
                part.uvs.append((0.5 + (p[2] - z) / (4 * radius), 0.5 + (p[1] - cy) / (4 * radius)))
                part.tangents.append(tg)
            part.indices.extend([base, base + 1, base + 2])


# --- Assemblage ---------------------------------------------------------------
parts = [Part(), Part(), Part(), Part()]

# 1) Carrosserie loftée + obturation de la queue.
build_body(parts[MAT_PAINT])
build_cap(parts[MAT_PAINT], Z_TAIL, 1.0)
build_cap(parts[MAT_PAINT], Z_TIP, -1.0)

# 2) Carénage de pantographe sur le toit (arrière).
build_box(parts[MAT_PAINT], -0.55, ROOF - 0.10, Z_TAIL - 5.5, 0.55, ROOF + 0.30, Z_TAIL - 3.0)

# 3) Pare-brise : plaqué sur la surface PLONGEANTE du nez, en travers du toit. C'est
#    l'intérêt du bandeau lofté — il suit la courbe au lieu de la couper.
build_band(parts[MAT_GLASS], Z_NOSE + 0.35, Z_TIP + 2.6,
           math.pi / 2 - 0.62, math.pi / 2 + 0.62, 10, 8, 0.03)

# 4) Vitres latérales de cabine, sur le haut du flanc (t = 0 est le flanc, pi/2 le toit).
for lo, hi in ((0.28, 0.72), (math.pi - 0.72, math.pi - 0.28)):
    build_band(parts[MAT_GLASS], Z_NOSE + 2.6, Z_NOSE + 0.2, lo, hi, 6, 4, 0.03)

# 5) PLUS DE BOGIES DANS LA CAISSE (M17.6). Un bogie ne tangue JAMAIS avec la caisse : il
#    reste plaqué sur la voie. On les dessine donc SÉPARÉMENT (modèle tgv_bogie.glb, placé
#    par la physique aux positions des bogies avec l'orientation de la VOIE). Les intégrer
#    ici les faisait tanguer avec la caisse, d'où des roues qui décollaient.
#    Les parts MAT_BOGIE/MAT_WHEEL restent donc vides et sont ignorées à la sérialisation.


# --- Sérialisation glTF binaire -----------------------------------------------
def align4(n):
    return (n + 3) & ~3


# On ne sérialise QUE les parts non vides : depuis le M17.6 les bogies/roues sont retirés
# de la caisse, donc MAT_BOGIE/MAT_WHEEL sont vides. `used` mappe chaque primitive à son
# matériau d'origine.
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
    "asset": {"version": "2.0", "generator": "noire-tgv-procedural (CC0)"},
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
print(f"{out} : motrice TGV {LENGTH:.2f} x {HALF_W * 2:.2f} x {ROOF - RAIL:.2f} m au-dessus du rail")
print("  " + ", ".join(f"{m['name']}={len(p.positions)}v" for m, p in zip(MATERIALS, parts)))
print(f"  nez {NOSE_LEN:.1f} m (museau à {TIP_Y - RAIL:.2f} m du rail), "
      f"{len(AXLES_Z) * 2} roues de {WHEEL_R * 2000:.0f} mm")
print(f"  {nv} sommets, {ni // 3} triangles, {glb_len} o")
