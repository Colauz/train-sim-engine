add_library(noire_core
    src/log.cpp
    src/engine.cpp)
add_library(noire::core ALIAS noire_core)

target_compile_features(noire_core PUBLIC cxx_std_20)

# En-têtes publics : engine/core/include/noire/core/*.hpp -> #include <noire/core/...>
target_include_directories(noire_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# version.hpp est généré depuis la version du projet CMake.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/noire/core/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/noire/core/version.hpp
    @ONLY)
target_include_directories(noire_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>)

# glm est exposé dans l'API publique (types math) ; spdlog reste un détail interne.
target_link_libraries(noire_core
    PUBLIC  glm::glm
    PRIVATE spdlog::spdlog)

noire_set_warnings(noire_core)
