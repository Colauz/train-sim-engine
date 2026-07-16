#!/usr/bin/env python3
"""Sculpte une VOITURE VOYAGEURS de TGV et un BOGIE JACOBS, au format .glb (M16).

La voiture reprend la carrosserie loftée de la motrice (tools/gen_tgv_procedural.py),
mais SANS nez : la section superellipse (n=4) est CONSTANTE sur toute la longueur — c'est
une caisse-tube, pas un profil aérodynamique. Elle est un peu plus longue que la motrice
et porte de GRANDES vitres latérales (bande de verre PBR plaquée sur chaque flanc).

Le bogie Jacobs est généré à part : c'est l'organe PARTAGÉ entre deux caisses, positionné
par le Consist à l'articulation. Son origine locale est au PLAN DE ROULEMENT (y = 0), ce
qui colle à Bogie::position() (échantillon sur le rail).

Repère caisse : x = droite, y = haut, z = arrière ; rail à y = -body_height = -2.20.
Aucune dépendance externe (stdlib seule). Sortie : deux .glb."""
import struct, json, math, sys

RAIL = -2.20
BODY_LEN = 23.60        # « légèrement plus longue » que la motrice (22.15 m)
HALF_W = 1.45           # même gabarit UIC (2.90 m)
ROOF = RAIL + 4.10
FLOOR = RAIL + 1.05
SECTION_N = 4.0         # superellipse : rectangle arrondi d'une caisse ferroviaire
T_SEGMENTS = 28
BODY_STEPS = 10         # section constante => peu de stations suffisent
Z_TAIL = BODY_LEN / 2.0
Z_HEAD = -BODY_LEN / 2.0

MATERIALS = [
    {"name": "peinture", "factor": [0.80, 0.81, 0.84, 1.0], "metallic": 0.35, "roughness": 0.28},
    {"name": "vitrage", "factor": [0.02, 0.025, 0.03, 1.0], "metallic": 0.0, "roughness": 0.05},
    {"name": "roue", "factor": [0.55, 0.55, 0.56, 1.0], "metallic": 1.0, "roughness": 0.30},
    {"name": "bogie", "factor": [0.09, 0.09, 0.10, 1.0], "metallic": 0.0, "roughness": 0.65},
]
MAT_PAINT, MAT_GLASS, MAT_WHEEL, MAT_BOGIE = 0, 1, 2, 3


# --- Algèbre ------------------------------------------------------------------
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 0.0, 1.0)


# --- Section CONSTANTE (aucun nez) --------------------------------------------
def surf(z, t):
    """Superellipse |x/a|^n + |y/b|^n = 1, invariante en z (caisse-tube)."""
    cy = (ROOF + FLOOR) * 0.5
    hy = (ROOF - FLOOR) * 0.5
    ct, st = math.cos(t), math.sin(t)
    e = 2.0 / SECTION_N
    x = HALF_W * math.copysign(abs(ct) ** e, ct)
    y = cy + hy * math.copysign(abs(st) ** e, st)
    return (x, y, z)


def surf_normal(z, t):
    """Normale analytique par différences centrées (la superellipse est singulière aux
    coins). La section étant constante, la tangente longitudinale est (0,0,1) : la normale
    est purement dans le plan de section."""
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


def build_body(part):
    zs = [Z_TAIL + (Z_HEAD - Z_TAIL) * i / BODY_STEPS for i in range(BODY_STEPS + 1)]
    grid = []
    for z in zs:
        row = []
        for j in range(T_SEGMENTS + 1):
            t = 2.0 * math.pi * j / T_SEGMENTS
            n = surf_normal(z, t)
            row.append((surf(z, t), n, (j / T_SEGMENTS, (Z_TAIL - z) / BODY_LEN),
                        surf_tangent(z, t, n)))
        grid.append(row)
    for i in range(len(zs) - 1):
        for j in range(T_SEGMENTS):
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
    """Bandeau plaqué sur la carrosserie, décalé de `offset` le long de la normale (anti
    z-fighting). Sert les grandes vitres latérales."""
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

    gltf = {"asset": {"version": "2.0", "generator": "noire-tgv-voiture (CC0)"},
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


# --- Voiture voyageurs --------------------------------------------------------
def build_car(out):
    parts = [Part(), Part(), Part(), Part()]
    build_body(parts[MAT_PAINT])
    build_cap(parts[MAT_PAINT], Z_TAIL, 1.0)
    build_cap(parts[MAT_PAINT], Z_HEAD, -1.0)
    # Carénage de toiture (climatisation), discret.
    build_box(parts[MAT_PAINT], -0.5, ROOF - 0.08, Z_HEAD + 3.0, 0.5, ROOF + 0.18, Z_TAIL - 3.0)
    # GRANDES vitres latérales : une longue bande de verre sur chaque flanc, sur presque
    # toute la longueur, à mi-hauteur de la caisse (t ~ 0 côté droit, ~pi côté gauche).
    win_z0, win_z1 = Z_HEAD + 1.6, Z_TAIL - 1.6
    for lo, hi in ((-0.34, 0.34), (math.pi - 0.34, math.pi + 0.34)):
        build_band(parts[MAT_GLASS], win_z0, win_z1, lo, hi, 24, 3, 0.02)
    write_glb(out, parts, "TGV_voiture")


# --- Bogie Jacobs (organe partagé) --------------------------------------------
def build_bogie(out):
    parts = [Part(), Part(), Part(), Part()]
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
