# Le moteur est découpé en modules indépendants qui s'empilent au-dessus de « core ».

add_subdirectory(core)          # math, log, spline, caméra, boucle principale
add_subdirectory(platform)      # fenêtre, entrées (GLFW)
add_subdirectory(render)        # backend Vulkan (vk-bootstrap + VMA)
add_subdirectory(physics)       # dynamique ferroviaire (wagon multi-corps)
add_subdirectory(scene)         # génération procédurale + streaming de la voie
add_subdirectory(audio)         # audio spatialisé (miniaudio), Doppler, son ferroviaire
add_subdirectory(app)           # orchestration (dépend des modules ci-dessus)
# add_subdirectory(resource)    # gestion avancée des assets                 (à venir)

# Cible agrégée : lier « noire::engine » suffit à récupérer tout le moteur.
add_library(noire_engine INTERFACE)
add_library(noire::engine ALIAS noire_engine)
target_link_libraries(noire_engine INTERFACE noire::app noire::core)
