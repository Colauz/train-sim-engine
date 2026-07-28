#!/usr/bin/env python3
"""Modèle 3D de lampadaire urbain japonais (M37), au format .glb, sans dépendances.

Composants :
  * Mât vertical en acier gris foncé (hauteur 6.0 m, Ø 16 cm).
  * Crosse horizontale recourbée en haut (longueur 1.5 m).
  * Boîtier de luminaire avec sous-face émissive jaune/orange sodium (HDR émissif).
"""

import struct
import json
import math
import sys

MATERIALS = [
    # 0: acier sombre pour le mât
    {"name": "mat_mat", "factor": [0.12, 0.13, 0.15, 1.0], "metallic": 0.80, "roughness": 0.35},
    # 1: ampoule sodium émissive (jaune/orange vif)
    {"name": "mat_ampoule", "factor": [1.0, 0.85, 0.30, 1.0], "emissive": [5.0, 3.8, 0.8],
     "metallic": 0.0, "roughness": 0.10},
]
MAT_MAT, MAT_BULB = 0, 1


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

    def add_quad(self, p0, p1, p2, p3, n=None):
        if n is None:
            v0 = (p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2])
            v1 = (p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2])
            nx = v0[1] * v1[2] - v0[2] * v1[1]
            ny = v0[2] * v1[0] - v0[0] * v1[2]
            nz = v0[0] * v1[1] - v0[1] * v1[0]
            l = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            n = (nx / l, ny / l, nz / l)
        base = len(self.positions)
        tg = (1.0, 0.0, 0.0, 1.0)
        self.positions.extend([p0, p1, p2, p3])
        self.normals.extend([n, n, n, n])
        self.uvs.extend([(0, 0), (1, 0), (1, 1), (0, 1)])
        self.tangents.extend([tg, tg, tg, tg])
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def add_box(self, x0, y0, z0, x1, y1, z1):
        c000, c100 = (x0, y0, z0), (x1, y0, z0)
        c110, c010 = (x1, y1, z0), (x0, y1, z0)
        c001, c101 = (x0, y0, z1), (x1, y0, z1)
        c111, c011 = (x1, y1, z1), (x0, y1, z1)
        self.add_quad(c000, c100, c110, c010, (0, 0, -1))
        self.add_quad(c101, c001, c011, c111, (0, 0, 1))
        self.add_quad(c001, c000, c010, c011, (-1, 0, 0))
        self.add_quad(c100, c101, c111, c110, (1, 0, 0))
        self.add_quad(c001, c101, c100, c000, (0, -1, 0))
        self.add_quad(c010, c110, c111, c011, (0, 1, 0))

    def add_cylinder(self, x0, y0, z0, x1, y1, z1, r, segs=8):
        dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
        l = math.sqrt(dx * dx + dy * dy + dz * dz) or 1.0
        dir_v = (dx / l, dy / l, dz / l)
        up = (0, 1, 0) if abs(dir_v[1]) < 0.9 else (1, 0, 0)
        ux = up[1] * dir_v[2] - up[2] * dir_v[1]
        uy = up[2] * dir_v[0] - up[0] * dir_v[2]
        uz = up[0] * dir_v[1] - up[1] * dir_v[0]
        ul = math.sqrt(ux * ux + uy * uy + uz * uz) or 1.0
        u_v = (ux / ul, uy / ul, uz / ul)
        vx = dir_v[1] * u_v[2] - dir_v[2] * u_v[1]
        vy = dir_v[2] * u_v[0] - dir_v[0] * u_v[2]
        vz = dir_v[0] * u_v[1] - dir_v[1] * u_v[0]

        for i in range(segs):
            a0 = 2.0 * math.pi * i / segs
            a1 = 2.0 * math.pi * (i + 1) / segs
            cos0, sin0 = math.cos(a0), math.sin(a0)
            cos1, sin1 = math.cos(a1), math.sin(a1)

            n0 = (cos0 * u_v[0] + sin0 * vx, cos0 * u_v[1] + sin0 * vy, cos0 * u_v[2] + sin0 * vz)
            n1 = (cos1 * u_v[0] + sin1 * vx, cos1 * u_v[1] + sin1 * vy, cos1 * u_v[2] + sin1 * vz)

            p0 = (x0 + r * n0[0], y0 + r * n0[1], z0 + r * n0[2])
            p1 = (x0 + r * n1[0], y0 + r * n1[1], z0 + r * n1[2])
            p2 = (x1 + r * n1[0], y1 + r * n1[1], z1 + r * n1[2])
            p3 = (x1 + r * n0[0], y1 + r * n0[1], z1 + r * n0[2])

            base = len(self.positions)
            tg = (1.0, 0.0, 0.0, 1.0)
            self.positions.extend([p0, p1, p2, p3])
            self.normals.extend([n0, n1, n1, n0])
            self.uvs.extend([(0, 0), (1, 0), (1, 1), (0, 1)])
            self.tangents.extend([tg, tg, tg, tg])
            self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def build_streetlamp():
    parts = [Part() for _ in MATERIALS]
    # Mât (6m, r=0.07)
    parts[MAT_MAT].add_cylinder(0.0, 0.0, 0.0, 0.0, 6.0, 0.0, 0.07, segs=10)
    # Crosse (1.4m)
    parts[MAT_MAT].add_cylinder(0.0, 5.9, 0.0, 1.4, 6.1, 0.0, 0.045, segs=8)
    # Boîtier luminaire
    parts[MAT_MAT].add_box(1.25, 6.02, -0.12, 1.55, 6.15, 0.12)
    # Ampoule émissive sodium
    parts[MAT_BULB].add_quad(
        (1.28, 6.01, -0.09), (1.52, 6.01, -0.09),
        (1.52, 6.01, 0.09), (1.28, 6.01, 0.09),
        (0.0, -1.0, 0.0)
    )
    return parts


def align4(n):
    return (n + 3) & ~3


def write_glb(path, parts):
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

    materials = []
    for mdef in MATERIALS:
        m = {"name": mdef["name"],
             "pbrMetallicRoughness": {"baseColorFactor": mdef["factor"],
                                      "metallicFactor": mdef["metallic"],
                                      "roughnessFactor": mdef["roughness"]}}
        if "emissive" in mdef:
            m["emissiveFactor"] = mdef["emissive"]
        materials.append(m)

    gltf = {"asset": {"version": "2.0", "generator": "noire-streetlamp (CC0)"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "streetlamp"}],
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
    print(f"{path} : {nv} sommets, {len(used)} primitives, {glb_len} o (streetlamp)")


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    parts = build_streetlamp()
    write_glb(f"{outdir}/streetlamp.glb", parts)
