#!/usr/bin/env python3
"""Un MODULE de station urbaine couverte, au format .glb (M31 — ex-M18 TGV).

La station est bâtie en RÉPÉTANT ce module le long de la voie (l'app le pose tous les
40 m sur la spline, orienté comme la voie : il épouse donc les courbes sans se déformer).
Un module contient, dans son repère local (x = latéral, y = hauteur au-dessus du plan de
roulement, z = le long de la voie) :
  * deux QUAIS béton encadrant la voie, dont le dessus est à +1,00 m du rail (= le seuil
    des portes, plancher à 1,05 m) ;
  * des COLONNES d'acier aux deux extrémités des quais (hors gabarit) ;
  * un GRAND TOIT PLEIN acier/verre enjambant TOUTE la largeur (les deux quais ET les
    voies) — c'est une gare de métro aérien sur viaduc, plus deux verrières de quai ;
  * des BANDEAUX ÉMISSIFS sous les rives du toit (néon cyan Neo-Tokyo), qui s'allument
    la nuit (facteur émissif HDR, modulé par le facteur nuit du moteur).

Repère : y = 0 est le PLAN DE ROULEMENT. Aucune dépendance externe (stdlib seule)."""
import struct, json, os, sys

MODULE_LEN = 40.0            # longueur d'un module ; l'app en pose 8 par gare
HZ = MODULE_LEN / 2.0

PLAT_INNER = 1.75           # bord quai côté voie (la caisse fait 1,45 de demi-largeur)
PLAT_OUTER = 5.20           # bord extérieur
PLAT_TOP = 1.00             # dessus du quai = plan de roulement + 1 m (seuil des portes)
PLAT_BOTTOM = -1.60         # base enterrée (dans le tablier du viaduc)
PILLAR_HALF = 0.18
PILLAR_X = 6.10             # colonnes hors des quais, au droit des rives du toit
ROOF_Y = 7.40               # intrados du grand toit (au-dessus de la caténaire, 6,60 m)
ROOF_THICK = 0.20
ROOF_HALF = 6.95            # demi-largeur du toit : couvre quais + voies
STRIP_Y = 7.12              # bandeaux néon sous les rives du toit
PILLAR_Z = (-10.0, 10.0)    # 2 colonnes par côté et par module => une tous les 20 m

MATERIALS = [
    {"name": "beton", "factor": [0.62, 0.62, 0.60, 1.0], "metallic": 0.0, "roughness": 0.90},
    {"name": "acier", "factor": [0.48, 0.50, 0.53, 1.0], "metallic": 0.7, "roughness": 0.45},
    {"name": "verriere", "factor": [0.60, 0.68, 0.78, 1.0], "metallic": 0.0, "roughness": 0.08},
    # Bandeau néon : albédo sombre, émissif CYAN HDR (> 1, hors spec stricte — assumé,
    # c'est ce qui flambe la nuit). Le moteur module par le facteur nuit.
    {"name": "neon", "factor": [0.05, 0.10, 0.12, 1.0], "metallic": 0.0, "roughness": 0.4,
     "emissive": [0.15, 2.2, 2.6]},
]
MAT_CONCRETE, MAT_STEEL, MAT_GLASS, MAT_NEON = 0, 1, 2, 3


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
    materials = []
    for m, _ in used:
        mat = {"name": MATERIALS[m]["name"],
               "pbrMetallicRoughness": {"baseColorFactor": MATERIALS[m]["factor"],
                                        "metallicFactor": MATERIALS[m]["metallic"],
                                        "roughnessFactor": MATERIALS[m]["roughness"]}}
        if "emissive" in MATERIALS[m]:
            mat["emissiveFactor"] = MATERIALS[m]["emissive"]
        materials.append(mat)
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
    parts = [Part(), Part(), Part(), Part()]
    # Deux quais, de part et d'autre de la voie. Ils débordent LÉGÈREMENT en z (± un chouïa)
    # pour que deux modules voisins se recouvrent au raccord et ne laissent aucun joint.
    z0, z1 = -HZ - 0.05, HZ + 0.05
    build_box(parts[MAT_CONCRETE], -PLAT_OUTER, PLAT_BOTTOM, z0, -PLAT_INNER, PLAT_TOP, z1)  # gauche
    build_box(parts[MAT_CONCRETE], PLAT_INNER, PLAT_BOTTOM, z0, PLAT_OUTER, PLAT_TOP, z1)    # droite

    # Colonnes aux deux extrémités (hors quai) + GRAND TOIT PLEIN sur toute la largeur.
    for sign in (-1.0, 1.0):
        cx = sign * PILLAR_X
        for pz in PILLAR_Z:
            build_box(parts[MAT_STEEL], cx - PILLAR_HALF, PLAT_BOTTOM, pz - PILLAR_HALF,
                      cx + PILLAR_HALF, ROOF_Y, pz + PILLAR_HALF)
    # Toit acier + lanterneau vitré central au-dessus des voies (il allège le toit le
    # jour). M56 — LE LANTERNEAU TRAVERSAIT LE TOIT. La dalle d'acier courait d'une rive
    # à l'autre et la verrière était une seconde boîte PLANTÉE DEDANS, 2 cm plus épaisse :
    # leurs faces de bout tombaient exactement dans le même plan, avec la même
    # orientation, et le raccord de chaque module scintillait sur 3,2 m de large
    # (check_coplanar.py le signalait). Un lanterneau ne se plante pas dans une toiture :
    # il en REMPLACE une partie. La dalle est donc coupée en deux versants, et le vitrage
    # comble l'ouverture, à fleur. Les flancs sont coplanaires — mais dos à dos (normales
    # opposées), donc jamais rasterisés ensemble.
    canopy_half = 1.6
    build_box(parts[MAT_STEEL], -ROOF_HALF, ROOF_Y, z0, -canopy_half, ROOF_Y + ROOF_THICK, z1)
    build_box(parts[MAT_STEEL], canopy_half, ROOF_Y, z0, ROOF_HALF, ROOF_Y + ROOF_THICK, z1)
    build_box(parts[MAT_GLASS], -canopy_half, ROOF_Y, z0, canopy_half, ROOF_Y + ROOF_THICK, z1)
    # Bandeaux néon sous les rives, côté ville : la signature nocturne de la station.
    for sign in (-1.0, 1.0):
        x0, x1 = sign * (ROOF_HALF - 0.30), sign * ROOF_HALF
        lo, hi = min(x0, x1), max(x0, x1)
        build_box(parts[MAT_NEON], lo, STRIP_Y, z0, hi, STRIP_Y + 0.22, z1)

    write_glb(out, parts, "station_module")


def _default_models_dir():
    """M56 — Par défaut, on écrit DANS assets/models, pas dans le répertoire courant.

    Le README documente `python3 tools/gen_metro.py` comme la façon de régénérer les
    modèles ; avec l'ancien défaut (« . ») la commande déposait les .glb à la racine du
    dépôt et le jeu continuait de charger les anciens. Un générateur dont la sortie par
    défaut n'est pas l'endroit où le moteur va lire est un piège, pas une commodité."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "models")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_default_models_dir(),
                                                             "station.glb")
    build_station(out)
