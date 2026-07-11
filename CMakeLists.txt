cmake_minimum_required(VERSION 3.24)

# =============================================================================
#  Noire Engine — moteur de jeu sur mesure pour la simulation ferroviaire
#  (nom de code « Noire », d'après projet_noire)
# =============================================================================
project(NoireEngine
    VERSION 0.1.0
    DESCRIPTION "Moteur maison pour la simulation ferroviaire hyper-réaliste"
    HOMEPAGE_URL "https://github.com/jaminfollietlaurian/train-sim-engine"
    LANGUAGES CXX)

# --- Options du projet --------------------------------------------------------
option(NOIRE_BUILD_TESTS        "Compiler les tests unitaires"          OFF)
option(NOIRE_BUILD_TOOLS        "Compiler l'éditeur et les outils"      OFF)
option(NOIRE_WARNINGS_AS_ERRORS "Traiter les warnings comme des erreurs" OFF)

# --- Réglages globaux ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Type de build par défaut si l'utilisateur n'en précise pas (générateurs mono-config).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Type de build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Sorties regroupées : bin/ pour les exécutables, lib/ pour les bibliothèques.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(CompilerWarnings)   # définit noire_set_warnings()
include(Dependencies)       # récupère spdlog, glm, ...

# --- Modules du moteur --------------------------------------------------------
add_subdirectory(engine)    # la bibliothèque « noire::engine »
add_subdirectory(runtime)   # l'exécutable du simulateur

if(NOIRE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(NOIRE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Noire Engine ${PROJECT_VERSION} configuré (${CMAKE_BUILD_TYPE})")
