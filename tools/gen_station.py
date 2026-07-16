#!/usr/bin/env python3
"""Un MODULE de gare TGV, au format .glb (M18).

La gare est bâtie en RÉPÉTANT ce module le long de la voie (l'app le pose tous les 40 m
sur la spline, orienté comme la voie : il épouse donc les courbes sans se déformer). Un
module contient, dans son repère local (x = latéral, y = hauteur au-dessus du plan de
roulement, z = le long de la voie) :
  * deux QUAIS béton encadrant la voie, dont le dessus est à +1,00 m du rail (= le seuil
    des portes du TGV, dont le plancher est à 1,05 m) ;
  * des POTEAUX d'acier à intervalle régulier (20 m) ;
  * une VERRIÈRE (toiture vitrée stylisée) portée par les poteaux, une par quai.

Repère : y = 0 est le PLAN DE ROULEMENT (comme la caisse : rail à -2,20 côté train, mais
ici on raisonne depuis le rail). Aucune dépendance externe (stdlib seule)."""
import struct, json, sys

MODULE_LEN = 40.0            # longueur d'un module ; l'app en pose 10 sur 0-400 m
HZ = MODULE_LEN / 2.0

PLAT_INNER = 1.75           # bord quai côté voie (le TGV fait 1,45 de demi-largeur)
PLAT_OUTER = 5.20           # bord extérieur
PLAT_TOP = 1.00             # dessus du quai = plan de roulement + 1 m (seuil des portes)
PLAT_BOTTOM = -1.60         # base enterrée
PILLAR_HALF = 0.14
PILLAR_TOP = 5.40
ROOF_Y = 5.40
ROOF_THICK = 0.16
PILLAR_Z = (-10.0, 10.0)    # 2 poteaux par quai et par module => un tous les 20 m

MATERIALS = [
    {"name": "beton", "factor": [0.62, 0.62, 0.60, 1.0], "metallic": 0.0, "roughness": 0.90},
    {"name": "acier", "factor": [0.48, 0.50, 0.53, 1.0], "metallic": 0.7, "roughness": 0.45},
    {"name": "verriere", "factor": [0.60, 0.68, 0.78, 1.0], "metallic": 0.0, "roughness": 0.08},
]
MAT_CONCRETE, MAT_STEEL, MAT_GLASS = 0, 1, 2


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []


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
    materials = [{"name": MATERIALS[m]["name"],
                  "pbrMetallicRoughness": {"baseColorFactor": MATERIALS[m]["factor"],
                                           "metallicFactor": MATERIALS[m]["metallic"],
                                           "roughnessFactor": MATERIALS[m]["roughness"]}}
                 for m, _ in used]
    gltf = {"asset": {"version": "2.0", "generator": "noire-station (CC0)"},
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
    print(f"{path} : module de gare {MODULE_LEN:.0f} m, {nv} sommets, {glb_len} o")


def build_station(out):
    parts = [Part(), Part(), Part()]
    # Deux quais, de part et d'autre de la voie. Ils débordent LÉGÈREMENT en z (± un chouïa)
    # pour que deux modules voisins se recouvrent au raccord et ne laissent aucun joint.
    z0, z1 = -HZ - 0.05, HZ + 0.05
    build_box(parts[MAT_CONCRETE], -PLAT_OUTER, PLAT_BOTTOM, z0, -PLAT_INNER, PLAT_TOP, z1)  # gauche
    build_box(parts[MAT_CONCRETE], PLAT_INNER, PLAT_BOTTOM, z0, PLAT_OUTER, PLAT_TOP, z1)    # droite

    # Poteaux + verrière, un ensemble par quai.
    for sign in (-1.0, 1.0):
        cx = sign * (PLAT_OUTER - 0.7)  # près du bord extérieur, hors du gabarit des portes
        for pz in PILLAR_Z:
            build_box(parts[MAT_STEEL], cx - PILLAR_HALF, PLAT_TOP, pz - PILLAR_HALF,
                      cx + PILLAR_HALF, PILLAR_TOP, pz + PILLAR_HALF)
        # Verrière : dalle vitrée au-dessus du quai, en léger débord vers la voie.
        rx0 = sign * PLAT_INNER - (0.6 if sign > 0 else -0.6)
        rx1 = sign * (PLAT_OUTER + 0.3)
        lo, hi = min(rx0, rx1), max(rx0, rx1)
        build_box(parts[MAT_GLASS], lo, ROOF_Y, z0, hi, ROOF_Y + ROOF_THICK, z1)

    write_glb(out, parts, "gare_module")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "station.glb"
    build_station(out)
