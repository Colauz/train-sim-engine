add_library(noire_render
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/renderer.cpp
    src/vma_impl.cpp)
add_library(noire::render ALIAS noire_render)

target_include_directories(noire_render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Vulkan est PUBLIC (types exposés dans les en-têtes) ; vk-bootstrap et VMA
# restent des détails d'implémentation PRIVATE.
target_link_libraries(noire_render
    PUBLIC  Vulkan::Vulkan
    PRIVATE vk-bootstrap::vk-bootstrap VulkanMemoryAllocator noire::core)

# VMA génère beaucoup de warnings tiers : on les fait taire pour CE fichier
# (implémentation isolée dans sa propre unité de traduction).
if(MSVC)
    set_source_files_properties(src/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Shaders GLSL -> SPIR-V -> en-têtes embarqués.
include(Shaders)
noire_add_shaders(noire_render
    shaders/triangle.vert
    shaders/triangle.frag)

noire_set_warnings(noire_render)
