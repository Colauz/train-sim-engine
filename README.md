# Noire Engine

Moteur de jeu **from scratch** dédié à la **simulation ferroviaire hyper-réaliste**
(nom de code *Noire*, d'après `projet_noire`).

## Stack technique

| Domaine        | Choix                     | Pourquoi (résumé)                                                  |
| -------------- | ------------------------- | ----------------------------------------------------------------- |
| Langage        | **C++20**                 | Écosystème moteur/middleware (Jolt, EnTT…), références abondantes  |
| API graphique  | **Vulkan**                | Multi-plateforme (Linux+open source), multithread, longue portée  |
| Build          | **CMake ≥ 3.24** + presets | `FetchContent` + `FIND_PACKAGE_ARGS`, zéro conf. manuelle          |
| Math           | glm                       | Standard de fait, header-only                                     |
| Log            | spdlog                    | Rapide, confiné derrière une façade `noire::log`                   |

## Prérequis

- Un compilateur C++20 (GCC ≥ 13, Clang ≥ 17, MSVC 19.3x) — testé sur **GCC 15**
- **CMake ≥ 3.24**
- Un accès réseau au premier `configure` (récupération de spdlog/glm)

## Compiler & lancer

```bash
# Configuration (Debug)
cmake --preset debug

# Compilation
cmake --build build/debug

# Exécution du simulateur
./build/debug/bin/noire-sim
```

> Sans Ninja installé, CMake utilise le générateur par défaut (Make sous Linux).
> Pour de meilleurs temps de build : `sudo apt install ninja-build`.

## Structure

Voir [`docs/architecture.md`](docs/architecture.md) pour l'architecture détaillée
et l'arborescence des modules.

## Licence

Open source — licence à définir (MIT ou Apache-2.0 recommandé).
