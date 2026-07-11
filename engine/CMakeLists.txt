# Le moteur est découpé en modules indépendants qui s'empilent au-dessus de « core ».
# On active chaque module au fur et à mesure de son implémentation.

add_subdirectory(core)          # math, log, allocateurs, job system, boucle principale
# add_subdirectory(platform)    # fenêtre, entrées, filesystem, horloge   (à venir)
# add_subdirectory(resource)    # streaming du monde, gestion des assets    (à venir)
# add_subdirectory(render)      # backend Vulkan, render graph, LOD         (à venir)
# add_subdirectory(physics)     # dynamique ferroviaire, bogies, adhérence  (à venir)
# add_subdirectory(audio)       # audio spatialisé, Doppler, procédural     (à venir)
# add_subdirectory(scene)       # ECS, réseau de voies, monde, IA           (à venir)

# Cible agrégée : lier « noire::engine » suffit à récupérer tout le moteur.
add_library(noire_engine INTERFACE)
add_library(noire::engine ALIAS noire_engine)
target_link_libraries(noire_engine INTERFACE
    noire::core
    # noire::render noire::physics noire::audio noire::resource noire::scene
)
