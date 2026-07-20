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
    {"name": "vitrage", "factor": [0.015, 0.020, 0.028, 1.0], "metallic": 0.0, "roughness": 0.05},
    {"name": "accent", "factor": [0.045, 0.075, 0.16, 1.0], "metallic": 0.35, "roughness": 0.32},
    {"name": "jupe", "factor": [0.24, 0.25, 0.27, 1.0], "metallic": 0.0, "roughness": 0.72},
    {"name": "soufflet", "factor": [0.05, 0.05, 0.055, 1.0], "metallic": 0.0, "roughness": 0.85},
    # Roues : acier NU, poli par le roulement — plus clair et plus lisse que le châssis,
    # sinon la roue disparaît dans le bogie (même teinte => aucune silhouette).
    {"name": "roue", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
    {"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65},
]
MAT_PAINT, MAT_GLASS, MAT_ACCENT, MAT_SKIRT, MAT_BELLOWS, MAT_WHEEL, MAT_BOGIE = range(7)


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
    échantillonnées au hasard de la grille et déchiquetées)."""
    out = [Z_TAIL + (Z_HEAD - Z_TAIL) * i / BODY_STEPS for i in range(BODY_STEPS + 1)]
    for (z0, z1, _, _, _, rz, _) in CHANNELS:
        out += [z0, z0 + rz, (z0 + z1) / 2.0, z1 - rz, z1]
    return sorted({round(z, 6) for z in out if Z_HEAD <= z <= Z_TAIL}, reverse=True)


def t_columns():
    """Colonnes angulaires : base uniforme + bords et pieds de rampe des canaux."""
    cols = {round(2.0 * math.pi * j / T_SEGMENTS, 6) for j in range(T_SEGMENTS)}
    two_pi = 2.0 * math.pi
    for (_, _, t0, t1, _, _, rt) in CHANNELS:
        for tt in (t0, t0 + rt, (t0 + t1) / 2.0, t1 - rt, t1):
            cols.add(round(tt % two_pi, 6))
    return sorted(cols)


def build_body(part):
    """Grille INDEXÉE à normales partagées (éclairage continu). La dernière colonne
    (t = 2pi) duplique la première GÉOMÉTRIQUEMENT mais porte u = 1 (couture UV)."""
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
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])


def build_cap(part, z, normal_z):
    center = (0.0, (ROOF + FLOOR) * 0.5, z)
    n = (0.0, 0.0, normal_z)
    tg = (1.0, 0.0, 0.0, 1.0)
    for j in range(T_SEGMENTS):
        t0 = 2.0 * math.pi * j / T_SEGMENTS
        t1 = 2.0 * math.pi * (j + 1) / T_SEGMENTS
        a, b = surf(z, t0), surf(z, t1)
        if normal_z < 0.0:
            a, b = b, a
        base = len(part.positions)
        for p in (center, a, b):
            part.positions.append(p); part.normals.append(n)
            part.uvs.append((0.5 + p[0] / (4.0 * HALF_W), 0.5 + p[1] / (4.0 * HALF_W)))
            part.tangents.append(tg)
        part.indices.extend([base, base + 1, base + 2])


def build_band(part, z0, z1, t0, t1, nz, nt, offset):
    """Bandeau plaqué sur la surface FINALE, décalé de `offset` le long de la normale
    (anti z-fighting). Posé sur un canal de vitrage, il épouse le creusement."""
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
            part.add_quad([grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]])


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


def build_wheel(part, cx, z, radius, half_width, segments):
    """Disque plein de rayon `radius`, axe transversal (x), centré à (cx, radius, z) :
    le bas de la roue affleure y = 0 (le rail)."""
    cy = radius
    for j in range(segments):
        a0 = 2.0 * math.pi * j / segments
        a1 = 2.0 * math.pi * (j + 1) / segments
        p0 = (cx - half_width, cy + radius * math.sin(a0), z + radius * math.cos(a0))
        p1 = (cx - half_width, cy + radius * math.sin(a1), z + radius * math.cos(a1))
        q0 = (cx + half_width, cy + radius * math.sin(a0), z + radius * math.cos(a0))
        q1 = (cx + half_width, cy + radius * math.sin(a1), z + radius * math.cos(a1))
        # bande de roulement
        part.add_quad([(p0, (0, math.sin(a0), math.cos(a0)), (0, 0), (1, 0, 0, 1)),
                       (q0, (0, math.sin(a0), math.cos(a0)), (1, 0), (1, 0, 0, 1)),
                       (q1, (0, math.sin(a1), math.cos(a1)), (1, 1), (1, 0, 0, 1)),
                       (p1, (0, math.sin(a1), math.cos(a1)), (0, 1), (1, 0, 0, 1))])
        # flancs (2 triangles vers le centre de chaque face)
        for cxx, nx in ((cx - half_width, -1.0), (cx + half_width, 1.0)):
            cen = (cxx, cy, z)
            e0 = (cxx, cy + radius * math.sin(a0), z + radius * math.cos(a0))
            e1 = (cxx, cy + radius * math.sin(a1), z + radius * math.cos(a1))
            a, b = (e0, e1) if nx > 0 else (e1, e0)
            base = len(part.positions)
            for p in (cen, a, b):
                part.positions.append(p); part.normals.append((nx, 0.0, 0.0))
                part.uvs.append((0.0, 0.0)); part.tangents.append((0.0, 0.0, 1.0, 1.0))
            part.indices.extend([base, base + 1, base + 2])


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
    materials = [{"name": MATERIALS[mat_idx]["name"],
                  "pbrMetallicRoughness": {"baseColorFactor": MATERIALS[mat_idx]["factor"],
                                           "metallicFactor": MATERIALS[mat_idx]["metallic"],
                                           "roughnessFactor": MATERIALS[mat_idx]["roughness"]}}
                 for mat_idx, _ in used]

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
    write_glb(out, parts, "TGV_voiture")


# --- Bogie Jacobs (organe partagé) --------------------------------------------
def build_bogie(out):
    parts = [Part() for _ in MATERIALS]
    WHEEL_R, WHEEL_W, GAUGE_HALF = 0.46, 0.14, 0.7175
    AXLES = (-1.5, 1.5)  # empattement 3 m
    # Châssis : au-dessus des roues, plus étroit que la voie (les roues doivent dépasser).
    build_box(parts[MAT_BOGIE], -0.45, WHEEL_R + 0.10, -1.9, 0.45, WHEEL_R + 0.85, 1.9)
    for z in AXLES:
        # essieu (barre transversale)
        build_box(parts[MAT_BOGIE], -GAUGE_HALF, WHEEL_R - 0.05, z - 0.05,
                  GAUGE_HALF, WHEEL_R + 0.05, z + 0.05)
        for side in (-GAUGE_HALF, GAUGE_HALF):
            build_wheel(parts[MAT_WHEEL], side, z, WHEEL_R, WHEEL_W / 2.0, 20)
    write_glb(out, parts, "TGV_bogie_jacobs")


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    build_car(f"{outdir}/tgv_voiture.glb")
    build_bogie(f"{outdir}/tgv_bogie.glb")
