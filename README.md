# Noire Engine

Moteur de jeu **from scratch** dédié à la **simulation ferroviaire hyper-réaliste**
(nom de code *Noire*, d'après `projet_noire`).

État actuel (**M57**) : une ligne de **métro japonais** (rame type E235) roulant dans un
environnement urbain procédural « Neo-Tokyo » — voie infinie streamée, terrain
geo-clipmap, gares avec quais carrelés, bande podotactile, **éclairage de quai** et
**portes palières (PSD) mi-hauteur** synchronisées avec la rame, caténaires, viaducs,
immeubles instanciés, PBR + IBL + ombres cascadées, **cycle jour/nuit qui éclaire
vraiment**, textures procédurales sur toutes les surfaces bâties et sur la rame, audio
spatialisé, météo dynamique, **ATS** (contrôle de vitesse, avec sa marge), **anti-recul**,
**Mascon** à crans et **arrêt de précision** assisté au pupitre.

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
| `F1`                 | **Niveau de qualité** : cycle BAS → MOYEN → HAUT              |
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
`FPS / GPU ms`, plus les témoins `IMMOBILISE`, `ANTI-RECUL`, `ATS ISOLE`,
`ATS SURVITESSE`, `URGENCE`, `PATINAGE`.

### `ANTI-RECUL` : pourquoi la rame ne part pas en arrière (M57)

Aucun inverseur n'est modélisé : cette rame ne circule **que vers l'avant**. Une vitesse
négative n'est donc jamais une manœuvre, c'est une dérive — mascon au neutre, frein
desserré, et la rampe qui reprend la rame. Le matériel réel a exactement ce dispositif
(*転動防止*, prévention du roulement) : dès que le sens s'inverse sans commande, le frein
de maintien serre et **tient** la rame. Le témoin bleu `ANTI-RECUL (PENTE)` s'allume tant
qu'il travaille, et la rame repart normalement dès le premier cran de traction.

Sans lui — et c'était le cas jusqu'au M56 — une rame laissée au neutre sur la rampe de
1,1 % accélérait indéfiniment vers l'arrière (−6,5 km/h au bout de 17 s, et toujours en
train de prendre de la vitesse) : elle quittait sa gare à reculons, le compteur affichait
un nombre négatif et l'ATS, qui comparait une vitesse **signée** à une limite positive,
ne la voyait jamais en survitesse.

### La marge de l'ATS (M57)

L'ATS tolère **5 km/h** au-dessus de la limite affichée avant d'armer son avertissement
(`ats_margin_kmh`) — sauf sur un aspect `R`, où l'arrêt est absolu et le moindre
mouvement le franchit. La consigne d'aide à la conduite vise 2 km/h **sous** la limite :
il reste donc 7 km/h entre « je suis la consigne » et « l'ATS s'énerve ».

### Aide à la conduite : quelle vitesse, à quel moment (M55)

Le pupitre affiche, sous la ligne ATS, la **consigne** : la vitesse à tenir *maintenant*
pour honorer tout ce qui vient — un abaissement de limite dans 300 m, un arrêt en gare
dans 700 m. C'est le principe de tout ATC/TASC réel : pour chaque contrainte à venir, la
cinématique donne la vitesse maximale admissible ici, et la consigne est le **minimum**
de toutes.

```
ATS  Y      45 KM/H        <- ce qui est INTERDIT
CONSIGNE    43 KM/H        <- ce qu'il FAUT tenir
  -> FREINAGE DANS  204 M  <- à quel moment serrer
```

La troisième ligne change selon la situation :

| Ligne                     | Sens                                                        |
| ------------------------- | ------------------------------------------------------------ |
| `MAINTENIR`               | vous êtes sur la courbe, rien à faire                        |
| `FREINAGE DANS 204 M`     | compte à rebours jusqu'au point de freinage                  |
| `FREINER B4`              | vous êtes au-dessus : voici le cran à passer                 |
| `ACCELERER`               | vous traînez de plus de 6 km/h sous la consigne              |

La couleur de la consigne suffit à piloter sans lire les chiffres : **vert** = sur la
courbe, **jaune** = léger excès, **rouge** = franchement trop vite, **bleu** = trop lent.

Le cran conseillé tient compte de la **pente** et de la résistance à l'avancement : en
rampe, la gravité freine déjà pour vous, et l'aide ne conseille que le complément.

> **Vérifié** : un banc simule un conducteur qui suit la consigne à la lettre depuis
> 90 km/h. La rame s'immobilise à **0,00 m** du repère d'arrêt.

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

## 🌃 Le cycle jour/nuit (M21, réellement branché au M57)

L'heure court à 60× (une journée simulée en 24 min) et se lit au pupitre. `NOIRE_HOUR`
fixe l'heure de départ ; le défaut est **22 h**, donc de nuit.

Jusqu'au M56, ce cycle était **décoratif sur toute la géométrie**. Le ciel, lui,
s'assombrissait bien — mais l'ambiante image-based, elle, ne suivait pas : la cubemap
HDR et ses harmoniques sphériques viennent d'un HDRI de *plein jour*, figé au
chargement, et rien ne les modulait. Résultat : à 2 h du matin, sous un ciel noir, les
quais, la verrière, le viaduc et les rails restaient éclairés par un soleil de midi.
Mesuré au pixel sur le banc épinglé, une dalle de quai rendait **(114, 131, 150) à 13 h
et (114, 131, 150) à 2 h** — rigoureusement identique.

Le gain vit désormais dans **un seul fichier**, `engine/render/shaders/common/sky.glsl`,
que `skybox.frag` et `common/pbr.glsl` incluent tous les deux — le ciel qu'on voit et le
ciel qui éclaire ne peuvent plus diverger. Il porte **deux** grandeurs, parce que ce sont
deux choses différentes :

| Gain                      | Ce qu'il module                              | Plancher de nuit |
| ------------------------- | --------------------------------------------- | ---------------- |
| `skyRadianceGain`         | la skybox **et** la couleur du brouillard      | ~2 % (bleu nuit) |
| `ambientIrradianceGain`   | l'irradiance SH + l'environnement préfiltré    | ~26 %            |

Pourquoi l'ambiante ne tombe pas aussi bas que le ciel : dans une ville de cette
densité, la lumière ambiante nocturne **ne vient pas du ciel, elle vient de la ville** —
façades allumées, enseignes, éclairage public, le tout renvoyé par le bitume et la
brume. C'est exactement pour cette raison qu'on ne voit pas les étoiles à Tokyo.

**Éclairage de quai (M57).** Corollaire immédiat : dès que la nuit est une vraie nuit,
une gare sans luminaires devient noire — et l'on demanderait au conducteur un arrêt à
50 cm sur un repère qu'il ne voit pas. Une file de tubes court donc sous l'intrados de
la verrière, au-dessus de l'axe de chaque quai. Ils partent dans le maillage `signs`,
donc dans le matériau émissif **déjà** utilisé par la signalétique suspendue : aucun
matériau de plus, **aucun draw call de plus**, et la même veilleuse de jour que les néons
de la ville.

---

## 🐌 Ça rame ? Les niveaux de qualité

Trois presets, choisis au lancement par `NOIRE_QUALITY` ou cyclés en jeu par **`F1`**
(le niveau courant est affiché au pupitre) :

```bash
NOIRE_QUALITY=low ./build/release/bin/noire-sim     # « grille-pain »
NOIRE_QUALITY=medium ./build/release/bin/noire-sim
NOIRE_QUALITY=high ./build/release/bin/noire-sim    # défaut
```

| Niveau  | Ombres         | Portée d'ombre | Ville  | Pluie |
| ------- | -------------- | -------------- | ------ | ----- |
| `low`   | 512² × 2       | 90 m           | 260 m  | non   |
| `medium`| 1024² × 2      | 160 m          | 450 m  | oui   |
| `high`  | 2048² × 2      | 250 m          | 700 m  | oui   |

Les presets n'agissent que sur les **trois postes que la mesure a désignés** : le
remplissage des cartes d'ombre, la quantité de géométrie qui y entre, et l'étendue de la
ville. Ni la voie, ni la rame, ni les gares ne sont dégradées — elles ne coûtent pas
assez pour que ça vaille de les abîmer.

Mesuré sur le banc reproductible (`NOIRE_PIN_CAM=1 NOIRE_SPEED=0 NOIRE_NO_VSYNC=1`,
iGPU, 1280×720), avant/après M54 :

| | M53 | M54 `high` | M54 `medium` | M54 `low` |
| --------------- | ---- | ---- | ---- | ---- |
| images/s        | 227  | 295  | 350  | 353  |
| GPU (ms)        | 3,0  | 2,6  | 2,3  | 1,8  |
| draw calls      | 868  | 410  | 402  | 365  |

> ⚠️ **Ces chiffres sont ceux du M54 et ne se comparent plus à aujourd'hui** — deux fois
> plutôt qu'une. D'abord le M56 : en remettant à l'endroit un winding inversé, il a rendu
> RASTÉRISÉE toute la géométrie que le GPU jetait gratuitement jusque-là (la rame
> entière, entre autres). Le temps de la passe scène a plus que triplé, et c'est le prix
> normal de la correction, pas une régression. Ensuite le M57 : jusqu'à lui, le banc ne
> *tenait pas* — la rame dérivait en arrière pendant la mesure, donc deux runs ne
> cadraient jamais la même scène (c'est exactement le défaut que l'anti-recul corrige).
> Toute campagne chiffrée doit donc repartir d'un relevé neuf, et les niveaux de qualité
> re-arbitrés sur ce relevé-là. Le protocole, lui, n'a pas changé.

### Lire la télémétrie

Une ligne par seconde sur la sortie standard, avec la **ventilation** du temps :

```
250 fps | CPU 0.05 ms | GPU 2.6 ms (ombres 0.8 / scene 1.9) | draws 192+218 | bâtiments=776/2555 vis
```

`CPU` = construction de la frame côté app (hors attente VSync) ; `draws` = passe
d'ombres + passe scène. C'est cette ventilation qui dit *où* chercher : un total ne se
corrige pas, un poste identifié si.

## ⚙️ Options de build

```bash
cmake --preset debug -DNOIRE_WARNINGS_AS_ERRORS=ON
```

| Option                    | Défaut | Effet                                                       |
| ------------------------- | :----: | ----------------------------------------------------------- |
| `NOIRE_WARNINGS_AS_ERRORS` |  OFF  | Traite les warnings comme des erreurs                        |
| `NOIRE_USE_RUST`          |  OFF   | PoC M13.5 : génération de la voie déléguée à un crate Rust via Corrosion (**kill switch** : à OFF, aucune trace de Rust dans le build) |
| `NOIRE_BUILD_TESTS`       |  OFF   | Compile `noire-tests` et l'enregistre auprès de CTest (cf. ci-dessous)  |
| `NOIRE_BUILD_TOOLS`       |  OFF   | Sans effet : `tools/` ne contient que des scripts Python. Le configure le **dit** au lieu d'échouer (M57) |

### 🧪 Tests

```bash
cmake --preset debug -DNOIRE_BUILD_TESTS=ON
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug --output-on-failure

# ou directement, avec un filtre sur le nom du cas :
./build/debug/bin/noire-tests anti_recul
```

Le périmètre est assumé : **uniquement les modules sans contexte graphique** — `core`
(profil de vitesse, aide à la conduite) et `physics` (frein pneumatique, dynamique
longitudinale, ATS). Ce sont aussi les seuls dont le comportement se vérifie par un
*nombre* : « la rame s'immobilise à moins de 50 cm de son repère » est une assertion,
« la gare a l'air juste » n'en est pas une. Pour le rendu, les garde-fous restent
`tools/check_topology.py`, `tools/check_coplanar.py` et la capture comparative.

Trois cas méritent d'être cités, parce qu'ils verrouillent des affirmations que ce README
faisait sans filet :

| Cas                                             | Ce qu'il empêche de casser                        |
| ----------------------------------------------- | -------------------------------------------------- |
| `suivre_la_consigne_arrete_la_rame_sur_le_repere` | Un conducteur qui suit la consigne **dépasse** son repère (enveloppe devenue trop optimiste) |
| `anti_recul_tient_la_rame_sur_la_rampe`          | La rame repart en arrière, et le banc de mesure redevient inexploitable |
| `ats_respecte_sa_marge`                          | La marge ATS redevient un réglage que personne ne lit |

Le premier de ces cas a échoué dès son premier lancement : l'aide conseillait un cran
trop faible (B2 là où il fallait B6) et le conducteur de test passait son repère de
**74 m**. Corrigé au M57 ; il s'immobilise désormais à 0,42 m en deçà.

La cote **fine** de l'arrêt (les « 0,00 m » cités plus haut) n'est volontairement pas
assertée : elle dépend autant du modèle de conducteur que de l'aide elle-même. Ce qui
est asserté, c'est le sens de l'erreur — jamais de dépassement — et il se lit en jeu sur
la règle graduée du pupitre.

Aucune dépendance de test n'est récupérée : le micro-framework tient en 60 lignes
(`tests/check.hpp`), pour la même raison que le reste du projet n'embarque que ce qui
paie sa place.

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
| `NOIRE_QUALITY=low\|medium\|high` | Niveau de qualité initial (cf. ci-dessus ; `F1` en jeu)          |
| `NOIRE_SPEED=<km/h>`            | Vitesse initiale de la rame (défaut : 20 km/h)                     |
| `NOIRE_PIN_CAM=1`               | Verrouille la caméra (cadrage reproductible pour les A/B à l'image) |
| `NOIRE_PITCH=<rad>`             | Pitch de la caméra épinglée (orbite ET cabine, avec `NOIRE_PIN_CAM`) |
| `NOIRE_YAW=<rad>`               | Lacet de la caméra épinglée (défaut orbite : 2.30 ; cabine : 0)    |
| `NOIRE_DIST=<m>`                | Distance de l'orbite épinglée (défaut : 38 m)                      |
| `NOIRE_CAB=1`                   | Démarre en vue CABINE (sinon orbite ; `C` bascule en jeu)          |
| `NOIRE_HOUR=<h>`                | Heure de départ, en heures décimales (défaut : 22 — inspecter la géométrie de jour, comparer deux runs au même éclairage) |
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
python3 tools/check_coplanar.py assets/models/*.glb   # z-fighting : deux faces dans un même plan
python3 tools/check_topology.py assets/models/*.glb   # trous, winding, normales inversées
```

> **Les générateurs écrivent dans `assets/models/` par défaut** (et `gen_textures.py`
> dans `assets/textures/`). Un chemin en argument reste possible pour produire ailleurs.
> Avant le M56 le défaut était le répertoire COURANT : la commande du README déposait
> les `.glb` à la racine du dépôt et le jeu continuait de charger les anciens.

> Les deux `check_*.py` sont des **garde-fous à passer après toute modification de
> géométrie**, et ils sont complémentaires :
> `check_coplanar.py` traque les surfaces EN TROP (deux faces qui se disputent un plan,
> donc du scintillement), `check_topology.py` les surfaces qui MANQUENT. Ce dernier rend
> un code de sortie non nul dès qu'un triangle est cousu à l'envers — glTF impose le sens
> trigonométrique vu de l'extérieur, et le moteur rastérise en
> `VK_FRONT_FACE_COUNTER_CLOCKWISE` avec back-face culling : un triangle inversé est
> purement et simplement JETÉ par le GPU. C'est ce défaut, appliqué à l'intégralité de la
> rame, qui la rendait traversable du regard avant le M56.

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
