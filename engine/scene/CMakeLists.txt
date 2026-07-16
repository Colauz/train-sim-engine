add_library(noire_scene
    src/catenary.cpp
    src/track_mesh.cpp
    src/world_streamer.cpp
    src/terrain_clipmap.cpp)
add_library(noire::scene ALIAS noire_scene)

target_include_directories(noire_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# scene relie le monde (core::Spline) à la géométrie rendue (render::Vertex).
target_link_libraries(noire_scene PUBLIC noire::core noire::render)

noire_set_warnings(noire_scene)

# --- PoC M13.5 : chemin Rust pour la génération de voie ----------------------
# Tout est sous NOIRE_USE_RUST. OFF (défaut) => rien de tout ceci n'existe, la voie est
# générée par track_mesh.cpp comme avant : le kill switch est au niveau du build lui-même.
if(NOIRE_USE_RUST)
    target_sources(noire_scene PRIVATE src/track_mesh_rust.cpp)
    # PRIVATE : seul noire_scene voit la macro (le #if du kill switch dans world_streamer.cpp
    # et la déclaration guardée dans track_mesh.hpp) ; les autres cibles l'ignorent.
    target_compile_definitions(noire_scene PRIVATE NOIRE_USE_RUST=1)
    # L'en-tête FFI est généré par cbindgen (build.rs) — dossier fixé par cmake/Rust.cmake.
    target_include_directories(noire_scene PRIVATE ${NOIRE_PROCGEN_INCLUDE_DIR})
    target_link_libraries(noire_scene PRIVATE noire_procgen)
    # Force la construction du crate (donc la génération de l'en-tête) AVANT toute
    # compilation de noire_scene : sinon track_mesh_rust.cpp pourrait compiler avant que
    # build.rs n'ait écrit noire_procgen.h.
    add_dependencies(noire_scene noire_procgen)

    # Banc A/B : prouve que le chemin Rust produit la même géométrie que le C++. Il lui
    # faut la macro pour voir la déclaration guardée de generate_track_mesh_rust.
    add_executable(noire-procgen-ab test/procgen_ab.cpp)
    target_compile_definitions(noire-procgen-ab PRIVATE NOIRE_USE_RUST=1)
    target_link_libraries(noire-procgen-ab PRIVATE noire::scene noire::core)
    noire_set_warnings(noire-procgen-ab)
endif()
