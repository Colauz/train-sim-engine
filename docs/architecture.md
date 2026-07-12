# Architecture — Noire Engine

## Vue en couches

Chaque couche ne dépend que des couches inférieures. Aucune dépendance
descendante : le moteur ne connaît pas le jeu.

```
┌──────────────────────────────────────────────────────────────┐
│  runtime/         Le simulateur (gameplay, scénarios, UI)      │  ← application
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
  et d'aiguillages), monde persistant, IA des trains, horaires.

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
- **M2 — Monde & caméra** : origine flottante, caméra libre, terrain simple.
- **M3 — Voie & train** : génération de rails par splines, un bogie physique.
- **M4 — Streaming** : chargement/déchargement tuilé sur longue distance.
- **M5 — Réalisme** : adhérence dynamique, météo, audio Doppler.
