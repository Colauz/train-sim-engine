#!/usr/bin/env python3
"""Génère un GABARIT de motrice TGV au format .glb — cotes réelles, formes grossières.

Ce n'est PAS un modèle : c'est un volume de référence, aux dimensions exactes d'une
motrice TGV (~22.15 m x 2.90 m x 4.10 m au-dessus du rail), destiné à donner le sens de
l'ÉCHELLE tant que le vrai .glb n'est pas là. Il sert aussi de mire pour régler
`ModelTransform` (application.cpp) : si le gabarit tombe juste, le calibrage est bon.

Repère = repère LOCAL de la caisse du moteur Noire :
  x = droite, y = haut, z = arrière (l'app le dessine via body_position * body_orientation).
  origine = centre de la caisse ; le PLAN DE ROULEMENT est à y = -body_height (= -2.2 m).
Aucune dépendance externe (struct/json/zlib de la stdlib)."""
import struct, json, zlib, sys

# --- Cotes réelles d'une motrice TGV (Réseau/Duplex), en mètres ----------------
RAIL = -2.20        # plan de roulement dans le repère caisse (= -body_height)
LENGTH = 22.15      # longueur hors tampons d'une motrice
HALF_W = 1.45       # 2.90 m de large (gabarit UIC)
ROOF = RAIL + 4.10  # 4.10 m au-dessus du rail = 1.90 dans le repère caisse
FLOOR = RAIL + 1.05  # plancher/bas de caisse
NOSE = 3.60         # longueur du nez profilé (avant = -Z)

# --- Matériaux PBR : mêmes conventions que gen_train.py ------------------------
# metallic = 1 => la baseColor est la couleur de RÉFLEXION (F0), pas un pigment.
# metallic = 0 => F0 = 0.04, la baseColor est le pigment diffus. Facteurs LINÉAIRES.
MATERIALS = [
    # Livrée TGV : gris clair, satiné. Pas de texture — le gabarit ne prétend à rien.
    {"name": "livree", "factor": [0.62, 0.63, 0.66, 1.0], "metallic": 0.0, "roughness": 0.45},
    # Bandeau vitré : diélectrique (surtout PAS métallique — un baseColor noir + metallic
    # élevé donnerait F0 ~ 0.008, une surface qui ne réfléchit rien).
    {"name": "vitrage", "factor": [0.02, 0.025, 0.03, 1.0], "metallic": 0.0, "roughness": 0.05},
    # Bogies + organes de roulement : acier sombre.
    {"name": "acier", "factor": [0.42, 0.43, 0.45, 1.0], "metallic": 1.0, "roughness": 0.35},
]

# --- Géométrie : (x0,y0,z0, x1,y1,z1) en mètres, repère caisse -----------------
# Le nez est un tronc de pyramide, donc pas une boîte : il est ajouté à part.
LIVREE = [
    (-HALF_W, FLOOR, -LENGTH / 2 + NOSE,  HALF_W, ROOF, LENGTH / 2),   # caisse
    (-1.20, ROOF, -LENGTH / 2 + NOSE + 1.0,  1.20, ROOF + 0.35, LENGTH / 2 - 2.0),  # carénage toit
]
VITRAGE = [
    # Bandeau latéral gauche/droit : plaqué 3 cm au-delà de la caisse, et enfoncé dedans,
    # pour qu'aucune face ne soit coplanaire avec elle (seul garde-fou contre le z-fighting).
    (-HALF_W - 0.03, RAIL + 2.60, -LENGTH / 2 + NOSE + 0.6, -HALF_W + 0.12, RAIL + 3.35, LENGTH / 2 - 0.6),
    (HALF_W - 0.12, RAIL + 2.60, -LENGTH / 2 + NOSE + 0.6,  HALF_W + 0.03, RAIL + 3.35, LENGTH / 2 - 0.6),
    # Lunette de cabine, à l'arrière de la zone de nez.
    (-1.00, RAIL + 2.70, -LENGTH / 2 + NOSE - 0.15, 1.00, RAIL + 3.40, -LENGTH / 2 + NOSE + 0.10),
]
ACIER = [
    # Deux bogies. Cotes plausibles : empattement 3 m, sous le plancher.
    (-1.25, RAIL + 0.05, -LENGTH / 2 + NOSE + 1.0, 1.25, FLOOR, -LENGTH / 2 + NOSE + 4.0),
    (-1.25, RAIL + 0.05, LENGTH / 2 - 4.5, 1.25, FLOOR, LENGTH / 2 - 1.5),
]

positions, normals, uvs, indices = [], [], [], []


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []


def add_quad(part, p0, p1, p2, p3):
    """Quad (p0,p1,p2,p3) ; la normale se déduit du winding, ce qui gère aussi les faces
    inclinées du nez — un simple axe ne suffirait pas."""
    import math
    ux, uy, uz = (p1[i] - p0[i] for i in range(3))
    vx, vy, vz = (p3[i] - p0[i] for i in range(3))
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
    n = (nx / ln, ny / ln, nz / ln)
    base = len(part.positions)
    for p, uv in zip((p0, p1, p2, p3), ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))):
        part.positions.append(p)
        part.normals.append(n)
        part.uvs.append(uv)
    part.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def add_box(part, x0, y0, z0, x1, y1, z1):
    c = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    for f in ((4, 5, 6, 7), (1, 0, 3, 2), (5, 1, 2, 6), (0, 4, 7, 3), (3, 7, 6, 2), (0, 1, 5, 4)):
        add_quad(part, c[f[0]], c[f[1]], c[f[2]], c[f[3]])


def add_nose(part):
    """Nez profilé : tronc de pyramide entre la section pleine de la caisse et un museau
    bas et étroit. Grossier, mais il donne la silhouette effilée qui distingue au premier
    coup d'oeil une motrice TGV d'une boîte."""
    zb = -LENGTH / 2 + NOSE   # base, contre la caisse
    zt = -LENGTH / 2          # pointe
    tip_half, tip_low, tip_high = 0.75, RAIL + 0.75, RAIL + 2.30
    b = [(-HALF_W, FLOOR, zb), (HALF_W, FLOOR, zb), (HALF_W, ROOF, zb), (-HALF_W, ROOF, zb)]
    t = [(-tip_half, tip_low, zt), (tip_half, tip_low, zt),
         (tip_half, tip_high, zt), (-tip_half, tip_high, zt)]
    add_quad(part, t[0], t[1], t[2], t[3])          # museau
    add_quad(part, b[0], t[0], t[3], b[3])          # flanc gauche
    add_quad(part, t[1], b[1], b[2], t[2])          # flanc droit
    add_quad(part, b[3], t[3], t[2], b[2])          # capot supérieur
    add_quad(part, t[0], b[0], b[1], t[1])          # dessous


parts = [Part(), Part(), Part()]
for box in LIVREE:
    add_box(parts[0], *box)
add_nose(parts[0])
for box in VITRAGE:
    add_box(parts[1], *box)
for box in ACIER:
    add_box(parts[2], *box)


# --- Sérialisation binaire (identique à gen_train.py) --------------------------
def align4(n):
    return (n + 3) & ~3


blocks, part_blocks = [], []
for p in parts:
    b = (b"".join(struct.pack("<fff", *v) for v in p.positions),
         b"".join(struct.pack("<fff", *v) for v in p.normals),
         b"".join(struct.pack("<ff", *v) for v in p.uvs),
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
for i, (p, blks) in enumerate(zip(parts, part_blocks)):
    base = 4 * i
    pmin = [min(v[k] for v in p.positions) for k in range(3)]
    pmax = [max(v[k] for v in p.positions) for k in range(3)]
    accessors += [
        {"bufferView": base, "componentType": 5126, "count": len(p.positions), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": base + 1, "componentType": 5126, "count": len(p.normals), "type": "VEC3"},
        {"bufferView": base + 2, "componentType": 5126, "count": len(p.uvs), "type": "VEC2"},
        {"bufferView": base + 3, "componentType": 5125, "count": len(p.indices), "type": "SCALAR"},
    ]
    targets = [34962, 34962, 34962, 34963]
    for k in range(4):
        buffer_views.append({"buffer": 0, "byteOffset": offsets[base + k],
                             "byteLength": len(blks[k]), "target": targets[k]})
    primitives.append({
        "attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2},
        "indices": base + 3, "material": i})

materials = [{"name": m["name"],
              "pbrMetallicRoughness": {"baseColorFactor": m["factor"],
                                       "metallicFactor": m["metallic"],
                                       "roughnessFactor": m["roughness"]}}
             for m in MATERIALS]

gltf = {
    "asset": {"version": "2.0", "generator": "noire-tgv-gabarit (CC0)"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "TGV_gabarit"}],
    "meshes": [{"primitives": primitives}],
    "materials": materials,
    "accessors": accessors,
    "bufferViews": buffer_views,
    "buffers": [{"byteLength": total}],
}

json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))
bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)

out = sys.argv[1] if len(sys.argv) > 1 else "tgv_gabarit.glb"
glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
with open(out, "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, glb_len))
    f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
    f.write(json_bytes)
    f.write(struct.pack("<II", len(bin_pad), 0x004E4942))
    f.write(bin_pad)

nv = sum(len(p.positions) for p in parts)
print(f"{out} : gabarit motrice TGV {LENGTH:.2f} m x {HALF_W * 2:.2f} m x {ROOF - RAIL:.2f} m "
      f"au-dessus du rail — {len(parts)} primitive(s), {nv} sommets, {glb_len} o")
