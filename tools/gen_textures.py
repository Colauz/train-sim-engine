#!/usr/bin/env python3
"""Textures PBR procédurales, tuilables — M53.

POURQUOI CE SCRIPT. Jusqu'ici, tout le monde sauf le ballast était peint en
COULEUR UNIE : béton du viaduc, quais, sol de la ville, carrosserie, intérieur
de la rame, pupitre. Une couleur unie ne donne AUCUNE échelle à l'œil — un mur
de 20 m et une console de 60 cm rendus dans le même gris plat se ressemblent, et
la scène entière a l'air d'une maquette en pâte à modeler. La texture, ici,
n'est pas de la décoration : c'est ce qui dit à l'œil la TAILLE des choses.

Chaque jeu produit les trois cartes de la convention glTF metallic-roughness que
le moteur consomme déjà (cf. le ballast Poly Haven, engine/app/application.cpp) :

    <nom>_diff.png  base color, espace sRGB
    <nom>_arm.png   AO (R) / roughness (G) / metallic (B), DONNÉES linéaires
    <nom>_nor.png   normal map en espace tangent, convention OpenGL (+Y = haut)

TUILABILITÉ. Tout le bruit est un bruit de valeur sur RÉSEAU PÉRIODIQUE : la
maille boucle en x et en y, donc chaque octave boucle, donc la somme boucle. Il
n'y a aucune couture à masquer — c'est vérifiable en juxtaposant deux copies.

ÉCHELLE. `period` est le côté, EN MÈTRES, du carré que couvre une tuile. Il
n'est pas décoratif : la géométrie du moteur porte des UV en mètres divisés par
son propre `uv_period` (4 m pour le viaduc et les quais, cf. viaduct.cpp). Une
texture déclarée pour 4 m et posée sur des UV calés sur 4 m rend des joints de
dalle à leur vraie taille. Les deux nombres doivent rester d'accord.

stdlib seule (zlib pour le PNG), comme tous les outils du dossier."""

import math
import os
import struct
import sys
import zlib

# --- Résolutions -----------------------------------------------------------------
# 512 pour les surfaces du monde (vues de près ET de loin, à 4 m la tuile => 128
# texels/m), 256 pour la rame (tuile de 1 m => 256 texels/m, largement assez).
WORLD_SIZE = 512
TRAIN_SIZE = 256


# ==================================================================================
# BRUIT PÉRIODIQUE
# ==================================================================================
def _rng(seed):
    """Générateur congruentiel minimal : reproductible, sans dépendance, et
    surtout INDÉPENDANT de la version de Python (random.random() ne garantit pas
    la même suite d'une version à l'autre ; les textures doivent, elles, se
    régénérer à l'identique)."""
    state = (seed * 747796405 + 2891336453) & 0xFFFFFFFF

    def next_float():
        nonlocal state
        state = (state * 747796405 + 2891336453) & 0xFFFFFFFF
        x = ((state >> ((state >> 28) + 4)) ^ state) * 277803737 & 0xFFFFFFFF
        return ((x >> 22) ^ x) / 4294967296.0

    return next_float


def lattice(cells, seed):
    """Réseau de valeurs aléatoires cells x cells, refermé sur lui-même."""
    rnd = _rng(seed)
    return [[rnd() for _ in range(cells)] for _ in range(cells)]


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def noise_layer(size, cells, seed):
    """Bruit de valeur périodique, interpolé en smoothstep, rendu à `size`.

    Séparé en deux passes (lignes puis colonnes) : c'est O(size * cells) au lieu
    de O(size²) évaluations de réseau, ce qui rend le script tenable en Python
    pur — la version naïve mettait plusieurs minutes par carte."""
    grid = lattice(cells, seed)
    step = cells / size
    # Pré-calcul des indices et poids d'interpolation (identiques en x et en y).
    idx0, idx1, frac = [], [], []
    for i in range(size):
        p = i * step
        i0 = int(math.floor(p)) % cells
        idx0.append(i0)
        idx1.append((i0 + 1) % cells)
        frac.append(_smooth(p - math.floor(p)))

    # Passe horizontale : cells lignes de `size` échantillons.
    rows = []
    for gy in range(cells):
        row = grid[gy]
        rows.append([row[idx0[i]] + (row[idx1[i]] - row[idx0[i]]) * frac[i]
                     for i in range(size)])
    # Passe verticale.
    out = []
    for j in range(size):
        r0, r1, f = rows[idx0[j]], rows[idx1[j]], frac[j]
        out.extend([r0[i] + (r1[i] - r0[i]) * f for i in range(size)])
    return out


def fbm(size, base_cells, octaves, seed, gain=0.5):
    """Somme d'octaves de bruit périodique, normalisée dans [0, 1]."""
    out = [0.0] * (size * size)
    amp, total, cells = 1.0, 0.0, base_cells
    for o in range(octaves):
        layer = noise_layer(size, cells, seed + o * 7919)
        for i in range(size * size):
            out[i] += layer[i] * amp
        total += amp
        amp *= gain
        cells *= 2
        if cells > size:  # au-delà, une cellule fait moins d'un texel : c'est du bruit blanc
            break
    inv = 1.0 / total
    return [v * inv for v in out]


def white(size, seed):
    """Bruit blanc par texel — le grain fin (grès, mouchetis, grain plastique)."""
    rnd = _rng(seed)
    return [rnd() for _ in range(size * size)]


# ==================================================================================
# OUTILS D'IMAGE
# ==================================================================================
def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def lerp(a, b, t):
    return a + (b - a) * t


def write_png(path, size, rgb_rows):
    """PNG 8 bits RGB non entrelacé. `rgb_rows` = bytearray de size*size*3."""
    raw = bytearray()
    stride = size * 3
    for y in range(size):
        raw.append(0)  # filtre 0 (None) : les textures procédurales compressent déjà bien
        raw += rgb_rows[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)  # 8 bits, truecolor
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


def pack_rgb(size, channels):
    """(r, g, b) en flottants [0,1] -> bytearray RGB8."""
    r, g, b = channels
    out = bytearray(size * size * 3)
    for i in range(size * size):
        out[3 * i] = int(clamp(r[i], 0.0, 1.0) * 255.0 + 0.5)
        out[3 * i + 1] = int(clamp(g[i], 0.0, 1.0) * 255.0 + 0.5)
        out[3 * i + 2] = int(clamp(b[i], 0.0, 1.0) * 255.0 + 0.5)
    return out


def srgb_encode(v):
    """Linéaire -> sRGB. Les base colors sont ÉCHANTILLONNÉES en sRGB par le
    matériel (TextureFormat::SrgbColor) : les écrire en linéaire les rendrait
    deux fois trop sombres une fois décodées."""
    v = clamp(v, 0.0, 1.0)
    return 12.92 * v if v <= 0.0031308 else 1.055 * (v ** (1.0 / 2.4)) - 0.055


def pack_albedo(size, rgb_linear):
    """Albédo LINÉAIRE (celui qu'on raisonne en PBR) -> octets sRGB."""
    r, g, b = rgb_linear
    out = bytearray(size * size * 3)
    for i in range(size * size):
        out[3 * i] = int(srgb_encode(r[i]) * 255.0 + 0.5)
        out[3 * i + 1] = int(srgb_encode(g[i]) * 255.0 + 0.5)
        out[3 * i + 2] = int(srgb_encode(b[i]) * 255.0 + 0.5)
    return out


def normal_from_height(size, height, strength):
    """Normal map en espace tangent, dérivée d'un champ de hauteur PÉRIODIQUE
    (différences centrées avec indices modulo : la carte reste tuilable).

    Convention OpenGL : G = +Y vers le HAUT de la texture. C'est celle des
    cartes `_nor_gl` de Poly Haven que le moteur consomme déjà — mélanger les
    deux conventions retourne l'éclairage en relief sur un axe."""
    out = bytearray(size * size * 3)
    for y in range(size):
        ym, yp = ((y - 1) % size) * size, ((y + 1) % size) * size
        row = y * size
        for x in range(size):
            xm, xp = (x - 1) % size, (x + 1) % size
            dx = (height[row + xp] - height[row + xm]) * strength
            dy = (height[yp + x] - height[ym + x]) * strength
            # n = normalize(-dx, -dy, 1) ; le -dy tient à ce que v croît vers le bas
            # du buffer alors que la convention GL veut +Y vers le haut.
            inv = 1.0 / math.sqrt(dx * dx + dy * dy + 1.0)
            i = 3 * (row + x)
            out[i] = int((-dx * inv * 0.5 + 0.5) * 255.0 + 0.5)
            out[i + 1] = int((dy * inv * 0.5 + 0.5) * 255.0 + 0.5)
            out[i + 2] = int((inv * 0.5 + 0.5) * 255.0 + 0.5)
    return out


def write_set(outdir, name, size, albedo, ao, rough, metal, height, normal_strength):
    """Écrit les trois cartes d'un jeu PBR complet."""
    os.makedirs(outdir, exist_ok=True)
    total = 0
    total += write_png(os.path.join(outdir, name + "_diff.png"), size, pack_albedo(size, albedo))
    total += write_png(os.path.join(outdir, name + "_arm.png"), size,
                       pack_rgb(size, (ao, rough, metal)))
    total += write_png(os.path.join(outdir, name + "_nor.png"), size,
                       normal_from_height(size, height, normal_strength))
    print(f"  {name:<10} {size}x{size}  {total / 1024.0:7.1f} Kio")


def grid_mask(size, cells, joint_texels, jitter=None):
    """Masque de joints d'un carrelage : 1 DANS le joint, 0 sur la dalle.
    Renvoie aussi l'indice de dalle (pour varier la teinte dalle par dalle)."""
    mask = [0.0] * (size * size)
    tile_id = [0] * (size * size)
    pitch = size / cells
    for y in range(size):
        ty = int(y / pitch)
        dy = min(y % pitch, pitch - (y % pitch))
        for x in range(size):
            tx = int(x / pitch)
            dx = min(x % pitch, pitch - (x % pitch))
            d = min(dx, dy)
            i = y * size + x
            # Bord adouci sur 1 texel : un joint parfaitement net crénelle en mip.
            mask[i] = clamp(1.0 - (d - joint_texels * 0.5), 0.0, 1.0)
            tile_id[i] = ty * cells + tx
    return mask, tile_id


# ==================================================================================
# JEUX DE TEXTURES — LE MONDE (tuile de 4 m, = uv_period de viaduct.cpp)
# ==================================================================================
def make_concrete(outdir):
    """Béton coffré : viaduc, piles, colonnes et bâti de gare.

    Ce qui fait « béton » et pas « gris » : les lignes de COFFRAGE (banches de
    1 m, donc 4 par tuile), les coulures verticales, et un piquetage fin. Sans
    les banches, une pile de 20 m n'a aucune échelle."""
    size = WORLD_SIZE
    n_big = fbm(size, 4, 4, 1001)          # marbrures de masse
    n_stain = fbm(size, 2, 3, 1002)        # coulures / salissures
    n_fine = fbm(size, 32, 3, 1003)        # piquetage
    grain = white(size, 1004)

    albedo_r, albedo_g, albedo_b = [], [], []
    ao, rough, metal, height = [], [], [], []
    pitch = size / 4.0  # 4 banches par tuile = 1 m
    for y in range(size):
        # Ligne de coffrage horizontale : un léger creux + une trace plus sombre.
        dline = min(y % pitch, pitch - (y % pitch))
        seam = clamp(1.0 - dline / 3.0, 0.0, 1.0)
        for x in range(size):
            i = y * size + x
            # Base à 0,34 en LINÉAIRE : c'est l'albédo réel d'un béton de génie
            # civil (~0,35). Le 0,50 de la première passe donnait un gris de
            # rendu, très au-dessus de tout béton existant — la pile de viaduc
            # ressortait plus claire que le ciel bas.
            v = 0.34 + (n_big[i] - 0.5) * 0.17 + (n_fine[i] - 0.5) * 0.07
            # Coulure : assombrit par plages étirées (n_stain est à très basse
            # fréquence, il traîne donc sur plusieurs mètres).
            stain = clamp((n_stain[i] - 0.42) * 2.2, 0.0, 1.0)
            v -= stain * 0.13
            v -= seam * 0.11
            v += (grain[i] - 0.5) * 0.03
            # Béton légèrement froid, très peu saturé.
            albedo_r.append(v * 1.00)
            albedo_g.append(v * 1.005)
            albedo_b.append(v * 1.02)
            ao.append(1.0 - seam * 0.25 - stain * 0.10)
            rough.append(clamp(0.86 + (n_fine[i] - 0.5) * 0.12 + stain * 0.05, 0.0, 1.0))
            metal.append(0.0)
            height.append(n_fine[i] * 0.35 + (grain[i] - 0.5) * 0.15 - seam * 0.9)
    write_set(outdir, "concrete", size, (albedo_r, albedo_g, albedo_b), ao, rough, metal,
              height, 2.2)


def make_platform(outdir):
    """Dalle de quai japonaise : carrelage clair 50 x 50 cm, joints creusés.

    Le carrelage est LE repère d'échelle du quai. C'est aussi lui qui donne au
    conducteur, en approche, une lecture immédiate de sa vitesse résiduelle :
    des dalles qui défilent une à une, ça se compte."""
    size = WORLD_SIZE
    joints, tile_id = grid_mask(size, 8, 3.0)   # 8 dalles / 4 m = 50 cm
    n_fine = fbm(size, 48, 3, 2001)
    n_dirt = fbm(size, 3, 4, 2002)
    grain = white(size, 2003)
    rnd = _rng(2004)
    tint = [0.94 + rnd() * 0.12 for _ in range(64)]  # variation dalle par dalle

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for i in range(size * size):
        j = joints[i]
        base = 0.60 * tint[tile_id[i] % 64]
        base += (n_fine[i] - 0.5) * 0.04 + (grain[i] - 0.5) * 0.025
        dirt = clamp((n_dirt[i] - 0.5) * 1.4, 0.0, 1.0)
        base -= dirt * 0.07
        v = lerp(base, 0.26, j)  # le joint est nettement plus sombre
        a_r.append(v * 1.02)
        a_g.append(v * 1.00)
        a_b.append(v * 0.97)   # dalle légèrement chaude, japonaise
        ao.append(1.0 - j * 0.45)
        # Dalle lissée (elle brille sous les néons), joint mat.
        rough.append(clamp(lerp(0.45 + dirt * 0.25, 0.92, j), 0.0, 1.0))
        metal.append(0.0)
        height.append(-j * 1.0 + n_fine[i] * 0.10)
    write_set(outdir, "platform", size, (a_r, a_g, a_b), ao, rough, metal, height, 3.0)


def make_asphalt(outdir):
    """Enrobé de la ville : sombre, granuleux, quelques réparations plus claires."""
    size = WORLD_SIZE
    n_patch = fbm(size, 3, 3, 3001)
    n_mid = fbm(size, 16, 3, 3002)
    aggregate = white(size, 3003)
    n_crack = fbm(size, 6, 4, 3004)

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for i in range(size * size):
        # Granulat : quelques % de texels nettement plus clairs = du gravier qui accroche
        # la lumière. C'est ce détail qui empêche l'enrobé de virer au feutre noir.
        agg = 1.0 if aggregate[i] > 0.86 else 0.0
        v = 0.055 + (n_mid[i] - 0.5) * 0.02 + (n_patch[i] - 0.5) * 0.025
        v += agg * (0.05 + aggregate[i] * 0.06)
        # Fissures : le bruit ridé (|n - 0.5| proche de 0) trace des lignes continues.
        crack = clamp(1.0 - abs(n_crack[i] - 0.5) * 26.0, 0.0, 1.0)
        v -= crack * 0.025
        a_r.append(v * 1.02)
        a_g.append(v)
        a_b.append(v * 1.03)
        ao.append(1.0 - crack * 0.35)
        rough.append(clamp(0.93 - agg * 0.20 + (n_mid[i] - 0.5) * 0.06, 0.0, 1.0))
        metal.append(0.0)
        height.append(aggregate[i] * 0.45 + n_mid[i] * 0.25 - crack * 1.2)
    write_set(outdir, "asphalt", size, (a_r, a_g, a_b), ao, rough, metal, height, 2.0)


def make_tactile(outdir):
    """Bande podotactile (点字ブロック) : jaune vif à plots ronds, tuile de 1 m.

    Elle borde le nez de quai. Ce n'est pas de la décoration : c'est le seul
    élément qui donne au bord du quai une LIGNE lisible depuis la cabine, et
    c'est ce que cherche l'œil quand on aligne une rame."""
    size = TRAIN_SIZE
    dots = 5                       # 5 plots par mètre = 20 cm de pas
    pitch = size / dots
    radius = pitch * 0.30
    grain = white(size, 4001)
    n_wear = fbm(size, 8, 3, 4002)

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for y in range(size):
        for x in range(size):
            i = y * size + x
            cx = (x % pitch) - pitch * 0.5
            cy = (y % pitch) - pitch * 0.5
            d = math.sqrt(cx * cx + cy * cy)
            # Plot à bord adouci sur 1,5 texel (sinon crénelage en mip).
            dot = clamp((radius - d) / 1.5, 0.0, 1.0)
            wear = clamp((n_wear[i] - 0.4) * 1.2, 0.0, 1.0)  # jaune usé par les pas
            v = 0.72 - wear * 0.18 + (grain[i] - 0.5) * 0.03
            a_r.append(v)
            a_g.append(v * 0.62)
            a_b.append(v * 0.07)
            ao.append(1.0 - (1.0 - dot) * 0.15)
            rough.append(clamp(0.62 + wear * 0.20 - dot * 0.10, 0.0, 1.0))
            metal.append(0.0)
            height.append(dot * 1.0 + (grain[i] - 0.5) * 0.05)
    write_set(outdir, "tactile", size, (a_r, a_g, a_b), ao, rough, metal, height, 2.5)


# NOTE — pas de jeu « façade » ici : les immeubles de Neo-Tokyo portent DÉJÀ leurs
# propres textures, engendrées et EMBARQUÉES par tools/gen_building.py (base color
# + émissive des fenêtres allumées, une grille par façade entière). En produire une
# seconde version ici donnerait deux sources de vérité pour la même surface.


# ==================================================================================
# JEUX DE TEXTURES — LA RAME (tuile de 1 m)
# ==================================================================================
def make_steel(outdir):
    """Inox brossé de la carrosserie E235.

    Le brossage est une variation de RUGOSITÉ, pas de couleur : c'est pour ça
    qu'un aplat métallique gris ne ressemble jamais à de l'inox. Les stries
    courent le long de u (donc le long de la caisse, sens du laminage)."""
    size = TRAIN_SIZE
    streak = fbm(size, 96, 2, 6001)     # haute fréquence en y => stries fines
    n_broad = fbm(size, 6, 3, 6002)
    grain = white(size, 6003)

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for y in range(size):
        # ÉTIREMENT ANISOTROPE : le bruit est échantillonné à x/8, donc une strie
        # garde sa valeur sur 8 texels de long tout en changeant à chaque texel en
        # travers. C'est exactement ce qu'est un brossage — un bruit très allongé
        # dans une direction. Un bruit isotrope donnerait du crépi, pas de l'inox.
        for x in range(size):
            i = y * size + x
            s = streak[y * size + (x >> 3)]
            v = 0.62 + (s - 0.5) * 0.05 + (n_broad[i] - 0.5) * 0.02
            a_r.append(v)
            a_g.append(v * 1.005)
            a_b.append(v * 1.02)   # inox très légèrement froid
            ao.append(1.0)
            rough.append(clamp(0.30 + (s - 0.5) * 0.22 + (grain[i] - 0.5) * 0.04, 0.05, 1.0))
            metal.append(1.0)
            height.append((s - 0.5) * 0.30 + (grain[i] - 0.5) * 0.05)
    write_set(outdir, "steel", size, (a_r, a_g, a_b), ao, rough, metal, height, 1.2)


def make_panel(outdir):
    """Mélamine des habillages intérieurs : blanc cassé, grain très fin."""
    size = TRAIN_SIZE
    n_fine = fbm(size, 40, 3, 7001)
    grain = white(size, 7002)
    joints, _ = grid_mask(size, 2, 2.0)   # panneaux de 50 cm

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for i in range(size * size):
        j = joints[i]
        v = 0.70 + (n_fine[i] - 0.5) * 0.03 + (grain[i] - 0.5) * 0.012
        v = lerp(v, 0.52, j * 0.8)
        a_r.append(v)
        a_g.append(v * 0.995)
        a_b.append(v * 0.96)   # blanc cassé chaud
        ao.append(1.0 - j * 0.25)
        rough.append(clamp(0.55 + (n_fine[i] - 0.5) * 0.10 + j * 0.15, 0.0, 1.0))
        metal.append(0.0)
        height.append(-j * 0.8 + (grain[i] - 0.5) * 0.08)
    write_set(outdir, "panel", size, (a_r, a_g, a_b), ao, rough, metal, height, 1.5)


def make_floor(outdir):
    """Sol de rame : revêtement vinyle moucheté, antidérapant."""
    size = TRAIN_SIZE
    speck = white(size, 8001)
    speck2 = white(size, 8002)
    n_wear = fbm(size, 5, 3, 8003)

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for i in range(size * size):
        base = 0.115 + (n_wear[i] - 0.5) * 0.03
        # Mouchetis : deux populations, claire et sombre, quelques % chacune.
        if speck[i] > 0.90:
            base += 0.16 * (speck[i] - 0.90) * 10.0
        elif speck2[i] > 0.94:
            base -= 0.045
        a_r.append(base * 1.00)
        a_g.append(base * 1.03)
        a_b.append(base * 1.06)   # gris-bleu métro
        ao.append(1.0)
        rough.append(clamp(0.68 + (speck[i] - 0.5) * 0.10, 0.0, 1.0))
        metal.append(0.0)
        height.append(speck[i] * 0.25)
    write_set(outdir, "floor", size, (a_r, a_g, a_b), ao, rough, metal, height, 0.9)


def make_fabric(outdir):
    """Moquette des banquettes : bleu profond, armure tissée visible."""
    size = TRAIN_SIZE
    n_fine = fbm(size, 30, 3, 9001)
    grain = white(size, 9002)
    weave_pitch = size / 32.0   # fil de ~3 cm

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for y in range(size):
        for x in range(size):
            i = y * size + x
            # Armure toile : une trame sur deux passe au-dessus de la chaîne.
            u = (x % weave_pitch) / weave_pitch
            v = (y % weave_pitch) / weave_pitch
            over = ((int(x / weave_pitch) + int(y / weave_pitch)) % 2) == 0
            bump = math.sin(math.pi * (u if over else v))
            shade = 0.85 + bump * 0.30
            base = (0.055 + (n_fine[i] - 0.5) * 0.012) * shade
            a_r.append(base * 0.55)
            a_g.append(base * 0.80)
            a_b.append(base * 2.35)   # bleu marine JR
            ao.append(clamp(0.72 + bump * 0.28, 0.0, 1.0))
            rough.append(clamp(0.93 + (grain[i] - 0.5) * 0.06, 0.0, 1.0))
            metal.append(0.0)
            height.append(bump * 0.8 + (grain[i] - 0.5) * 0.12)
    write_set(outdir, "fabric", size, (a_r, a_g, a_b), ao, rough, metal, height, 1.8)


def make_console(outdir):
    """Pupitre de conduite : plastique technique grainé, gris très sombre mat.

    Un pupitre en aplat noir avale toute la lumière et devient une silhouette :
    le grain est ce qui lui rend son volume sous la lumière rasante de la cabine."""
    size = TRAIN_SIZE
    grain = white(size, 10001)
    n_fine = fbm(size, 64, 2, 10002)
    n_broad = fbm(size, 8, 3, 10003)

    a_r, a_g, a_b, ao, rough, metal, height = [], [], [], [], [], [], []
    for i in range(size * size):
        v = 0.045 + (n_broad[i] - 0.5) * 0.012 + (n_fine[i] - 0.5) * 0.010
        v += (grain[i] - 0.5) * 0.008
        a_r.append(v)
        a_g.append(v * 1.02)
        a_b.append(v * 1.05)
        ao.append(1.0 - n_fine[i] * 0.10)
        rough.append(clamp(0.62 + (grain[i] - 0.5) * 0.12 + (n_fine[i] - 0.5) * 0.10, 0.0, 1.0))
        metal.append(0.0)
        # Grain fin ET serré : c'est du plastique injecté, pas de la pierre.
        height.append(grain[i] * 0.55 + n_fine[i] * 0.45)
    write_set(outdir, "console", size, (a_r, a_g, a_b), ao, rough, metal, height, 1.6)


# ==================================================================================
WORLD_SETS = (make_concrete, make_platform, make_asphalt, make_tactile)
TRAIN_SETS = (make_steel, make_panel, make_floor, make_fabric, make_console)

if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else "assets/textures"
    print(f"Textures procédurales -> {root}")
    print("Monde (tuile de 4 m) :")
    for fn in WORLD_SETS:
        fn(os.path.join(root, "world"))
    print("Rame (tuile de 1 m) :")
    for fn in TRAIN_SETS:
        fn(os.path.join(root, "train"))
