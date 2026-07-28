#!/usr/bin/env python3
"""Détecteur de z-fighting pour les GLB générés (M30.5).

Signale les paires de triangles COPLANAIRES, de MÊME orientation, dont les
projections 2D se recouvrent : ce sont les seuls cas réellement rasterisés en
compétition (faces dos-à-dos de normales opposées = inoffensives, ignorées).

Usage : python3 tools/check_coplanar.py assets/models/metro_motrice.glb ...
"""
import struct
import sys
import json
import math
from collections import defaultdict


def load_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"glTF"
    length = struct.unpack("<I", data[8:12])[0]
    off = 12
    gltf = None
    binchunk = None
    while off < length:
        clen, ctype = struct.unpack("<II", data[off:off + 8])
        chunk = data[off + 8:off + 8 + clen]
        if ctype == 0x4E4F534A:
            gltf = json.loads(chunk)
        elif ctype == 0x004E4942:
            binchunk = chunk
        off += 8 + clen
    return gltf, binchunk


def read_accessor(gltf, binchunk, idx):
    acc = gltf["accessors"][idx]
    bv = gltf["bufferViews"][acc["bufferView"]]
    comp_size = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}[acc["componentType"]]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    fmt = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}[acc["componentType"]]
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", comp_size * ncomp)
    out = []
    for i in range(acc["count"]):
        o = start + i * stride
        out.append(struct.unpack("<" + fmt * ncomp, binchunk[o:o + comp_size * ncomp]))
    return out


def collect_triangles(gltf, binchunk):
    """Liste de (mat, qid, ((ax,ay,az),(bx,by,bz),(cx,cy,cz))) en monde.
    qid = (index de primitive) * 100000 + (index du quad dans la primitive) :
    deux triangles du même quad partagent un qid ; deux primitives placées
    différemment à l'exécution ont des qid de blocs distincts."""
    tris = []
    prim_counter = [0]

    def walk(node_idx, m):
        node = gltf["nodes"][node_idx]
        # matrice locale
        if "matrix" in node:
            lm = node["matrix"]  # column-major
            local = [[lm[c * 4 + r] for c in range(4)] for r in range(4)]
        else:
            t = node.get("translation", [0, 0, 0])
            r = node.get("rotation", [0, 0, 0, 1])
            s = node.get("scale", [1, 1, 1])
            x, y, z, w = r
            rot = [
                [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
            ]
            local = [[rot[i][j] * s[j] for j in range(3)] + [t[i]] for i in range(3)]
            local.append([0, 0, 0, 1])
        world = [[sum(m[i][k] * local[k][j] for k in range(4)) for j in range(4)]
                 for i in range(4)]
        if "mesh" in node:
            mesh = gltf["meshes"][node["mesh"]]
            for prim in mesh["primitives"]:
                pos = read_accessor(gltf, binchunk, prim["attributes"]["POSITION"])
                idxs = read_accessor(gltf, binchunk, prim["indices"])
                mat = prim.get("material", -1)
                flat = [i[0] for i in idxs]
                for t0 in range(0, len(flat), 3):
                    pts = []
                    for vi in flat[t0:t0 + 3]:
                        p = pos[vi]
                        pts.append(tuple(
                            world[i][0] * p[0] + world[i][1] * p[1] +
                            world[i][2] * p[2] + world[i][3] for i in range(3)))
                    tris.append((mat, prim_counter[0] * 100000 + t0 // 6, tuple(pts)))
                prim_counter[0] += 1
        for c in node.get("children", []):
            walk(c, world)

    ident = [[1 if i == j else 0 for j in range(4)] for i in range(4)]
    scene = gltf["scenes"][gltf.get("scene", 0)]
    for n in scene["nodes"]:
        walk(n, ident)
    return tris


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def check(path, same_prim_only=False):
    """same_prim_only : ne compare que les triangles d'une même primitive
    (bogie : les essieux sont replacés à z=±1.5 par l'application, leurs
    coplanarités inter-primitives sont fictives)."""
    gltf, binchunk = load_glb(path)
    tris = collect_triangles(gltf, binchunk)
    # Regroupe par (axe du plan, coordonnée arrondie, signe de la normale).
    planes = defaultdict(list)
    for mat, qid, (a, b, c) in tris:
        n = cross(sub(b, a), sub(c, a))
        ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if ln < 1e-12:
            continue
        n = tuple(v / ln for v in n)
        ax = max(range(3), key=lambda i: abs(n[i]))
        if abs(n[ax]) < 0.999:  # pas axis-aligned : ignoré (rare, panneaux inclinés)
            continue
        key = (ax, round(a[ax], 4), 1 if n[ax] > 0 else -1)
        # projection 2D du triangle
        o = [i for i in range(3) if i != ax]
        pts2 = [((p[o[0]]), (p[o[1]])) for p in (a, b, c)]
        planes[key].append((mat, qid, pts2))

    def overlap(t1, t2):
        # Recouvrement EXACT triangle/triangle en 2D : t2 est clippé par les
        # 3 demi-plans intérieurs de t1 (Sutherland–Hodgman). Un AABB 2D
        # suffisait pour des dalles rectangulaires, mais faussement positif
        # sur les secteurs en éventail des disques de roues.
        def clip_edge(poly, p, q):
            def cross2(e, r):
                return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
            out = []
            for i in range(len(poly)):
                s, e = poly[i - 1], poly[i]
                ds, de = cross2(None, s), cross2(None, e)
                if de >= -1e-12:
                    if ds < -1e-12:
                        t = ds / (ds - de)
                        out.append((s[0] + t * (e[0] - s[0]), s[1] + t * (e[1] - s[1])))
                    out.append(e)
                elif ds >= -1e-12:
                    t = ds / (ds - de)
                    out.append((s[0] + t * (e[0] - s[0]), s[1] + t * (e[1] - s[1])))
            return out
        poly = list(t2)
        for i in range(3):
            poly = clip_edge(poly, t1[i], t1[(i + 1) % 3])
            if len(poly) < 3:
                return None
        area = abs(sum(poly[k][0] * poly[(k + 1) % len(poly)][1] -
                       poly[(k + 1) % len(poly)][0] * poly[k][1]
                       for k in range(len(poly)))) / 2.0
        if area <= 1e-8:
            return None
        u0 = min(p[0] for p in poly); u1 = max(p[0] for p in poly)
        v0 = min(p[1] for p in poly); v1 = max(p[1] for p in poly)
        return (u1 - u0, v1 - v0)

    bad = 0
    for key, lst in sorted(planes.items()):
        seen = set()
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                if lst[i][1] == lst[j][1]:
                    continue  # deux triangles du même quad
                if same_prim_only and (lst[i][1] // 100000) != (lst[j][1] // 100000):
                    continue  # primitives replacées différemment à l'exécution
                ov = overlap(lst[i][2], lst[j][2])
                if ov:
                    sig = (round(ov[0], 3), round(ov[1], 3))
                    if sig in seen:
                        continue
                    seen.add(sig)
                    bad += 1
                    print(f"  {path} plan axe{key[0]}={key[1]:+.4f} dir{key[2]:+d} "
                          f"mat {lst[i][0]} vs {lst[j][0]} recouvrement {ov[0]:.3f}x{ov[1]:.3f}")
    return bad


if __name__ == "__main__":
    total = 0
    for p in sys.argv[1:]:
        total += check(p, same_prim_only="bogie" in p)
    print(f"\n{total} paire(s) coplanaire(s) de même orientation (z-fight visible).")
    sys.exit(1 if total else 0)
