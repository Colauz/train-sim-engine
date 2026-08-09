# Noire Engine

Moteur de jeu **from scratch** dédié à la **simulation ferroviaire hyper-réaliste**
(nom de code *Noire*, d'après `projet_noire`).

État actuel (**M53**) : une ligne de **métro japonais** (rame type E235) roulant dans un
environnement urbain procédural « Neo-Tokyo » — voie infinie streamée, terrain
geo-clipmap, gares avec quais carrelés, bande podotactile et **portes palières (PSD)
mi-hauteur** synchronisées avec la rame, caténaires, viaducs, immeubles instanciés,
PBR + IBL + ombres cascadées, textures procédurales sur toutes les surfaces bâties et
sur la rame, audio spatialisé, météo dynamique, **ATS** (contrôle de vitesse), **Mascon**
à crans et **arrêt de précision** assisté au pupitre.

---

## 🚀 Lancer le projet

### 1. Prérequis système (Ubuntu / Debian)

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake ninja-build git \
    libvulkan-dev vulkan-tools \
    glslang-tools \
    xorg-dev
```

| Prérequis                         | Détail                                                      |
| --------------------------------- | ----------------------------------------------------------- |
| Compilateur **C++20**             | GCC ≥ 13, Clang ≥ 17, MSVC 19.3x — testé sur **GCC 15**      |
| **CMake ≥ 3.24**                  | requis pour `FetchContent` + `FIND_PACKAGE_ARGS`             |
| **Vulkan** (loader + headers)     | `libvulkan-dev` ou le Vulkan SDK LunarG                      |
| **Compilateur de shaders**        | `glslc` ou `glslangValidator` (paquet `glslang-tools`)       |
| **X11** (Linux)                   | `xorg-dev` — GLFW est compilé en X11 seul (XWayland sinon)   |
| **GPU Vulkan 1.3**                | pilote installé ; vérifier avec `vulkaninfo --summary`       |
| **Accès réseau**                  | au *premier* `configure` uniquement (dépendances FetchContent) |

> ⚠️ Wayland natif est **désactivé** volontairement (`GLFW_BUILD_WAYLAND=OFF`) : la
> fenêtre passe par XWayland, ce qui fonctionne sans configuration supplémentaire.

### 2. Compiler

```bash
# Configuration (télécharge spdlog, glm, GLFW, vk-bootstrap, VMA, miniaudio, cgltf, stb)
cmake --preset debug

# Compilation
cmake --build build/debug -j$(nproc)
```

Variante optimisée (recommandée pour *jouer*, le Debug est lent en rendu) :

```bash
cmake --preset release           # = RelWithDebInfo
cmake --build build/release -j$(nproc)
```

### 3. Lancer le simulateur

```bash
# Depuis la RACINE du dépôt (important : les assets sont cherchés à partir du CWD)
./build/debug/bin/noire-sim

# ou, en Release
./build/release/bin/noire-sim
```

Le binaire s'appelle **`noire-sim`** et atterrit dans `<dossier-de-build>/bin/`.

**Où sont les assets ?** Au démarrage, `AssetPaths::discover()` cherche un dossier
`assets/` en remontant jusqu'à 6 niveaux depuis le répertoire courant — lancer depuis la
racine du dépôt ou depuis `build/debug/` fonctionne donc dans les deux cas. En cas de
doute, forcez-le :

```bash
NOIRE_ASSETS=/chemin/absolu/vers/train-sim-engine/assets ./build/debug/bin/noire-sim
```

Les shaders, eux, sont **compilés en SPIR-V et embarqués dans le binaire** : aucun
dossier `shaders/` n'est nécessaire au runtime.

> Sans Ninja, CMake utilise le générateur par défaut (Make sous Linux) — ça marche, c'est
> juste plus lent. `sudo apt install ninja-build` pour de meilleurs temps de build.

---

## 🎮 Commandes

### Conduite (Mascon japonais à crans)

Le manipulateur unique va de `EB` (urgence) à `P5` (pleine traction) en passant par
`B8…B1` (freinage) et `N` (neutre). Chaque appui déplace **d'un cran**.

| Touche              | Action                                                      |
| ------------------- | ----------------------------------------------------------- |
| `Z` / `W` / `↑`     | Mascon **+1 cran** (vers `P5`)                               |
| `S` / `↓`           | Mascon **−1 cran** (vers `EB`)                               |
| `E`                 | **Freinage d'urgence** immédiat (`EB`)                       |
| `Espace`            | Frein manuel d'appoint (maintenu)                            |
| `H`                 | Sifflet / klaxon (maintenu, spatialisé + Doppler)            |
| `P`                 | Portes : ouverture immédiate / fermeture après *hassha melody* (3 s) |
| `K`                 | **Isolation de l'ATS** (mode « arcade », contrôle total)     |
| `L`                 | Phares                                                       |
| `R` (ou `M`)        | Bascule **pluie / temps sec** (brouillard, adhérence, gouttes) |

### Caméra & fenêtre

| Touche               | Action                                                      |
| -------------------- | ----------------------------------------------------------- |
| `C`                  | Bascule **Cabine (FPS)** ⇄ **Externe (orbite)**              |
| Souris               | Orientation (orbite ou regard cabine)                        |
| `Ctrl` / `Maj` gauche | Zoom arrière / avant (mode orbite uniquement)               |
| `F11`                | Plein écran                                                  |
| `Échap`              | **Menu Pause** (fige la simulation et l'audio)               |

### Menu Pause

`↑`/`↓` (ou `Z`/`S`) pour naviguer, `Entrée` ou `Espace` pour valider, ou directement
`1` = Reprendre, `2` = Plein écran, `3` = Quitter.

### Réarmer l'ATS après une urgence

L'ATS déclenche un freinage d'urgence en cas de survitesse maintenue (10 s de grâce).
Pour le réarmer : **arrêt complet** → Mascon sur `EB` → Mascon sur `N`.

### Lire le HUD

`HEURE` · `VITESSE` · `ATS <aspect> <limite>` (G / YG / Y / R) · `MASCON` ·
`CG` (pression de conduite générale, 5 bar = desserré) · `PENTE` · `METEO` ·
`FPS / GPU ms`, plus les témoins `IMMOBILISE`, `ATS ISOLE`, `ATS SURVITESSE`,
`URGENCE`, `PATINAGE`.

### Arrêt de précision (M52 / M53)

Les **portes palières** ne s'ouvrent que si la rame est arrêtée **à moins de 50 cm** du
repère d'arrêt au moment où l'on appuie sur `P`. Sinon seules les portes de la rame
s'ouvrent — dans le vide. La décision est verrouillée à l'appui.

Pour viser, quatre repères, tous dérivés du **même** point d'arrêt :

| Repère                      | Où                                                            |
| --------------------------- | -------------------------------------------------------------- |
| **Losange** (停止位置目標)   | sur mât, des deux côtés, à +2,95 m — au-dessus des façades      |
| **Ligne d'arrêt** peinte     | en travers des deux quais, au chainage exact du losange          |
| **`ARRET` au HUD**           | distance restante, puis écart signé au centimètre sous 12 m      |
| **Règle graduée**            | sous le pupitre : curseur = la rame, zone verte = la tolérance   |

À l'arrêt, le HUD annonce le verdict : `ARRET PARFAIT` (< 15 cm), `A QUAI` (< 50 cm) ou
`HORS TOLERANCE` avec l'écart. Les façades de quai s'ouvrent **exactement en même temps**
que les portes de la rame (même vitesse, même courbe, même instant).

---

## ⚙️ Options de build

```bash
cmake --preset debug -DNOIRE_WARNINGS_AS_ERRORS=ON
```

| Option                    | Défaut | Effet                                                       |
| ------------------------- | :----: | ----------------------------------------------------------- |
| `NOIRE_WARNINGS_AS_ERRORS` |  OFF  | Traite les warnings comme des erreurs                        |
| `NOIRE_USE_RUST`          |  OFF   | PoC M13.5 : génération de la voie déléguée à un crate Rust via Corrosion (**kill switch** : à OFF, aucune trace de Rust dans le build) |
| `NOIRE_BUILD_TESTS`       |  OFF   | ⚠️ `tests/` ne contient pas encore de `CMakeLists.txt` — activer casse le configure |
| `NOIRE_BUILD_TOOLS`       |  OFF   | ⚠️ idem pour `tools/` (les outils y sont des scripts Python, pas des cibles CMake) |

### Le chemin Rust (PoC M13.5)

`-DNOIRE_USE_RUST=ON` nécessite une toolchain Rust (`rustup`) ; Corrosion et `cbindgen`
sont récupérés automatiquement. Un banc A/B `noire-procgen-ab` est alors construit pour
vérifier que la géométrie produite est identique au chemin C++. **Conclusion du PoC :
−19 % de perf côté Rust**, donc l'interrupteur reste sur OFF et le générateur C++ est le
chemin par défaut.

---

## 🔧 Variables d'environnement (banc de mesure)

Ces variables sont des **leviers de mesure**, jamais des réglages de jeu.

| Variable                        | Effet                                                             |
| ------------------------------- | ------------------------------------------------------------------ |
| `NOIRE_ASSETS=<dir>`            | Force la racine des assets (court-circuite la remontée depuis le CWD) |
| `NOIRE_SPEED=<km/h>`            | Vitesse initiale de la rame (défaut : 20 km/h)                     |
| `NOIRE_PIN_CAM=1`               | Verrouille la caméra (cadrage reproductible pour les A/B à l'image) |
| `NOIRE_PITCH=<rad>`             | Pitch de la caméra épinglée (avec `NOIRE_PIN_CAM`)                 |
| `NOIRE_STILL=1`                 | Gèle la physique, laisse courir l'horloge du vent                  |
| `NOIRE_CREEP=<m/frame>`         | Translation caméra par frame présentée (test de précision)         |
| `NOIRE_NOCULL=1`                | Désactive le frustum culling CPU des bâtiments                     |
| `NOIRE_NO_VSYNC=1`              | Désactive la V-Sync (mesure de framerate brut)                     |
| `NOIRE_CAM_X/Y/Z`               | Position de la tête du conducteur en cabine (défaut `0 / 0.25 / −8.55`) |
| `NOIRE_CAM_NEAR` (`NOIRE_CAB_ZNEAR`) | Plan proche en cabine (défaut 0.10 m)                         |

Exemple de run reproductible :

```bash
NOIRE_PIN_CAM=1 NOIRE_STILL=1 NOIRE_NO_VSYNC=1 ./build/release/bin/noire-sim
```

---

## 🎨 Régénérer les assets procéduraux

Les modèles `.glb`, les textures `.png` et les sons `.wav` d'`assets/` sont
**versionnés** : rien à faire pour lancer le jeu. Ils sont produits par des scripts
Python (**stdlib uniquement**, aucune dépendance à installer) :

```bash
python3 tools/gen_textures.py    # jeux PBR procéduraux (base color / ARM / normale)
python3 tools/gen_metro.py       # rame E235 : motrice (cabine), voiture, bogie
python3 tools/gen_station.py     # gare : quais, verrière, portes palières
python3 tools/gen_building.py    # immeubles building_a/b/c
python3 tools/gen_tree.py        # végétation instanciée
python3 tools/gen_streetlamp.py  # lampadaires
python3 tools/gen_train.py       # ancien TGV procédural (hérité, avant le pivot M30)
python3 tools/gen_hassha_melody.py   # jingle de départ japonais
python3 tools/gen_ats_alarm.py       # buzzer ATS
python3 tools/check_coplanar.py      # diagnostic de coplanarité des faces
```

> `gen_textures.py` doit tourner **avant** `gen_metro.py` : la rame référence les cartes
> d'`assets/textures/train/` par URI relative depuis ses `.glb`.
>
> Les jeux produits sont tuilables et **calés en mètres** : une tuile fait 4 m pour les
> surfaces du monde (béton, carrelage de quai, enrobé) et 1 m pour la rame et la bande
> podotactile. Cette période doit rester d'accord avec l'`uv_period` de la géométrie
> (`viaduct.cpp`, `UV_PERIOD` de `gen_metro.py`, `kGroundUvPeriod` de `application.cpp`),
> sinon les textures sortent à la mauvaise échelle.

---

## 🧱 Stack technique

| Domaine        | Choix                      | Pourquoi (résumé)                                                 |
| -------------- | -------------------------- | ----------------------------------------------------------------- |
| Langage        | **C++20**                  | Écosystème moteur/middleware, références abondantes               |
| API graphique  | **Vulkan**                 | Multi-plateforme (Linux + open source), multithread, longue portée |
| Build          | **CMake ≥ 3.24** + presets | `FetchContent` + `FIND_PACKAGE_ARGS`, zéro conf. manuelle          |
| Fenêtrage      | GLFW 3.4                   | Multi-plateforme, intégration Vulkan, encapsulé dans `platform`    |
| Init. Vulkan   | vk-bootstrap               | Supprime le boilerplate instance/device/swapchain                  |
| Mémoire GPU    | VMA 3.3                    | Allocateur Vulkan, socle du world streaming                        |
| Math           | glm 1.0.1                  | Standard de fait, header-only                                      |
| Log            | spdlog 1.14                | Rapide, confiné derrière une façade `noire::log`                   |
| Audio          | miniaudio 0.11             | Spatialisation 3D + Doppler, header unique                         |
| Modèles        | cgltf 1.14                 | Lecture glTF / GLB, header unique                                  |
| Textures       | stb_image                  | PNG / JPG / HDR, header unique                                     |
| Rust (option)  | Corrosion 0.5              | PoC FFI, désactivé par défaut (`NOIRE_USE_RUST=OFF`)               |

Toutes les dépendances sont récupérées automatiquement au `configure` — **rien à
installer à la main** hormis les prérequis système listés plus haut.

## 📁 Structure

```
engine/      le moteur, en modules empilés
  core/      math, log, boucle fixe, job system, spline, terrain, caméra, limites de vitesse
  platform/  fenêtre + entrées (GLFW)
  render/    Vulkan (vk-bootstrap + VMA), shaders GLSL→SPIR-V embarqués, HUD
  physics/   wagon, bogies, consist, frein pneumatique, adhérence
  scene/     voie, caténaire, viaduc, gares, ville, terrain clipmap, world streamer
  audio/     miniaudio, audio ferroviaire procédural
  resource/  cache d'assets, loaders glTF / images / HDR / audio
  app/       orchestration (PIMPL) — c'est ici que vit la logique du simulateur
runtime/     l'exécutable noire-sim (main.cpp, ~15 lignes)
crates/      noire_procgen (PoC Rust, optionnel)
tools/       générateurs d'assets en Python
assets/      modèles .glb, textures (world/ + train/ procédurales, ballast, ciel HDR), sons
cmake/       modules : dépendances, shaders, warnings, Rust
```

Voir [`docs/architecture.md`](docs/architecture.md) pour l'architecture en couches, les
décisions transverses et la feuille de route détaillée (M0 → M52).

---

## 🩹 Dépannage

| Symptôme                                                     | Solution                                                                 |
| ------------------------------------------------------------ | ------------------------------------------------------------------------ |
| `Aucun compilateur de shaders trouvé (glslc / glslangValidator)` | `sudo apt install glslang-tools`, ou installer le Vulkan SDK LunarG   |
| `Could NOT find Vulkan`                                       | `sudo apt install libvulkan-dev`                                         |
| Erreur de compilation de GLFW (X11 manquant)                  | `sudo apt install xorg-dev`                                              |
| `AssetPaths : dossier 'assets' introuvable`                   | Lancer depuis la racine du dépôt, ou définir `NOIRE_ASSETS`              |
| Fenêtre noire / crash à l'init Vulkan                         | Vérifier le pilote : `vulkaninfo --summary`                              |
| Le premier `configure` échoue                                 | Il faut un accès réseau (FetchContent clone les dépendances)             |
| Rendu très lent                                               | Utiliser le preset `release` : le `debug` n'est pas optimisé             |
| Repartir de zéro                                              | `rm -rf build/debug && cmake --preset debug`                             |

## Licence

Open source — licence à définir (MIT ou Apache-2.0 recommandé).
