add_library(noire_app src/application.cpp)
add_library(noire::app ALIAS noire_app)

target_include_directories(noire_app PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Tout est PRIVATE : grâce au PIMPL, application.hpp n'expose ni Vulkan ni GLFW.
# Le runtime qui lie noire::app ne voit donc AUCUN détail graphique.
target_link_libraries(noire_app
    PRIVATE noire::core noire::platform noire::render noire::physics noire::scene noire::audio
            noire::resource)

noire_set_warnings(noire_app)
