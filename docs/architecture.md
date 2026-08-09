# Architecture — Noire Engine

## Vue en couches

Chaque couche ne dépend que des couches inférieures. Aucune dépendance
descendante : le moteur ne connaît pas le jeu.

```
┌──────────────────────────────────────────────────────────────┐
│  runtime/         Le point d'entrée (main.cpp → Application)   │  ← exécutable
├──────────────────────────────────────────────────────────────┤
│  app              Orchestration PIMPL : gameplay, HUD, inputs  │  ← simulateur
├──────────────────────────────────────────────────────────────┤
│  scene            ECS · réseau de voies · monde · IA trains    │  ← runtime moteur
├───────────┬───────────┬───────────┬───────────┬───────────────┤
│  render   │  physics  │  audio    │  resource │               │  ← sous-systèmes
│  (Vulkan) │ (ferro.)  │ (spatial) │(streaming)│               │
├───────────┴───────────┴───────────┴───────────┴───────────────┤
│  core             math · mémoire · job system · log · boucle   │  ← fondations
├──────────────────────────────────────────────────────────────┤
│  platform         fenêtre · entrées · filesystem · horloge     │  ← OS
└──────────────────────────────────────────────────────────────┘
```

## Rôle des modules

- **platform** — abstraction OS : fenêtrage (GLFW), entrées, accès disque
  asynchrone, horloge haute résolution.
- **core** — briques transverses : bibliothèque math, allocateurs custom
  (pool, arena, stack), **job system** (thread pool + graphe de tâches),
  log/asserts, sérialisation, base de l'ECS, et la **boucle principale**.
- **render** — RHI Vulkan (abstraction fine), **render graph**, système de
  matériaux, génération/lissage des rails par splines, terrain, culling
  (frustum + occlusion), LOD, atmosphère et météo dynamique.
- **physics** — le cœur « métier » ferroviaire : dynamique longitudinale du
  convoi (efforts d'attelage, jeu de tampons/*slack action*), pneumatique de
  freinage, contact **roue/rail** (adhérence, glissement — modèles de
  Kalker/Polach), corps rigides des bogies et suspensions (via Jolt).
- **audio** — audio spatialisé (atténuation, **effet Doppler**), sons
  procéduraux (joints de rail, crissement en courbe).
- **resource** — **world streaming** tuilé et géoréférencé : I/O asynchrone,
  cache LRU, éviction, LOD de streaming, cuisson d'assets.
- **scene** — ECS, représentation du **réseau de voies** (graphe de segments
  et d'aiguillages), monde persistant, IA des trains, horaires ; aujourd'hui aussi la
  génération procédurale du décor : caténaire, viaducs, gares, ville, terrain clipmap.
- **app** — l'orchestrateur (PIMPL) : assemble tous les modules, tient l'état du
  simulateur (Mascon, ATS, portes, caméras, météo, HUD) et pilote la boucle `core`.
  C'est le seul module qui connaisse à la fois le moteur et les règles du jeu.

## Décisions transverses (critiques pour le ferroviaire)

1. **Pas de temps fixe** pour la simulation, rendu interpolé (déjà implémenté
   dans `core/engine.cpp`). Garantit le **déterminisme** de la physique,
   indispensable pour une simulation lourde reproductible.
2. **Origine flottante + double précision monde.** Sur des centaines de km, un
   `float` (~7 chiffres significatifs) perd toute précision → tremblements
   géométriques. Coordonnées monde en `double`, rendu **relatif à la caméra**
   pour rester en `float` côté GPU.
3. **Job system** comme colonne vertébrale : streaming, physique et génération
   des command buffers Vulkan tournent en parallèle. C'est ici que le choix
   C++/threads (ou plus tard `std::execution`) porte ses fruits.
4. **Séparation simulation / rendu** stricte : deux fréquences, deux threads,
   états échangés par double-buffering interpolé.

## Feuille de route (jalons)

- **M0 — Fondations** *(fait)* : build CMake, `core` (log + boucle fixe), exécutable.
- **M1 — Fenêtre & Vulkan** *(fait)* : `platform` (GLFW), `render` (Vulkan via
  vk-bootstrap + VMA, premier triangle), `app` (orchestration PIMPL). Boucle `core`
  rendue indépendante du graphique via des callbacks (`EngineHooks`).
- **M2 — Monde & caméra** *(fait)* : origine flottante (positions monde `double`,
  Model relatif caméra ramené en `float` — `render` ne voit aucun `double`), caméra
  libre (fly, inputs clavier/souris), UBO caméra + push-constant Model, vertex
  buffers & depth buffer via VMA, scène grille + cubes.
- **M3 — Voie & train** *(fait)* : `Spline` Catmull-Rom en `double` (paramétrée par
  distance d'arc, LUT arc-longueur), génération procédurale des rails (`scene`, sommets
  relatifs à l'origine de voie, calculée une seule fois), `Bogie` cinématique (`physics`)
  aligné sur la tangente, avancé dans `fixed_update`. Courbe en S de ~500 m.
- **M4 — Physique multi-corps** *(fait)* : `Wagon` (2 bogies + caisse), dynamique
  `F = m·a`, gravité projetée sur la pente, adhérence acier/acier (patinage si effort
  > μ·N), suspension 1D ressort/amortisseur (pilonnement + tangage). Bogie devenu passif.
- **M5 — World Streaming** *(fait)* : voie **infinie** analytique (`TrackSource` /
  `ProceduralTrack`), tuiles de 2 km générées en asynchrone (`core::JobSystem` +
  `scene::WorldStreamer`), upload GPU budgété, destruction GPU différée, GC des
  tuiles lointaines. Origine flottante **par tuile**. Physique découplée du streaming.
- **M6 — Réalisme sensoriel** *(fait)* : module `audio` (miniaudio) — spatialisation 3D
  + Doppler, listener sur la caméra, émetteurs sur le train ; audio ferroviaire procédural
  (joints « clac-clac » selon la vitesse, crissement selon la courbure, roulement) ;
  météo dynamique (`wetness` → brouillard de distance + assombrissement via UBO global).
- **M7 — Adhérence météo & assets** *(fait)* : `wetness` couplé à μ (pluie/feuilles),
  pipeline d'assets (`resource` : cgltf + stb), éclairage.
- **M8 — PBR & IBL** *(fait)* : shader Cook-Torrance, tone mapping ACES, tangentes
  géométriques, matériaux multi-textures, skybox HDR équirectangulaire → cubemap,
  IBL complet (SH9 pour l'irradiance, spéculaire préfiltré, extraction du soleil).
- **M9 — Voie 3D** *(fait)* : profil en I, traverses, ballast, LOD de voie, mipmaps,
  Reverse-Z (précision jusqu'à 10 km), accotement procédural, tuiles de 500 m.
- **M10–M12 — Terrain & végétation** *(fait)* : geo-clipmap 3D (amplitude 25 m),
  splatting PBR herbe/craie, tuilage stochastique avec fondu, macro-variation d'albédo,
  végétation instanciée (ombres, alpha discard, vent, wrap lighting), CSM réparées,
  caténaires + pendules, anticrénelage par couverture analytique.
- **M13 — Traction & frein** *(fait)* : hyperbole puissance/vitesse, frein pneumatique
  (conduite générale), HUD Vulkan optimisé. **M13.5** : PoC FFI Rust (Corrosion) pour la
  géométrie de voie — **−19 % de perf**, `NOIRE_USE_RUST` reste OFF.
- **M14–M15 — Sensoriel** *(fait)* : sifflet spatialisé + Doppler natif, shader de pluie
  procédurale, écran de chargement avec fondu, frustum culling CPU de la végétation.
- **M16–M22 — La rame** *(fait)* : `Consist`/`CarBody` avec cinématique inverse et bogies
  Jacobs partagés, KVB puis profil de vitesse SNCF, gare (0–400 m), ciel atmosphérique et
  nuages, phares dynamiques, portes animées, mode arcade (`K`), intérieur avec sièges et
  vitrages PBR.
- **M29–M32 — Pivot Neo-Tokyo** *(fait)* : abandon du TGV pour un **métro japonais type
  E235** (`tools/gen_metro.py`), caméra cabine FPS, viaducs et gratte-ciels procéduraux,
  hassha melody à la fermeture des portes, diagnostics physiques.
- **M33–M36 — Poste de conduite** *(fait)* : **Mascon** à crans (`EB`/`B8..B1`/`N`/`P1..P5`),
  **ATS** avec délai de grâce de 10 s et réarmement `EB → N`, menu Pause (moteur audio
  inclus), correctifs des portes.
- **M37–M47 — Ville procédurale** *(fait, puis élagué)* : immeubles instanciés, HUD ATS,
  lampadaires ; le réseau routier a traversé une longue série d'échecs (altitude, relief,
  autotiling, collisions avec la voie) et a été **abandonné en M47** au profit du train,
  des gares et des shaders.
- **M48–M52 — Gares** *(fait)* : fenêtres aléatoires, sol unifié global et **isolation
  mathématique des gares** (purge des routes buggées), échelle du toit et repère d'arrêt
  corrigés, climatiseurs sur le toit de la rame, gap quai-train supprimé, alignement des
  **portes palières (PSD)** et **logique d'arrêt de précision** (tolérance 50 cm,
  décision verrouillée à l'appui sur `P`).
- **M53 — Matière et arrêt à quai** *(fait)* : bibliothèque de textures PBR
  procédurales tuilables (`tools/gen_textures.py`) appliquée au béton du viaduc, aux
  quais, au sol de la ville, à la carrosserie, à l'intérieur de la rame et au poste de
  conduite — avec des UV **métriques** partout (planaires par axe côté rame, en mètres
  divisés par la période côté monde). Façades de quai ramenées à la hauteur
  **mi-hauteur** de la Yamanote (1,30 m au-dessus du quai, donc sous la ligne de
  caisse) et **synchronisées** avec les portes de la rame (même vitesse, même courbe de
  coulissement, même instant de départ). Aide à l'arrêt de précision : losange
  au-dessus des façades, ligne d'arrêt peinte, écart signé et règle graduée au pupitre.
  Le banc d'alignement a confirmé que les baies tombaient déjà en face des portes à
  ±1 mm : ce qui manquait n'était pas la géométrie, c'était le moyen de viser.
