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

CELL_M = 3.0          # une cellule de fenêtre = 3 m (colonne) x 3 m (étage)

VARIANTS = {
    "tower": (22.0, 95.0, 22.0),
    "block": (30.0, 48.0, 30.0),
    "slab":  (42.0, 26.0, 22.0),
}

# La texture couvre UNE FAÇADE ENTIÈRE : grille complète de cellules ~3 m x 3 m, chaque
# cellule tirée indépendamment — plus aucun motif répété à l'intérieur d'une façade.
# UVs en 0..1 (les façades étroites ne parcourent qu'une fraction de la largeur, les
# fenêtres restent carrées en mètres). REPEAT est conservé pour que l'offset UV par
# instance du moteur varie le tirage d'un immeuble à l'autre.
SAMPLER = {"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}


def grid_of(sx, sy, sz):
    """(cols_x, cols_z, floors) : nombre de cellules de CELL_M m par façade."""
    return (max(1, round(sx / CELL_M)),
            max(1, round(sz / CELL_M)),
            max(1, round(sy / CELL_M)))


def tex_size(cols_max, floors):
    """Dimensions de texture (puissances de deux, ~8-16 px par cellule)."""
    def pot(n):
        p = 8
        while p < n:
            p *= 2
        return p
    return pot(cols_max * 8), pot(floors * 8)


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


# M56 — LES FAÇADES ÉTAIENT PEINTES EN NUIT.
#
# Le béton était engendré à (34, 37, 44) et le vitrage éteint à (22, 26, 34) : en sRGB
# c'est 13 % et 10 %, soit un albédo LINÉAIRE de 1,6 % — plus sombre que du charbon
# (0,04) et six fois plus sombre que l'enrobé de la chaussée. Conséquence : la ville
# restait un mur NOIR en plein midi, avec ses fenêtres allumées par-dessus. C'était
# l'incohérence la plus voyante du jeu.
#
# Le piège est classique : on règle un albédo à l'oeil sur une scène de nuit, et on
# encode l'ÉCLAIRAGE dans la MATIÈRE. Un moteur PBR ne le pardonne pas — c'est
# l'éclairage qui doit faire la nuit, l'albédo décrit ce que la surface renvoie, point.
# Les valeurs ci-dessous sont donc des réflectances plausibles de bâtiment (25 à 45 %
# en sRGB), et la nuit reste noire parce que le soleil est sous l'horizon.
FACADES = {
    # (béton, vitrage éteint) — une famille par gabarit, pour que la ville ne soit pas
    # monochrome : les trois textures sont les seules du jeu, la variété vient d'elles.
    "tower": ((122, 132, 146), (78, 94, 112)),   # tour de bureaux, verre teinté bleu
    "block": ((168, 164, 156), (86, 92, 100)),   # immeuble courant, béton clair
    "slab":  ((140, 138, 134), (74, 80, 88)),    # barre basse, béton gris
}


def make_facades(seed, cols_max, floors, tex_w, tex_h, palette):
    """Deux textures tex_w x tex_h : (base color, émissive). La grille fait cols_max x
    floors cellules couvrant la façade entière ; chaque cellule est une fenêtre entourée
    de béton, tirée indépendamment. Semis figé : reproductible à l'identique."""
    rnd = random.Random(seed)
    base = bytearray(tex_w * tex_h * 4)
    emis = bytearray(tex_w * tex_h * 4)
    cw, ch = tex_w // cols_max, tex_h // floors  # taille d'une cellule en px

    # Quelles cellules sont allumées, et de quelle couleur. Décision PAR CELLULE :
    # ~40 % de fenêtres allumées, majoritairement blanc chaud/froid, 8 % de néons.
    lit = {}
    for c in range(cols_max):
        for f in range(floors):
            if rnd.random() < 0.40:
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

    # Béton de façade, nuancé par bandes d'étage (les nez de dalle d'un immeuble ne
    # renvoient pas la lumière comme ses allèges).
    concrete, glass = palette
    for y in range(tex_h):
        for x in range(tex_w):
            o = (y * tex_w + x) * 4
            shade = 0.92 + 0.16 * ((y // ch) % 2)
            for k in range(3):
                base[o + k] = min(255, int(concrete[k] * shade))
            base[o + 3] = 255

    # Les fenêtres : rectangle centré dans la cellule (marges = l'embrasure béton).
    mx, my = int(cw * 0.18), int(ch * 0.22)
    for c in range(cols_max):
        for f in range(floors):
            x0, x1 = c * cw + mx, (c + 1) * cw - mx
            y0, y1 = f * ch + my, (f + 1) * ch - my
            glow = lit.get((c, f))
            for y in range(y0, y1):
                for x in range(x0, x1):
                    o = (y * tex_w + x) * 4
                    # Le vitrage a le MÊME albédo qu'il soit allumé ou non : de jour, une
                    # fenêtre derrière laquelle brûle un néon ressemble à n'importe quelle
                    # autre fenêtre. C'est l'ÉMISSIF, et lui seul, qui les distingue la
                    # nuit — sinon la ville garderait à midi le damier de son éclairage
                    # nocturne, ce qui était exactement le défaut d'avant.
                    base[o], base[o + 1], base[o + 2] = glass
                    if glow:
                        emis[o], emis[o + 1], emis[o + 2] = glow
                        emis[o + 3] = 255
    return png_encode(tex_w, tex_h, bytes(base)), png_encode(tex_w, tex_h, bytes(emis))


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []


def build_tower(part, sx, sy, sz, cols_max, floors):
    """Parallélépipède posé sur y=0, centré en x/z. UV en 0..1 sur les façades : la
    texture couvre une façade ENTIÈRE (cols_max x floors cellules) ; une façade plus
    étroite que cols_max cellules n'en parcourt qu'une fraction, les fenêtres gardant
    leur échelle de CELL_M m. Toit et semelle rabattus sur le texel béton (0,0) —
    fenêtres sur un toit, jamais."""
    hx, hz = sx / 2.0, sz / 2.0
    cols_x, cols_z, _ = grid_of(sx, sy, sz)
    span_u = cols_max * CELL_M   # largeur en mètres de la grille complète
    span_v = floors * CELL_M     # hauteur en mètres de la grille complète
    # (normale, coins (x,y,z) des 4 sommets CCW vus de l'extérieur, axes u/v pour les UV)
    faces = [
        ((0, 0, 1),  [(-hx, 0, hz), (hx, 0, hz), (hx, sy, hz), (-hx, sy, hz)],
         lambda p: ((p[0] + hx) / span_u * (cols_x * CELL_M / sx), p[1] / span_v)),
        ((0, 0, -1), [(hx, 0, -hz), (-hx, 0, -hz), (-hx, sy, -hz), (hx, sy, -hz)],
         lambda p: ((p[0] + hx) / span_u * (cols_x * CELL_M / sx), p[1] / span_v)),
        ((1, 0, 0),  [(hx, 0, hz), (hx, 0, -hz), (hx, sy, -hz), (hx, sy, hz)],
         lambda p: ((p[2] + hz) / span_u * (cols_z * CELL_M / sz), p[1] / span_v)),
        ((-1, 0, 0), [(-hx, 0, -hz), (-hx, 0, hz), (-hx, sy, hz), (-hx, sy, -hz)],
         lambda p: ((p[2] + hz) / span_u * (cols_z * CELL_M / sz), p[1] / span_v)),
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


def write_glb(path, part, images, variant, tex_w, tex_h):
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
          f"2 textures {tex_w}x{tex_h} ({glb_len} o)")


def build(variant, out):
    sx, sy, sz = VARIANTS[variant]
    cols_x, cols_z, floors = grid_of(sx, sy, sz)
    cols_max = max(cols_x, cols_z)
    tex_w, tex_h = tex_size(cols_max, floors)
    part = Part()
    build_tower(part, sx, sy, sz, cols_max, floors)
    # Graine par variante : deux gabarits n'ont pas la même carte de fenêtres allumées.
    seed = {"tower": 20260728, "block": 20260729, "slab": 20260730}[variant]
    base_png, emis_png = make_facades(seed, cols_max, floors, tex_w, tex_h,
                                      FACADES[variant])
    write_glb(out, part, (base_png, emis_png), variant, tex_w, tex_h)


if __name__ == "__main__":
    if len(sys.argv) == 1:
        # Sans argument : régénère les trois variantes utilisées par le moteur.
        import os
        models = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "assets", "models")
        for variant, name in (("tower", "building_a.glb"),
                              ("block", "building_b.glb"),
                              ("slab", "building_c.glb")):
            build(variant, os.path.join(models, name))
        sys.exit(0)
    out = sys.argv[1]
    variant = sys.argv[2] if len(sys.argv) > 2 else "tower"
    if variant not in VARIANTS:
        sys.exit(f"variante inconnue : {variant} (attendu : {', '.join(VARIANTS)})")
    build(variant, out)
