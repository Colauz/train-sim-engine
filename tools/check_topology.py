#!/usr/bin/env python3
"""Vérificateur de TOPOLOGIE des GLB générés (M56) — le pendant de check_coplanar.py.

check_coplanar.py traque le z-fighting : deux surfaces qui se disputent le même plan.
Celui-ci traque le défaut opposé, et bien plus voyant — LES SURFACES QUI MANQUENT.

Il rapporte, primitive par primitive :

  * NORMALE INVERSÉE — la normale géométrique du triangle (déduite de l'ordre de ses
    sommets) contredit la normale de ses sommets. glTF impose le sens trigonométrique
    vu de l'extérieur (spec 3.7.2.1) et le moteur rastérise en
    VK_FRONT_FACE_COUNTER_CLOCKWISE + back-face culling : un triangle inversé est
    littéralement JETÉ par le GPU. C'est ce défaut, appliqué à TOUTE la rame, qui la
    rendait traversable du regard avant le M56.

  * TROU — une arête qui n'appartient qu'à UN triangle : la surface s'arrête là. C'est
    légitime pour une carte de feuillage ou une vitre (une nappe n'a pas d'intérieur) ;
    c'est un défaut pour tout ce qui prétend être un volume.

  * WINDING — une arête partagée par deux triangles qui la parcourent dans le MÊME sens :
    les deux faces se tournent le dos, l'une des deux disparaîtra sous le culling.

  * NON-MANIFOLD — une arête partagée par plus de deux triangles.

Les arêtes sont soudées PAR POSITION (arrondie au dixième de millimètre) : deux boîtes
accolées comptent comme une seule surface, ce qui est exactement ce que voit le GPU.

Usage : python3 tools/check_topology.py assets/models/*.glb
Sortie : code 1 si au moins une normale inversée, une arête de winding ou une arête
non-manifold est trouvée (les trous, eux, sont seulement listés — ils sont parfois
voulus : cartes de feuillage, vitres, tronc enterré).
"""
import json
import math
import struct
import sys
from collections import defaultdict


def load_glb(path):
    data = open(path, "rb").read()
    assert data[:4] == b"glTF", f"{path} n'est pas un GLB"
    length = struct.unpack("<I", data[8:12])[0]
    off, gltf, binchunk = 12, None, None
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
    comp = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}[acc["componentType"]]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    fmt = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}[acc["componentType"]]
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", comp * ncomp)
    out = []
    for i in range(acc["count"]):
        o = start + i * stride
        out.append(struct.unpack("<" + fmt * ncomp, binchunk[o:o + comp * ncomp]))
    return out


def node_matrix(node):
    if "matrix" in node:
        m = node["matrix"]  # column-major
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = node.get("translation", [0, 0, 0])
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
           [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
           [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    out = [[rot[i][j] * s[j] for j in range(3)] + [t[i]] for i in range(3)]
    out.append([0.0, 0.0, 0.0, 1.0])
    return out


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def collect(gltf, binchunk):
    """[(nom du matériau, positions monde, normales, indices)] pour chaque primitive."""
    mats = gltf.get("materials", [])
    prims = []

    def walk(node_idx, parent):
        node = gltf["nodes"][node_idx]
        world = matmul(parent, node_matrix(node))
        if "mesh" in node:
            for prim in gltf["meshes"][node["mesh"]]["primitives"]:
                pos = read_accessor(gltf, binchunk, prim["attributes"]["POSITION"])
                nrm = (read_accessor(gltf, binchunk, prim["attributes"]["NORMAL"])
                       if "NORMAL" in prim["attributes"] else None)
                idx = [i[0] for i in read_accessor(gltf, binchunk, prim["indices"])]
                world_pos = [tuple(world[i][0] * p[0] + world[i][1] * p[1] +
                                   world[i][2] * p[2] + world[i][3] for i in range(3))
                             for p in pos]
                mi = prim.get("material", -1)
                name = mats[mi]["name"] if 0 <= mi < len(mats) else "?"
                prims.append((name, world_pos, nrm, idx))
        for child in node.get("children", []):
            walk(child, world)

    ident = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    for n in gltf["scenes"][gltf.get("scene", 0)]["nodes"]:
        walk(n, ident)
    return prims


def loops(edges):
    """Regroupe des arêtes en composantes connexes, la plus grande d'abord."""
    adj = defaultdict(list)
    for u, v in edges:
        adj[u].append(v)
        adj[v].append(u)
    seen, out = set(), []
    for start in adj:
        if start in seen:
            continue
        stack, comp = [start], set()
        while stack:
            x = stack.pop()
            if x in comp:
                continue
            comp.add(x)
            seen.add(x)
            stack.extend(adj[x])
        out.append(comp)
    return sorted(out, key=len, reverse=True)


def bbox(points):
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    return (f"x[{lo[0]:+.3f},{hi[0]:+.3f}] y[{lo[1]:+.3f},{hi[1]:+.3f}] "
            f"z[{lo[2]:+.3f},{hi[2]:+.3f}]")


def check(path):
    gltf, binchunk = load_glb(path)
    print(f"\n=== {path} ===")
    faults = 0
    for pi, (mat, pos, nrm, idx) in enumerate(collect(gltf, binchunk)):
        def key(vi):
            return (round(pos[vi][0], 4), round(pos[vi][1], 4), round(pos[vi][2], 4))

        directed = defaultdict(int)
        inverted = 0
        for t in range(0, len(idx), 3):
            a, b, c = idx[t], idx[t + 1], idx[t + 2]
            for u, v in ((a, b), (b, c), (c, a)):
                ku, kv = key(u), key(v)
                directed[(ku, kv) if ku < kv else (kv, ku)] += 1 if ku < kv else -1
            if nrm is not None:
                pa, pb, pc = pos[a], pos[b], pos[c]
                ux = [pb[i] - pa[i] for i in range(3)]
                vx = [pc[i] - pa[i] for i in range(3)]
                g = (ux[1] * vx[2] - ux[2] * vx[1],
                     ux[2] * vx[0] - ux[0] * vx[2],
                     ux[0] * vx[1] - ux[1] * vx[0])
                gl = math.sqrt(sum(k * k for k in g))
                if gl > 1e-12:
                    vn = nrm[a]
                    if sum(g[i] / gl * vn[i] for i in range(3)) < -0.2:
                        inverted += 1
        border = [e for e, c in directed.items() if abs(c) == 1]
        wind = [e for e, c in directed.items() if abs(c) == 2]
        odd = [e for e, c in directed.items() if abs(c) > 2]
        if not (border or wind or odd or inverted):
            continue
        faults += inverted + len(wind) + len(odd)
        print(f" prim {pi:2d} mat={mat:14s} {len(idx) // 3:5d} tris | "
              f"normales inversées={inverted} trous={len(border)} "
              f"winding={len(wind)} non-manifold={len(odd)}")
        for label, edges in (("TROU", border), ("WINDING", wind), ("NON-MANIFOLD", odd)):
            for comp in loops(edges)[:4]:
                print(f"    {label:12s} {len(comp):4d} pts  {bbox(list(comp))}")
    return faults


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    total = sum(check(p) for p in sys.argv[1:])
    print(f"\n{total} défaut(s) bloquant(s) (normale inversée / winding / non-manifold).")
    sys.exit(1 if total else 0)
