#!/usr/bin/env python3
"""Génère une locomotive CC0 au format .glb (glTF 2.0 binaire), texture PNG embarquée.

Repère du modèle = repère LOCAL de la caisse du moteur Noire :
  x = droite, y = haut, z = arrière (l'app le dessine via body_position * body_orientation).
  origine = centre de la caisse ; les rails sont à y = -body_height (= -2.2 m).

La loco est découpée en PRIMITIVES par matériau (M8 étape 5) : l'app itère sur
model->primitives et lie le descriptor set du matériau de chacune, donc une primitive
par matériau suffit à faire cohabiter acier, peinture et verre sur le même maillage.
Aucune dépendance externe (struct/json/zlib de la stdlib)."""
import struct, json, zlib, sys

# --- Matériaux PBR (metallic-roughness glTF) ----------------------------------
# Rappel de convention, qui dicte tout ce qui suit :
#   metallic = 1 => la baseColor n'est PAS un pigment, c'est la couleur de réflexion
#                   (F0) du métal, et le terme diffus disparaît.
#   metallic = 0 => F0 = 0.04 fixe (diélectrique), la baseColor est le pigment diffus.
# Les FACTEURS glTF sont linéaires ; seules les TEXTURES sont encodées en sRGB.
MATERIALS = [
    # Acier usé : brillant sans être chromé. Pas de texture — la livrée verte n'a rien
    # à faire sur de l'acier nu, le facteur seul décide (secours blanc 1x1 côté moteur).
    {"name": "acier", "factor": [0.52, 0.53, 0.55, 1.0],
     "metallic": 1.0, "roughness": 0.25, "textured": False},
    # Peinture industrielle : diélectrique satiné. Seule primitive à porter la livrée.
    {"name": "peinture", "factor": [1.0, 1.0, 1.0, 1.0],
     "metallic": 0.0, "roughness": 0.6, "textured": True},
    # Verre : diélectrique, surtout PAS métallique. Un baseColor noir + metallic élevé
    # donnerait F0 ~ 0.008, soit une surface qui ne réfléchit rien (un trou noir mat) —
    # l'inverse du verre. À metallic 0, F0 = 0.04 et c'est le Fresnel qui fait tout :
    # sombre de face, miroir du ciel aux angles rasants.
    {"name": "vitrage", "factor": [0.02, 0.025, 0.03, 1.0],
     "metallic": 0.0, "roughness": 0.05, "textured": False},
]

# --- Géométrie : boîtes (x0,y0,z0, x1,y1,z1) en mètres, repère caisse ---------
# Chaque groupe devient UNE primitive glTF portant le matériau de même index.
CAISSE = [
    (-1.15, -1.30, -8.40,  1.15,  0.35,  3.20),  # capot (avant = -Z)
    (-1.25, -1.30,  3.20,  1.25,  1.35,  8.40),  # cabine (arrière = +Z)
    (-1.25,  1.15,  3.00,  1.25,  1.50,  4.30),  # visière de toit
    (-0.35,  0.35, -6.20,  0.35,  0.95, -4.60),  # cheminée d'échappement
    (-0.90, -1.25, -9.10,  0.90, -0.20, -8.40),  # nez / tampon avant
]
ACIER = [
    (-1.20, -1.90, -8.80,  1.20, -1.30,  8.80),  # châssis bas
    (-1.35, -2.15, -7.90, -1.05, -1.50, -6.10),  # roue avant gauche
    ( 1.05, -2.15, -7.90,  1.35, -1.50, -6.10),  # roue avant droite
    (-1.35, -2.15,  6.10, -1.05, -1.50,  7.90),  # roue arrière gauche
    ( 1.05, -2.15,  6.10,  1.35, -1.50,  7.90),  # roue arrière droite
]
# Vitrage : dalles fines PLAQUÉES sur la cabine (x ±1.25, z 3.20..8.40). Chacune dépasse
# de 3 cm vers l'extérieur et s'enfonce dans la cabine : aucune face n'est coplanaire
# avec la carrosserie, donc aucun z-fighting (le décalage est le seul garde-fou ici).
VITRAGE = [
    (-0.95,  0.45,  3.17,  0.95,  1.10,  3.35),  # pare-brise (sous la visière)
    (-1.28,  0.30,  4.20, -1.10,  1.05,  7.60),  # vitre latérale gauche
    ( 1.10,  0.30,  4.20,  1.28,  1.05,  7.60),  # vitre latérale droite
    (-0.95,  0.30,  8.25,  0.95,  1.05,  8.43),  # lunette arrière
]
GROUPS = [ACIER, CAISSE, VITRAGE]  # l'ordre suit MATERIALS


class Part:
    """Un tampon de sommets par primitive : les index glTF sont locaux à la primitive."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []


def add_box(part, x0, y0, z0, x1, y1, z1):
    faces = [
        ((0, 0, 1),  [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
        ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
        ((1, 0, 0),  [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
        ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
        ((0, 1, 0),  [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
        ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
    ]
    uvq = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    for normal, corners in faces:
        base = len(part.positions)
        for k in range(4):
            part.positions.append(corners[k])
            part.normals.append(normal)
            part.uvs.append(uvq[k])
        part.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


parts = []
for boxes in GROUPS:
    p = Part()
    for b in boxes:
        add_box(p, *b)
    parts.append(p)


# --- Texture PNG embarquée : dégradé vertical vert « livrée loco » -------------
def png_encode(w, h, rgba):
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # RGBA 8 bits
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)  # filtre None
        raw += rgba[y * stride:(y + 1) * stride]
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def make_texture(size=16):
    top = (74, 112, 92)
    bot = (34, 54, 46)
    px = bytearray()
    for y in range(size):
        t = y / (size - 1)  # y=0 en haut de l'image (v=0)
        r = round(top[0] * (1 - t) + bot[0] * t)
        g = round(top[1] * (1 - t) + bot[1] * t)
        bl = round(top[2] * (1 - t) + bot[2] * t)
        for x in range(size):
            px += bytes((r, g, bl, 255))
    return png_encode(size, size, bytes(px))


png = make_texture()

# --- Sérialisation binaire ----------------------------------------------------
blocks, part_blocks = [], []
for p in parts:
    b = (b"".join(struct.pack("<fff", *v) for v in p.positions),
         b"".join(struct.pack("<fff", *v) for v in p.normals),
         b"".join(struct.pack("<ff", *v) for v in p.uvs),
         b"".join(struct.pack("<I", i) for i in p.indices))
    part_blocks.append(b)
    blocks.extend(b)
blocks.append(png)


def align4(n):
    return (n + 3) & ~3


offsets, cur = [], 0
for blk in blocks:
    offsets.append(cur)
    cur = align4(cur + len(blk))
total = cur
bin_data = bytearray(total)
for off, blk in zip(offsets, blocks):
    bin_data[off:off + len(blk)] = blk

# 4 bufferViews/accessors par primitive (pos, nrm, uv, idx), puis le PNG en dernier.
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

png_view = 4 * len(parts)
buffer_views.append({"buffer": 0, "byteOffset": offsets[png_view], "byteLength": len(png)})

materials = []
for m in MATERIALS:
    pbr = {"baseColorFactor": m["factor"],
           "metallicFactor": m["metallic"], "roughnessFactor": m["roughness"]}
    if m["textured"]:
        pbr["baseColorTexture"] = {"index": 0}
    materials.append({"name": m["name"], "pbrMetallicRoughness": pbr})

gltf = {
    "asset": {"version": "2.0", "generator": "noire-train-gen (CC0)"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "Locomotive"}],
    "meshes": [{"primitives": primitives}],
    "materials": materials,
    "textures": [{"sampler": 0, "source": 0}],
    "images": [{"bufferView": png_view, "mimeType": "image/png"}],
    "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}],
    "accessors": accessors,
    "bufferViews": buffer_views,
    "buffers": [{"byteLength": total}],
}

json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))       # pad espaces
bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)

out = sys.argv[1] if len(sys.argv) > 1 else "train.glb"
glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
with open(out, "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, glb_len))            # header glTF
    f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))        # chunk JSON
    f.write(json_bytes)
    f.write(struct.pack("<II", len(bin_pad), 0x004E4942))           # chunk BIN
    f.write(bin_pad)

nv = sum(len(p.positions) for p in parts)
ni = sum(len(p.indices) for p in parts)
print(f"{out} : {len(parts)} primitive(s) [" +
      ", ".join(f"{m['name']}={len(p.positions)}v" for m, p in zip(MATERIALS, parts)) +
      f"], {nv} sommets, {ni} indices, texture {len(png)} o, total {glb_len} o")
