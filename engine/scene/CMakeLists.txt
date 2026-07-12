add_library(noire_scene
    src/track_mesh.cpp
    src/world_streamer.cpp)
add_library(noire::scene ALIAS noire_scene)

target_include_directories(noire_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# scene relie le monde (core::Spline) à la géométrie rendue (render::Vertex).
target_link_libraries(noire_scene PUBLIC noire::core noire::render)

noire_set_warnings(noire_scene)
