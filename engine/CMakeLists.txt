# Le moteur est découpé en modules indépendants qui s'empilent au-dessus de « core ».

add_subdirectory(core)          # math, log, spline, caméra, boucle principale
add_subdirectory(platform)      # fenêtre, entrées (GLFW)
add_subdirectory(render)        # backend Vulkan (vk-bootstrap + VMA)
add_subdirectory(physics)       # dynamique ferroviaire : bogie cinématique
add_subdirectory(scene)         # génération procédurale de la voie (rails)
add_subdirectory(app)           # orchestration (dépend des modules ci-dessus)
# add_subdirectory(resource)    # streaming du monde, gestion des assets     (à venir)
# add_subdirectory(audio)       # audio spatialisé, Doppler                  (à venir)

# Cible agrégée : lier « noire::engine » suffit à récupérer tout le moteur.
add_library(noire_engine INTERFACE)
add_library(noire::engine ALIAS noire_engine)
target_link_libraries(noire_engine INTERFACE noire::app noire::core)
