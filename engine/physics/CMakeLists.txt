add_library(noire_physics
    src/air_brake.cpp
    src/bogie.cpp
    src/car_body.cpp
    src/consist.cpp
    src/wagon.cpp)
add_library(noire::physics ALIAS noire_physics)

target_include_directories(noire_physics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# La physique ne dépend que du core (maths + spline). Aucun Vulkan, aucune fenêtre.
target_link_libraries(noire_physics PUBLIC noire::core)

noire_set_warnings(noire_physics)
