#!/usr/bin/env python3
"""Génère une locomotive CC0 au format .glb (glTF 2.0 binaire), texture PNG embarquée.

Repère du modèle = repère LOCAL de la caisse du moteur Noire :
  x = droite, y = haut, z = arrière (l'app le dessine via body_position * body_orientation).
  origine = centre de la caisse ; les rails sont à y = -body_height (= -2.2 m).
Aucune dépendance externe (struct/json/zlib de la stdlib)."""
import struct, json, zlib, sys, math

# --- Géométrie : boîtes (x0,y0,z0, x1,y1,z1) en mètres, repère caisse ---------
BOXES = [
    (-1.20, -1.90, -8.80,  1.20, -1.30,  8.80),  # châssis
    (-1.15, -1.30, -8.40,  1.15,  0.35,  3.20),  # capot (avant = -Z)
    (-1.25, -1.30,  3.20,  1.25,  1.35,  8.40),  # cabine (arrière = +Z)
    (-1.25,  1.15,  3.00,  1.25,  1.50,  4.30),  # visière de toit
    (-0.35,  0.35, -6.20,  0.35,  0.95, -4.60),  # cheminée d'échappement
    (-0.90, -1.25, -9.10,  0.90, -0.20, -8.40),  # nez / tampon avant
    (-1.35, -2.15, -7.90, -1.05, -1.50, -6.10),  # roue avant gauche
    ( 1.05, -2.15, -7.90,  1.35, -1.50, -6.10),  # roue avant droite
    (-1.35, -2.15,  6.10, -1.05, -1.50,  7.90),  # roue arrière gauche
    ( 1.05, -2.15,  6.10,  1.35, -1.50,  7.90),  # roue arrière droite
]

positions, normals, uvs, indices = [], [], [], []


def add_box(x0, y0, z0, x1, y1, z1):
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
        base = len(positions)
        for k in range(4):
            positions.append(corners[k])
            normals.append(normal)
            uvs.append(uvq[k])
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


for b in BOXES:
    add_box(*b)


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
pos_b = b"".join(struct.pack("<fff", *p) for p in positions)
nrm_b = b"".join(struct.pack("<fff", *n) for n in normals)
uv_b = b"".join(struct.pack("<ff", *u) for u in uvs)
idx_b = b"".join(struct.pack("<I", i) for i in indices)


def align4(n):
    return (n + 3) & ~3


blocks = [pos_b, nrm_b, uv_b, idx_b, png]
offsets, cur = [], 0
for blk in blocks:
    offsets.append(cur)
    cur = align4(cur + len(blk))
total = cur
bin_data = bytearray(total)
for off, blk in zip(offsets, blocks):
    bin_data[off:off + len(blk)] = blk

pmin = [min(p[i] for p in positions) for i in range(3)]
pmax = [max(p[i] for p in positions) for i in range(3)]

gltf = {
    "asset": {"version": "2.0", "generator": "noire-train-gen (CC0)"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "Locomotive"}],
    "meshes": [{"primitives": [{
        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
        "indices": 3, "material": 0}]}],
    "materials": [{
        "name": "livree",
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicFactor": 0.1, "roughnessFactor": 0.75}}],
    "textures": [{"sampler": 0, "source": 0}],
    "images": [{"bufferView": 4, "mimeType": "image/png"}],
    "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(pos_b), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(nrm_b), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(uv_b), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(idx_b), "target": 34963},
        {"buffer": 0, "byteOffset": offsets[4], "byteLength": len(png)},
    ],
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

print(f"{out} : {len(positions)} sommets, {len(indices)} indices, "
      f"texture {len(png)} o, total {glb_len} o")
