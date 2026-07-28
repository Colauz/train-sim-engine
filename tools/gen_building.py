#!/usr/bin/env python3
"""Une TOUR de bureaux Neo-Tokyo, au format .glb (M31), textures procédurales embarquées.

Le bâtiment est un simple parallélépipède — la ville vient de la RÉPÉTITION (l'app en
seme des centaines, instanciés) et des FAÇADES : une texture de fenêtres dont une partie
est ALLUMÉE. Deux textures par variante :
  * base color : façade béton sombre, fenêtres éteintes à peine plus claires ;
  * émissive   : noir partout, sauf les fenêtres allumées (blanc chaud, blanc froid, et
                 quelques néons cyan/magenta). Le facteur émissif est > 1 (HDR) : la nuit,
                 les fenêtres flambent au-dessus de l'éclairage (l'ACES gère).

Trois variantes de gabarit (l'échelle d'instance du moteur est UNIFORME, donc la
variété des proportions est cuite dans le modèle) :
  * tower : 22 x 95 x 22 m  (tour haute)
  * block : 30 x 48 x 30 m  (immeuble courant)
  * slab  : 42 x 26 x 22 m  (barre basse)

Repère : origine au centre de la base, y = haut. Le semis pose le bâtiment sur
Terrain::height moins quelques mètres d'enfoncement. Aucune dépendance externe
(struct/json/zlib/math/random de la stdlib)."""
import struct, json, zlib, math, sys, random

TEX = 256             # côté des textures (px)
CELL_M = 3.0          # une cellule de fenêtre = 3 m (colonne) x 3 m (étage)
COLS, FLOORS = 4, 8   # la texture tuile 4 colonnes x 8 étages (répétée)

VARIANTS = {
    "tower": (22.0, 95.0, 22.0),
    "block": (30.0, 48.0, 30.0),
    "slab":  (42.0, 26.0, 22.0),
}

# Samplers : REPEAT (10497) — la grille de fenêtres se répète sur toute la façade.
SAMPLER = {"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}


def png_encode(w, h, rgba):
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # RGBA 8 bits
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def make_facades(seed):
    """Deux textures TEX x TEX : (base color, émissive). La tuile fait COLS x FLOORS
    cellules ; chaque cellule est une fenêtre entourée de béton. Semis figé : le modèle
    doit être reproductible à l'identique."""
    rnd = random.Random(seed)
    base = bytearray(TEX * TEX * 4)
    emis = bytearray(TEX * TEX * 4)
    cw, ch = TEX // COLS, TEX // FLOORS  # taille d'une cellule en px

    # Quelles cellules sont allumées, et de quelle couleur. Décision PAR CELLULE :
    # ~45 % de fenêtres allumées, majoritairement blanc chaud/froid, 8 % de néons.
    lit = {}
    for c in range(COLS):
        for f in range(FLOORS):
            if rnd.random() < 0.45:
                r = rnd.random()
                if r < 0.04:
                    color = (40, 255, 240)    # néon cyan
                elif r < 0.08:
                    color = (255, 60, 220)    # néon magenta
                elif r < 0.55:
                    color = (255, 214, 150)   # blanc chaud (logements)
                else:
                    color = (200, 225, 255)   # blanc froid (bureaux)
                lit[(c, f)] = color

    # Béton de façade : gris-bleu sombre, légèrement nuancé par bandes d'étage.
    for y in range(TEX):
        for x in range(TEX):
            o = (y * TEX + x) * 4
            shade = 0.9 + 0.2 * ((y // ch) % 2)
            base[o] = min(255, int(34 * shade))
            base[o + 1] = min(255, int(37 * shade))
            base[o + 2] = min(255, int(44 * shade))
            base[o + 3] = 255

    # Les fenêtres : rectangle centré dans la cellule (marges = l'embrasure béton).
    mx, my = int(cw * 0.18), int(ch * 0.22)
    for c in range(COLS):
        for f in range(FLOORS):
            x0, x1 = c * cw + mx, (c + 1) * cw - mx
            y0, y1 = f * ch + my, (f + 1) * ch - my
            glow = lit.get((c, f))
            for y in range(y0, y1):
                for x in range(x0, x1):
                    o = (y * TEX + x) * 4
                    # Fenêtre éteinte : vitre sombre à peine plus claire que le béton.
                    base[o], base[o + 1], base[o + 2] = 22, 26, 34
                    if glow:
                        # Fenêtre allumée : l'albédo suit aussi (jour), mais c'est
                        # l'ÉMISSIF qui porte la lumière la nuit.
                        base[o] = min(255, glow[0] // 3)
                        base[o + 1] = min(255, glow[1] // 3)
                        base[o + 2] = min(255, glow[2] // 3)
                        emis[o], emis[o + 1], emis[o + 2] = glow
                        emis[o + 3] = 255
    return png_encode(TEX, TEX, bytes(base)), png_encode(TEX, TEX, bytes(emis))


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []


def build_tower(part, sx, sy, sz):
    """Parallélépipède posé sur y=0, centré en x/z. UV en MÈTRES / CELL_M sur les façades
    (la grille de fenêtres garde son échelle quelle que soit la variante) ; toit et
    semelle rabattus sur le texel béton (0,0) — fenêtres sur un toit, jamais."""
    hx, hz = sx / 2.0, sz / 2.0
    # (normale, coins (x,y,z) des 4 sommets CCW vus de l'extérieur, axes u/v pour les UV)
    faces = [
        ((0, 0, 1),  [(-hx, 0, hz), (hx, 0, hz), (hx, sy, hz), (-hx, sy, hz)],
         lambda p: ((p[0] + hx) / CELL_M, p[1] / CELL_M)),
        ((0, 0, -1), [(hx, 0, -hz), (-hx, 0, -hz), (-hx, sy, -hz), (hx, sy, -hz)],
         lambda p: ((p[0] + hx) / CELL_M, p[1] / CELL_M)),
        ((1, 0, 0),  [(hx, 0, hz), (hx, 0, -hz), (hx, sy, -hz), (hx, sy, hz)],
         lambda p: ((p[2] + hz) / CELL_M, p[1] / CELL_M)),
        ((-1, 0, 0), [(-hx, 0, -hz), (-hx, 0, hz), (-hx, sy, hz), (-hx, sy, -hz)],
         lambda p: ((p[2] + hz) / CELL_M, p[1] / CELL_M)),
        ((0, 1, 0),  [(-hx, sy, hz), (hx, sy, hz), (hx, sy, -hz), (-hx, sy, -hz)],
         lambda p: (0.0, 0.0)),
        ((0, -1, 0), [(-hx, 0, -hz), (hx, 0, -hz), (hx, 0, hz), (-hx, 0, hz)],
         lambda p: (0.0, 0.0)),
    ]
    for n, corners, uv_of in faces:
        t = (0.0, 0.0, 1.0) if abs(n[2]) < 0.9 else (1.0, 0.0, 0.0)
        base = len(part.positions)
        for p in corners:
            part.positions.append(p)
            part.normals.append(n)
            part.uvs.append(uv_of(p))
            part.tangents.append((t[0], t[1], t[2], 1.0))
        part.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def align4(n):
    return (n + 3) & ~3


def write_glb(path, part, images, variant):
    geom = (b"".join(struct.pack("<fff", *v) for v in part.positions),
            b"".join(struct.pack("<fff", *v) for v in part.normals),
            b"".join(struct.pack("<ff", *v) for v in part.uvs),
            b"".join(struct.pack("<ffff", *v) for v in part.tangents),
            b"".join(struct.pack("<I", i) for i in part.indices))
    blocks = list(geom) + list(images)
    offsets, cur = [], 0
    for blk in blocks:
        offsets.append(cur)
        cur = align4(cur + len(blk))
    total = cur
    bin_data = bytearray(total)
    for off, blk in zip(offsets, blocks):
        bin_data[off:off + len(blk)] = blk

    pmin = [min(v[k] for v in part.positions) for k in range(3)]
    pmax = [max(v[k] for v in part.positions) for k in range(3)]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(part.positions), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": len(part.normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(part.uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": len(part.tangents), "type": "VEC4"},
        {"bufferView": 4, "componentType": 5125, "count": len(part.indices), "type": "SCALAR"},
    ]
    buffer_views = [{"buffer": 0, "byteOffset": offsets[k], "byteLength": len(blocks[k]),
                     "target": 34963 if k == 4 else 34962} for k in range(5)]
    for k in range(len(images)):
        buffer_views.append({"buffer": 0, "byteOffset": offsets[5 + k], "byteLength": len(images[k])})

    materials = [{
        "name": "facade",
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicFactor": 0.0,
            "roughnessFactor": 0.85,
        },
        # Émissif HDR : hors spec stricte (max 1,0 sans KHR_materials_emissive_strength),
        # assumé — c'est ce qui fait flamber les fenêtres la nuit.
        "emissiveTexture": {"index": 1},
        "emissiveFactor": [3.0, 3.0, 3.0],
    }]
    gltf = {
        "asset": {"version": "2.0", "generator": "noire-building-gen (CC0)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": f"batiment_{variant}"}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
            "indices": 4, "material": 0}]}],
        "materials": materials,
        "textures": [{"sampler": 0, "source": 0}, {"sampler": 0, "source": 1}],
        "images": [{"bufferView": 5, "mimeType": "image/png"},
                   {"bufferView": 6, "mimeType": "image/png"}],
        "samplers": [SAMPLER],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": total}],
    }
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
    sx, sy, sz = VARIANTS[variant]
    print(f"{path} : {variant} {sx:.0f}x{sy:.0f}x{sz:.0f} m — {len(part.positions)} sommets, "
          f"2 textures {TEX}x{TEX} ({glb_len} o)")


def build(variant, out):
    part = Part()
    build_tower(part, *VARIANTS[variant])
    # Graine par variante : deux gabarits n'ont pas la même carte de fenêtres allumées.
    seed = {"tower": 20260728, "block": 20260729, "slab": 20260730}[variant]
    base_png, emis_png = make_facades(seed)
    write_glb(out, part, (base_png, emis_png), variant)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "building.glb"
    variant = sys.argv[2] if len(sys.argv) > 2 else "tower"
    if variant not in VARIANTS:
        sys.exit(f"variante inconnue : {variant} (attendu : {', '.join(VARIANTS)})")
    build(variant, out)
