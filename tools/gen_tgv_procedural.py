#!/usr/bin/env python3
"""Generator procédural TGV V3 AAA — Architecture Modulaire Cabine & Motrice (M29).

Architecture modulaire (panneaux & structures distinctes) :
  * Plancher & chassis caisse ;
  * Murs latéraux gauche/droite & Piliers A de pare-brise ;
  * Toit bombé & visière de cabine ;
  * Capot inférieur / nez plongeant (sous le pupitre) ;
  * VIDE GÉOMÉTRIQUE RÉEL pour le pare-brise (pas de tube/cône fermé) ;
  * Primitive Pare-brise & vitres en verre translucide PBR (alphaMode BLEND, doubleSided) ;
  * Poste de conduite ergonomique AAA (pupitre, écrans KVB/vitesse, leviers Z & S, siège).
"""

import math
import json
import struct
import sys

# --- Dimensions réelles (mètres) -----------------------------------------------
RAIL = -2.20           # Plan de roulement dans le repère caisse
LENGTH = 22.15         # Longueur totale caisse
HALF_W = 1.45          # Demi-largeur (2.90 m total)
ROOF_Y = 1.85          # Hauteur de la crête du toit
FLOOR_Y = 0.00         # Hauteur du plancher de caisse (2.20 m au-dessus du rail)

Z_TAIL = LENGTH / 2.0   # +11.075 m (arrière)
Z_TIP = -LENGTH / 2.0   # -11.075 m (pointe du nez)
Z_CAB_REAR = -4.50      # Cloison arrière de la cabine
Z_PUPITRE = -2.85       # Position du pupitre devant le conducteur
Z_NOSE_BASE = -3.50     # Début du capot inférieur du nez

# Matériaux PBR glTF
MATERIALS = [
    # 0: peinture (carrosserie extérieure blanc métallisé TGV inOui)
    {"name": "peinture", "factor": [0.85, 0.86, 0.88, 1.0], "metallic": 0.45, "roughness": 0.25},
    # 1: vitrage (pare-brise PBR bleuté très transparent)
    {"name": "vitrage", "factor": [0.10, 0.12, 0.18, 0.30], "metallic": 0.0, "roughness": 0.02, "blend": True, "doubleSided": True},
    # 2: accent (filet bleu nuit TGV inOui)
    {"name": "accent", "factor": [0.04, 0.07, 0.18, 1.0], "metallic": 0.30, "roughness": 0.30},
    # 3: jupe (bas de caisse & tablier gris anthracite mat)
    {"name": "jupe", "factor": [0.20, 0.21, 0.23, 1.0], "metallic": 0.05, "roughness": 0.70},
    # 4: soufflet (attelage / joint noir)
    {"name": "soufflet", "factor": [0.05, 0.05, 0.05, 1.0], "metallic": 0.0, "roughness": 0.85},
    # 5: interieur (murs cabine gris mat double-face)
    {"name": "interieur", "factor": [0.45, 0.46, 0.48, 1.0], "metallic": 0.0, "roughness": 0.80, "doubleSided": True},
    # 6: pupitre (tableau de bord noir / gris sombre mat)
    {"name": "pupitre", "factor": [0.12, 0.13, 0.15, 1.0], "metallic": 0.0, "roughness": 0.75},
    # 7: ecran (écrans KVB, speedometer & ATESS bleuté brillant)
    {"name": "ecran", "factor": [0.05, 0.15, 0.30, 1.0], "metallic": 0.0, "roughness": 0.10},
    # 8: bouton (commandes, leviers Z & S, voyants lumineux)
    {"name": "bouton", "factor": [0.85, 0.20, 0.15, 1.0], "metallic": 0.3, "roughness": 0.30},
    # 9: siege (siège conducteur velours bleu)
    {"name": "siege", "factor": [0.08, 0.10, 0.25, 1.0], "metallic": 0.0, "roughness": 0.90},
]
(MAT_PAINT, MAT_GLASS, MAT_ACCENT, MAT_SKIRT, MAT_BELLOWS,
 MAT_INTERIOR, MAT_PUPITRE, MAT_ECRAN, MAT_BOUTON, MAT_SIEGE) = range(10)


def norm(v):
    l = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
    return (v[0]/l, v[1]/l, v[2]/l) if l > 1e-6 else (0.0, 1.0, 0.0)

def cross(a, b):
    return (a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0])

def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


class Part:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.tangents, self.indices = [], [], [], [], []

    def add_quad(self, p0, p1, p2, p3, n=None, uv0=(0,0), uv1=(1,0), uv2=(1,1), uv3=(0,1)):
        """Ajoute un quad 3D plan avec calcul automatique de normale et tangente."""
        if n is None:
            v01 = sub(p1, p0)
            v02 = sub(p2, p0)
            n = norm(cross(v01, v02))
        v10 = sub(p1, p0)
        tan = norm(v10) if (v10[0]**2 + v10[1]**2 + v10[2]**2) > 1e-6 else (1.0, 0.0, 0.0)
        tg = (tan[0], tan[1], tan[2], 1.0)

        base = len(self.positions)
        for p, uv in zip((p0, p1, p2, p3), (uv0, uv1, uv2, uv3)):
            self.positions.append(p)
            self.normals.append(n)
            self.uvs.append(uv)
            self.tangents.append(tg)
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def add_box(self, x0, y0, z0, x1, y1, z1, top=True, bot=True, front=True, back=True, left=True, right=True):
        """Construit un pavé droit axis-aligned."""
        # 8 coins
        c000, c100 = (x0, y0, z0), (x1, y0, z0)
        c110, c010 = (x1, y1, z0), (x0, y1, z0)
        c001, c101 = (x0, y0, z1), (x1, y0, z1)
        c111, c011 = (x1, y1, z1), (x0, y1, z1)

        if back:   self.add_quad(c000, c100, c110, c010, (0, 0, -1))
        if front:  self.add_quad(c101, c001, c011, c111, (0, 0, 1))
        if left:   self.add_quad(c001, c000, c010, c011, (-1, 0, 0))
        if right:  self.add_quad(c100, c101, c111, c110, (1, 0, 0))
        if bot:    self.add_quad(c001, c101, c100, c000, (0, -1, 0))
        if top:    self.add_quad(c010, c110, c111, c011, (0, 1, 0))


parts = [Part() for _ in MATERIALS]

# ==============================================================================
# 1. PLANCHER & CHASSIS (Floor & Skirts)
# ==============================================================================
# Dalle de plancher principal (du nez à la queue)
parts[MAT_PAINT].add_box(-HALF_W, FLOOR_Y - 0.15, Z_TIP, HALF_W, FLOOR_Y, Z_TAIL, top=True, bot=True)

# Jupe inférieure (apron gris mat sous la caisse)
parts[MAT_SKIRT].add_box(-HALF_W + 0.05, RAIL + 0.40, Z_TIP + 0.50, HALF_W - 0.05, FLOOR_Y - 0.15, Z_TAIL - 0.20)

# ==============================================================================
# 2. MURS LATÉRAUX & TOIT (Main Body Shell)
# ==============================================================================
# Murs latéraux arrière (derrière la cabine : z de Z_CAB_REAR à Z_TAIL)
parts[MAT_PAINT].add_box(-HALF_W, FLOOR_Y, Z_CAB_REAR, -HALF_W + 0.12, ROOF_Y, Z_TAIL) # Mur gauche
parts[MAT_PAINT].add_box(HALF_W - 0.12, FLOOR_Y, Z_CAB_REAR, HALF_W, ROOF_Y, Z_TAIL)   # Mur droit

# Toit principal (du fond cabine z = Z_CAB_REAR jusqu'à la queue)
parts[MAT_PAINT].add_box(-HALF_W, ROOF_Y, Z_CAB_REAR, HALF_W, ROOF_Y + 0.25, Z_TAIL)

# Obturation arrière (queue)
parts[MAT_PAINT].add_box(-HALF_W, FLOOR_Y, Z_TAIL - 0.05, HALF_W, ROOF_Y + 0.25, Z_TAIL, front=False)

# ==============================================================================
# 3. STRUCTURE DE LA CABINE (Flancs & Piliers de Pare-brise)
# ==============================================================================
# Cloison arrière de la cabine (séparation moteur/cabine) avec double face
parts[MAT_INTERIOR].add_box(-HALF_W + 0.10, FLOOR_Y, Z_CAB_REAR - 0.08, HALF_W - 0.10, ROOF_Y, Z_CAB_REAR)

# Murs latéraux de la cabine (de z = Z_CAB_REAR à z = Z_PUPITRE)
# Côté gauche (x = -HALF_W)
parts[MAT_PAINT].add_box(-HALF_W, FLOOR_Y, Z_PUPITRE, -HALF_W + 0.12, ROOF_Y, Z_CAB_REAR)
parts[MAT_INTERIOR].add_box(-HALF_W + 0.12, FLOOR_Y, Z_PUPITRE, -HALF_W + 0.15, ROOF_Y, Z_CAB_REAR)

# Côté droit (x = +HALF_W)
parts[MAT_PAINT].add_box(HALF_W - 0.12, FLOOR_Y, Z_PUPITRE, HALF_W, ROOF_Y, Z_CAB_REAR)
parts[MAT_INTERIOR].add_box(HALF_W - 0.15, FLOOR_Y, Z_PUPITRE, HALF_W - 0.12, ROOF_Y, Z_CAB_REAR)

# Toit au-dessus de la cabine & Visière pare-brise (z de Z_CAB_REAR jusqu'à z = -3.20 m)
parts[MAT_PAINT].add_box(-HALF_W, ROOF_Y, -3.20, HALF_W, ROOF_Y + 0.25, Z_CAB_REAR)
parts[MAT_INTERIOR].add_box(-HALF_W + 0.10, ROOF_Y - 0.05, -3.20, HALF_W - 0.10, ROOF_Y, Z_CAB_REAR)

# Piliers A du pare-brise (Left & Right A-Pillars sloping forward)
# Pilier A Gauche (x = -1.45 à -0.90)
p_pil_L_top_back  = (-HALF_W, ROOF_Y, -3.20)
p_pil_L_top_front = (-0.90, ROOF_Y, -3.20)
p_pil_L_bot_back  = (-HALF_W, 0.65, Z_PUPITRE)
p_pil_L_bot_front = (-0.90, 0.65, -3.45)

parts[MAT_PAINT].add_quad(p_pil_L_top_back, p_pil_L_top_front, p_pil_L_bot_front, p_pil_L_bot_back)
parts[MAT_INTERIOR].add_quad(p_pil_L_top_front, p_pil_L_top_back, p_pil_L_bot_back, p_pil_L_bot_front)

# Pilier A Droit (x = +0.90 à +1.45)
p_pil_R_top_front = (0.90, ROOF_Y, -3.20)
p_pil_R_top_back  = (HALF_W, ROOF_Y, -3.20)
p_pil_R_bot_front = (0.90, 0.65, -3.45)
p_pil_R_bot_back  = (HALF_W, 0.65, Z_PUPITRE)

parts[MAT_PAINT].add_quad(p_pil_R_top_front, p_pil_R_top_back, p_pil_R_bot_back, p_pil_R_bot_front)
parts[MAT_INTERIOR].add_quad(p_pil_R_top_back, p_pil_R_top_front, p_pil_R_bot_front, p_pil_R_bot_back)

# ==============================================================================
# 4. CAPOT INFÉRIEUR & NEZ PLONGEANT (Lower Nose Hood)
# ==============================================================================
# Capot plongeant sous le pupitre (z de -3.45 m à z = Z_TIP, y de 0.0 à 0.65 m)
p_hood_back_L  = (-0.90, 0.65, -3.45)
p_hood_back_R  = (0.90, 0.65, -3.45)
p_hood_front_R = (0.60, RAIL + 1.55, Z_TIP + 0.80)
p_hood_front_L = (-0.60, RAIL + 1.55, Z_TIP + 0.80)

parts[MAT_PAINT].add_quad(p_hood_back_L, p_hood_back_R, p_hood_front_R, p_hood_front_L)

# Flancs du nez (raccordement sous piliers)
parts[MAT_PAINT].add_quad((-HALF_W, 0.65, Z_PUPITRE), (-0.90, 0.65, -3.45), p_hood_front_L, (-HALF_W, 0.0, Z_TIP + 0.80))
parts[MAT_PAINT].add_quad((0.90, 0.65, -3.45), (HALF_W, 0.65, Z_PUPITRE), (HALF_W, 0.0, Z_TIP + 0.80), p_hood_front_R)

# Museau / Tablier avant à la pointe (z = Z_TIP + 0.80 à Z_TIP)
parts[MAT_SKIRT].add_box(-0.65, RAIL + 0.40, Z_TIP, 0.65, RAIL + 1.55, Z_TIP + 0.80)

# ==============================================================================
# 5. VITRAGE PBR SÉPARÉ (Windshield & Windows)
# ==============================================================================
# LE PARE-BRISE (Fitted into the physical windshield opening between A-Pillars & Roof)
p_win_top_L = (-0.88, ROOF_Y - 0.02, -3.20)
p_win_top_R = (0.88, ROOF_Y - 0.02, -3.20)
p_win_bot_R = (0.88, 0.66, -3.44)
p_win_bot_L = (-0.88, 0.66, -3.44)

# Vitrage PBR translucide (double-face)
parts[MAT_GLASS].add_quad(p_win_top_L, p_win_top_R, p_win_bot_R, p_win_bot_L)

# Vitres latérales de cabine
parts[MAT_GLASS].add_box(-HALF_W - 0.01, 0.80, -4.00, -HALF_W + 0.13, 1.45, -3.00, bot=False, top=False, front=False, back=False)
parts[MAT_GLASS].add_box(HALF_W - 0.13, 0.80, -4.00, HALF_W + 0.01, 1.45, -3.00, bot=False, top=False, front=False, back=False)

# ==============================================================================
# 6. POSTE DE COMMANDE & ERGONOMIE CABINE (Dashboard, Levers, Displays & Seat)
# ==============================================================================
# Dalle du pupitre de conduite (sous le pare-brise)
parts[MAT_PUPITRE].add_box(-0.92, 0.45, -3.40, 0.92, 0.65, -2.75)

# Panneau d'instrumentation incliné (face au conducteur)
p_dash_top_L = (-0.88, 0.82, -3.15)
p_dash_top_R = (0.88, 0.82, -3.15)
p_dash_bot_R = (0.88, 0.65, -2.95)
p_dash_bot_L = (-0.88, 0.65, -2.95)
parts[MAT_PUPITRE].add_quad(p_dash_top_L, p_dash_top_R, p_dash_bot_R, p_dash_bot_L)
parts[MAT_PUPITRE].add_box(-0.88, 0.65, -3.15, 0.88, 0.82, -2.95, top=False)

# Écrans de contrôle & cadrans (KVB, Tachymètre & ATESS)
# Écran KVB (gauche)
parts[MAT_ECRAN].add_box(-0.65, 0.68, -3.08, -0.35, 0.79, -3.05)
# Compteur de vitesse / Tachymètre central
parts[MAT_ECRAN].add_box(-0.15, 0.68, -3.08, 0.15, 0.80, -3.05)
# Écran ATESS / Témoins (droite)
parts[MAT_ECRAN].add_box(0.35, 0.68, -3.08, 0.65, 0.79, -3.05)

# Manipulateurs & Commandes (Levier Z traction & Levier S frein)
# Levier Z (traction) - Console gauche
parts[MAT_BOUTON].add_box(-0.60, 0.65, -2.85, -0.52, 0.68, -2.78)
parts[MAT_BOUTON].add_box(-0.57, 0.68, -2.82, -0.55, 0.78, -2.80) # levier droit

# Levier S (frein) - Console droite
parts[MAT_BOUTON].add_box(0.52, 0.65, -2.85, 0.60, 0.68, -2.78)
parts[MAT_BOUTON].add_box(0.55, 0.68, -2.82, 0.57, 0.78, -2.80) # levier droit

# Bouton coup de poing urgence (rouge)
parts[MAT_BOUTON].add_box(-0.04, 0.65, -2.82, 0.04, 0.69, -2.76)

# Siège Conducteur TGV Ergonomique (aligné avec driver head x=0, y=1.20, z=-2.50)
parts[MAT_PUPITRE].add_box(-0.25, 0.00, -2.65, 0.25, 0.45, -2.15)  # Socle métallique
parts[MAT_SIEGE].add_box(-0.28, 0.45, -2.70, 0.28, 0.55, -2.10)    # Assise velours
parts[MAT_SIEGE].add_box(-0.26, 0.55, -2.12, 0.26, 1.15, -2.02)     # Dossier contoured
parts[MAT_SIEGE].add_box(-0.18, 1.15, -2.12, 0.18, 1.30, -2.00)     # Appui-tête

# Accoudoirs du siège
parts[MAT_PUPITRE].add_box(-0.35, 0.70, -2.55, -0.28, 0.75, -2.25)
parts[MAT_PUPITRE].add_box(0.28, 0.70, -2.55, 0.35, 0.75, -2.25)


# ==============================================================================
# SÉRIALISATION GLTF BINAIRE (.GLB)
# ==============================================================================
def align4(n):
    return (n + 3) & ~3

used = [(i, p) for i, p in enumerate(parts) if len(p.positions) > 0]

part_blocks, blocks = [], []
for mat_idx, p in used:
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
        {"bufferView": base, "componentType": 5126, "count": len(p.positions), "type": "VEC3", "min": pmin, "max": pmax},
        {"bufferView": base + 1, "componentType": 5126, "count": len(p.normals), "type": "VEC3"},
        {"bufferView": base + 2, "componentType": 5126, "count": len(p.uvs), "type": "VEC2"},
        {"bufferView": base + 3, "componentType": 5126, "count": len(p.tangents), "type": "VEC4"},
        {"bufferView": base + 4, "componentType": 5125, "count": len(p.indices), "type": "SCALAR"},
    ]
    targets = [34962, 34962, 34962, 34962, 34963]
    for k in range(5):
        buffer_views.append({"buffer": 0, "byteOffset": offsets[base + k], "byteLength": len(part_blocks[slot][k]), "target": targets[k]})
    primitives.append({
        "attributes": {"POSITION": base, "NORMAL": base + 1, "TEXCOORD_0": base + 2, "TANGENT": base + 3},
        "indices": base + 4, "material": slot
    })

materials = []
for mat_idx, _ in used:
    mat_def = MATERIALS[mat_idx]
    m = {
        "name": mat_def["name"],
        "pbrMetallicRoughness": {
            "baseColorFactor": mat_def["factor"],
            "metallicFactor": mat_def["metallic"],
            "roughnessFactor": mat_def["roughness"]
        }
    }
    if mat_def.get("blend"):
        m["alphaMode"] = "BLEND"
        m["doubleSided"] = True
    elif mat_def.get("doubleSided"):
        m["doubleSided"] = True
    materials.append(m)

gltf = {
    "asset": {"version": "2.0", "generator": "noire-tgv-procedural-v3-aaa (CC0)"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "TGV_motrice"}],
    "meshes": [{"primitives": primitives}],
    "materials": materials,
    "accessors": accessors,
    "bufferViews": buffer_views,
    "buffers": [{"byteLength": total}],
}

json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
json_bytes += b" " * (align4(len(json_bytes)) - len(json_bytes))
bin_pad = bytes(bin_data) + b"\x00" * (align4(total) - total)

out = sys.argv[1] if len(sys.argv) > 1 else "tgv_procedural.glb"
glb_len = 12 + 8 + len(json_bytes) + 8 + len(bin_pad)
with open(out, "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, glb_len))
    f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
    f.write(json_bytes)
    f.write(struct.pack("<II", len(bin_pad), 0x004E4942))
    f.write(bin_pad)

nv = sum(len(p.positions) for p in parts)
ni = sum(len(p.indices) for p in parts)
print(f"{out} : Motrice TGV AAA V3, {LENGTH:.2f} x {HALF_W * 2:.2f} m")
print("  " + ", ".join(f"{m['name']}={len(p.positions)}v" for m, p in zip(MATERIALS, parts)))
print(f"  {nv} sommets, {ni // 3} triangles, {glb_len} octets")
