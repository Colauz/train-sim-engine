# Le moteur est découpé en modules indépendants qui s'empilent au-dessus de « core ».

add_subdirectory(core)          # math, log, allocateurs, boucle principale
add_subdirectory(platform)      # fenêtre, entrées (GLFW)
add_subdirectory(render)        # backend Vulkan (vk-bootstrap + VMA), triangle
add_subdirectory(app)           # orchestration platform + render + core
# add_subdirectory(resource)    # streaming du monde, gestion des assets     (à venir)
# add_subdirectory(physics)     # dynamique ferroviaire, bogies, adhérence   (à venir)
# add_subdirectory(audio)       # audio spatialisé, Doppler                  (à venir)
# add_subdirectory(scene)       # ECS, réseau de voies, monde, IA            (à venir)

# Cible agrégée : lier « noire::engine » suffit à récupérer tout le moteur.
add_library(noire_engine INTERFACE)
add_library(noire::engine ALIAS noire_engine)
target_link_libraries(noire_engine INTERFACE noire::app noire::core)
