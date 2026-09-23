# Tests unitaires du moteur — `cmake --preset debug -DNOIRE_BUILD_TESTS=ON` puis `ctest`.
#
# PÉRIMÈTRE ASSUMÉ : uniquement les modules SANS contexte graphique — core (profil de
# vitesse, aide à la conduite) et physics (frein pneumatique, dynamique longitudinale,
# ATS). Ce sont aussi les seuls dont le comportement se vérifie par un NOMBRE plutôt que
# par un oeil : « la rame s'immobilise à moins de 50 cm de son repère » est une
# assertion, « la gare a l'air juste » n'en est pas une. Pour le rendu, les garde-fous
# du projet restent tools/check_topology.py et tools/check_coplanar.py, plus la capture
# d'écran comparative.
#
# Aucune dépendance de test : cf. tests/check.hpp.
add_executable(noire_tests
    main.cpp
    test_speed_limits.cpp
    test_air_brake.cpp
    test_wagon.cpp
    test_ats.cpp
    test_driving_advisor.cpp)

set_target_properties(noire_tests PROPERTIES OUTPUT_NAME noire-tests)

target_include_directories(noire_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noire_tests PRIVATE noire::core noire::physics)

noire_set_warnings(noire_tests)

# COMMAND prend le NOM DE CIBLE (noire_tests), que CMake résout en chemin complet.
# « noire-tests » n'est que l'OUTPUT_NAME, pas une cible : ctest le cherchait tel quel
# et rapportait « Not Run » sans jamais lancer la suite.
add_test(NAME noire.unit COMMAND noire_tests)
