# L'application « simulateur » : point d'entrée qui pilote le moteur.
add_executable(noire_sim src/main.cpp)
add_executable(noire::sim ALIAS noire_sim)

target_link_libraries(noire_sim PRIVATE noire::engine)
set_target_properties(noire_sim PROPERTIES OUTPUT_NAME "noire-sim")

noire_set_warnings(noire_sim)
