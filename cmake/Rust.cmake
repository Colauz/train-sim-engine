# Intégration Rust (PoC M13.5) — n'est incluse QUE si NOIRE_USE_RUST=ON.
#
# Corrosion (corrosion-rs) importe un crate Cargo comme une cible CMake ordinaire :
# on la lie et on en dépend comme de n'importe quelle bibliothèque. Récupérée via
# FetchContent, exactement comme spdlog/glm/GLFW (cf. Dependencies.cmake) — aucune
# étape manuelle, aucun outil global à installer (cbindgen tourne dans build.rs).

include(FetchContent)

FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5.2
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(Corrosion)

# Le crate déclare déjà crate-type = ["staticlib"] : Corrosion crée la cible `noire_procgen`.
corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_SOURCE_DIR}/crates/noire_procgen/Cargo.toml
    PROFILE release)

# Dossier de l'en-tête généré par build.rs. Créé au configure pour que
# target_include_directories pointe sur un chemin existant même avant le premier cargo build ;
# l'en-tête lui-même apparaît quand la cible noire_procgen se construit (garantie par
# add_dependencies côté scene).
set(NOIRE_PROCGEN_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/crates/noire_procgen/generated
    CACHE INTERNAL "Répertoire de l'en-tête FFI généré par cbindgen")
file(MAKE_DIRECTORY ${NOIRE_PROCGEN_INCLUDE_DIR})
