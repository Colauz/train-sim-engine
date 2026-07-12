add_library(noire_platform src/window.cpp)
add_library(noire::platform ALIAS noire_platform)

target_include_directories(noire_platform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Vulkan (headers) est PUBLIC : window.hpp expose VkInstance/VkSurfaceKHR.
# GLFW reste PRIVATE : détail d'implémentation masqué à l'appelant.
target_link_libraries(noire_platform
    PUBLIC  Vulkan::Vulkan
    PRIVATE glfw noire::core)

noire_set_warnings(noire_platform)
