#!/usr/bin/env python3
"""Génère un arbre en « crossed quads » (foliage cards) au format .glb, texture embarquée.

TECHNIQUE : le feuillage n'est PAS de la géométrie. Ce sont 4 plans qui se croisent en
étoile autour du tronc, chacun portant une texture de branche feuillue DÉTOURÉE par son
canal alpha. Vu de n'importe quel azimut, on en voit toujours au moins deux de biais :
le cerveau reconstruit un volume. C'est ce qui permet un arbre crédible en ~200 triangles
là où une vraie ramure en demanderait des dizaines de milliers.

Le repère est celui du monde : y = haut, origine au PIED du tronc (le semis pose donc
l'arbre directement sur Terrain::height, sans offset).
Aucune dépendance externe (struct/json/zlib/math de la stdlib)."""
import struct, json, zlib, math, sys, random

# --- Cotes d'un arbre de haie champenoise -------------------------------------
HEIGHT = 7.2          # hauteur totale
TRUNK_R = 0.17        # rayon du tronc au pied
TRUNK_TOP = 2.6       # hauteur où le feuillage commence
TRUNK_SEGMENTS = 7    # section du tronc : 7 côtés suffisent à cette échelle
CARDS = 4             # plans de feuillage entrecroisés
CARD_W = 5.4          # largeur d'un plan
CARD_H = 5.2          # hauteur d'un plan
TEX = 256             # côté de la texture de feuillage

MATERIALS = [
    # Écorce : diélectrique très mat, brun sombre.
    {"name": "ecorce", "factor": [0.09, 0.065, 0.045, 1.0], "metallic": 0.0, "roughness": 0.9,
     "textured": False, "mask": False},
    # Feuillage : la texture porte la COULEUR et surtout l'ALPHA (le détourage). Le facteur
    # reste blanc pour ne pas la teinter. alphaMode MASK => le moteur fait un discard.
    {"name": "feuillage", "factor": [1.0, 1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 0.78,
     "textured": True, "mask": True},
]
MAT_BARK, MAT_LEAF = 0, 1


# --- Texture de feuillage : couleur + ALPHA procéduraux ------------------------
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


def make_foliage(size):
    """Une branche feuillue : des dizaines de folioles elliptiques semées le long de
    rameaux qui divergent depuis le bas-centre (le point d'attache au tronc).

    L'ALPHA est ce qui compte : c'est lui qui découpe la silhouette. Il vaut 0 partout
    sauf dans les folioles — le reste du quad disparaîtra au discard. Un alpha binaire
    (pas de dégradé) est VOULU : notre pipeline n'a pas de blending trié, seulement un
    seuil, donc un bord progressif ne ferait que du bruit."""
    rnd = random.Random(20260716)  # semis figé : le modèle doit être reproductible
    px = bytearray(size * size * 4)  # tout à 0 => alpha 0 => transparent

    def leaf(cx, cy, rx, ry, ang, shade):
        ca, sa = math.cos(ang), math.sin(ang)
        x0, x1 = max(0, int(cx - rx - ry)), min(size, int(cx + rx + ry) + 1)
        y0, y1 = max(0, int(cy - rx - ry)), min(size, int(cy + rx + ry) + 1)
        for y in range(y0, y1):
            for x in range(x0, x1):
                dx, dy = x - cx, y - cy
                u = dx * ca + dy * sa
                v = -dx * sa + dy * ca
                if (u / rx) ** 2 + (v / ry) ** 2 <= 1.0:
                    o = (y * size + x) * 4
                    # Teintes de vert : les folioles ne sont pas toutes identiques, sinon
                    # le feuillage se lit comme un aplat.
                    #
                    # ATTENTION AU PIÈGE sRGB : ces octets sont décodés en LINÉAIRE par le
                    # matériau (base color = R8G8B8A8_SRGB), donc élevés à ~2.2. Le premier
                    # jeu (38, 96, 30) donnait un albédo linéaire de (0.017, 0.115, 0.010) :
                    # rouge et bleu quasi NULS, soit une feuille déjà presque noire AVANT
                    # tout éclairage. Une vraie feuille renvoie ~(0.05, 0.24, 0.03).
                    # Choisir une couleur « à l'oeil » dans l'espace sRGB trompe : 96/255
                    # ressemble à un vert moyen, mais ne vaut que 0.115 en linéaire.
                    px[o] = min(255, int(62 * shade))
                    px[o + 1] = min(255, int(139 * shade))
                    px[o + 2] = min(255, int(49 * shade))
                    px[o + 3] = 255  # OPAQUE : c'est ici que la feuille existe

    # Rameaux : ils partent du bas-centre (attache au tronc) et s'ouvrent en éventail.
    for branch in range(9):
        a = math.radians(rnd.uniform(-78, 78))
        length = rnd.uniform(0.55, 0.95) * size
        for i in range(26):
            t = (i + 3) / 29.0
            # Léger arc : une branche droite se lit comme un trait.
            bend = math.sin(t * 2.2) * 0.10 * size
            bx = size * 0.5 + math.sin(a) * length * t + bend * math.cos(a)
            by = size * 0.97 - math.cos(a) * length * t * 0.95
            if not (0 <= bx < size and 0 <= by < size):
                continue
            for _ in range(3):
                lx = bx + rnd.uniform(-0.055, 0.055) * size
                ly = by + rnd.uniform(-0.055, 0.055) * size
                r = rnd.uniform(0.016, 0.030) * size
                leaf(lx, ly, r, r * rnd.uniform(0.42, 0.62), rnd.uniform(0, math.pi),
                     rnd.uniform(0.62, 1.25))
    return png_encode(size, size, bytes(px))


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

    def add_quad(self, verts):
        base = len(self.positions)
        for p, n, uv, tg in verts:
            self.positions.append(p)
            self.normals.append(n)
            self.uvs.append(uv)
            self.tangents.append(tg)
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def build_trunk(part):
    """Tronc : cône tronqué à section polygonale, qui s'affine vers le haut. Normales
    RADIALES et partagées entre les deux triangles d'une face : l'arbre est cylindrique
    à la lumière, pas seulement à la silhouette."""
    top_r = TRUNK_R * 0.55
    ring = [(math.cos(2 * math.pi * i / TRUNK_SEGMENTS), math.sin(2 * math.pi * i / TRUNK_SEGMENTS))
            for i in range(TRUNK_SEGMENTS + 1)]
    top_y = TRUNK_TOP + 1.4  # le tronc monte un peu DANS le feuillage, sinon il flotte
    for i in range(TRUNK_SEGMENTS):
        (c0, s0), (c1, s1) = ring[i], ring[i + 1]
        n0, n1 = (c0, 0.0, s0), (c1, 0.0, s1)
        a = (c0 * TRUNK_R, 0.0, s0 * TRUNK_R)
        b = (c1 * TRUNK_R, 0.0, s1 * TRUNK_R)
        c = (c1 * top_r, top_y, s1 * top_r)
        d = (c0 * top_r, top_y, s0 * top_r)
        tg = (0.0, 1.0, 0.0, 1.0)  # le long du tronc
        u0, u1 = i / TRUNK_SEGMENTS, (i + 1) / TRUNK_SEGMENTS
        part.add_quad([(a, n0, (u0, 0.0), tg), (b, n1, (u1, 0.0), tg),
                       (c, n1, (u1, 1.0), tg), (d, n0, (u0, 1.0), tg)])


def build_cards(part):
    """Les plans de feuillage. Deux règles :
      * ils se croisent à l'axe du tronc et se répartissent en étoile (180/CARDS degrés,
        pas 360 : un quad est visible des DEUX côtés, le doubler serait redondant) ;
      * un plan supplémentaire QUASI HORIZONTAL coiffe l'ensemble — sans lui, vu de haut
        (et notre caméra survole souvent), l'arbre n'est qu'une croix de papier.

    Les normales sont celles du plan, mais REDRESSÉES vers le haut et l'extérieur : une
    normale strictement perpendiculaire au quad rendrait la moitié du feuillage noire
    (elle tournerait le dos au ciel). C'est la triche classique du foliage card, et elle
    est indispensable avec notre IBL, qui lit le ciel via N."""
    cy = TRUNK_TOP + CARD_H * 0.42
    for i in range(CARDS):
        a = math.pi * i / CARDS
        ca, sa = math.cos(a), math.sin(a)
        right = (ca, 0.0, sa)
        hw, hh = CARD_W * 0.5, CARD_H * 0.5
        corners = [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)]
        pts = [(right[0] * u, cy + v, right[2] * u) for u, v in corners]
        uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
        tg = (right[0], right[1], right[2], 1.0)
        # Normale « gonflée » : moitié perpendiculaire au plan, moitié vers le haut.
        perp = (-sa, 0.0, ca)
        n = (perp[0] * 0.35, 0.75, perp[2] * 0.35)
        ln = math.sqrt(sum(c * c for c in n))
        n = tuple(c / ln for c in n)
        part.add_quad([(pts[k], n, uvs[k], tg) for k in range(4)])

    # Le plan coiffant.
    hw = CARD_W * 0.42
    top = HEIGHT - 1.5
    pts = [(-hw, top, -hw), (hw, top - 0.25, -hw), (hw, top, hw), (-hw, top + 0.25, hw)]
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    part.add_quad([(pts[k], (0.0, 1.0, 0.0), uvs[k], (1.0, 0.0, 0.0, 1.0)) for k in range(4)])


parts = [Part(), Part()]
build_trunk(parts[MAT_BARK])
build_cards(parts[MAT_LEAF])
png = make_foliage(TEX)

# --- Sérialisation glTF binaire -----------------------------------------------
def align4(n):
    return (n + 3) & ~3


blocks, part_blocks = [], []
for p in parts:
    b = (b"".join(struct.pack("<fff", *v) for v in p.positions),
         b"".join(struct.pack("<fff", *v) for v in p.normals),
         b"".join(struct.pack("<ff", *v) for v in p.uvs),
         b"".join(struct.pack("<ffff", *v) for v in p.tangents),
         b"".join(struct.pack("<I", i) for i in p.indices))
    part_blocks.append(b)
    blocks.extend(b)
blocks.append(png)

offsets, cur = [], 0
for blk in blocks:
    offsets.append(cur)
    cur = align4(cur + len(blk))
total = cur
bin_data = bytearray(total)
for off, blk in zip(offsets, blocks):
    bin_data[off:off + len(blk)] = blk

accessors, buffer_views, primitives, materials = [], [], [], []
for i, (p, blks) in enumerate(zip(parts, part_blocks)):
    base = 5 * i
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
    for k, target in enumerate([34962, 34962, 34962, 34962, 34963]):
        buffer_views.append({"buffer": 0, "byteOffset": offsets[base + k],
                             "byteLength": len(blks[k]), "target": target})
    primitives.append({
        "attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2,
                       "TANGENT": base + 3},
        "indices": base + 4, "material": i})

png_view = 5 * len(parts)
buffer_views.append({"buffer": 0, "byteOffset": offsets[png_view], "byteLength": len(png)})

for m in MATERIALS:
    pbr = {"baseColorFactor": m["factor"], "metallicFactor": m["metallic"],
           "roughnessFactor": m["roughness"]}
    if m["textured"]:
        pbr["baseColorTexture"] = {"index": 0}
    mat = {"name": m["name"], "pbrMetallicRoughness": pbr}
    if m["mask"]:
        mat["alphaMode"] = "MASK"
        mat["alphaCutoff"] = 0.5
        mat["doubleSided"] = True  # un plan de feuillage se voit des deux côtés
    materials.append(mat)

gltf = {
    "asset": {"version": "2.0", "generator": "noire-tree-gen (CC0)"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "Arbre"}],
    "meshes": [{"primitives": primitives}],
    "materials": materials,
    "textures": [{"sampler": 0, "source": 0}],
    "images": [{"bufferView": png_view, "mimeType": "image/png"}],
    "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}],
    "accessors": accessors,
    "bufferViews": buffer_views,
    "buffers": [{"byteLength": total}],
}

json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))
bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)

out = sys.argv[1] if len(sys.argv) > 1 else "tree.glb"
glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
with open(out, "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, glb_len))
    f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
    f.write(json_bytes)
    f.write(struct.pack("<II", len(bin_pad), 0x004E4942))
    f.write(bin_pad)

nv = sum(len(p.positions) for p in parts)
ni = sum(len(p.indices) for p in parts)
print(f"{out} : arbre {HEIGHT:.1f} m — tronc {TRUNK_SEGMENTS} pans + {CARDS} plans croisés "
      f"+ 1 coiffant")
print(f"  {nv} sommets, {ni // 3} triangles, texture {TEX}x{TEX} RGBA ({len(png)} o), "
      f"total {glb_len} o")
