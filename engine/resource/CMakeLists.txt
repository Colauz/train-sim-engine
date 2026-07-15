# Pipeline d'assets (M7) : cache central + loaders CPU (cgltf / stb_image).
# Le GPU (staging, textures) reste dans noire::render ; ce module ne fait que du
# décodage disque -> RAM et l'orchestration du cache. Il est donc GPU-agnostique.

add_library(noire_resource
    src/asset_paths.cpp
    src/image_loader.cpp
    src/hdr_loader.cpp
    src/gltf_loader.cpp
    src/resource_manager.cpp
    src/cgltf_impl.cpp
    src/stb_image_impl.cpp)
add_library(noire::resource ALIAS noire_resource)

target_include_directories(noire_resource PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# noire::render est PUBLIC : les en-têtes exposent render::MeshId/TextureId (Model).
# noire::core pour la journalisation ; cgltf/stb + noire::audio (décodage ma_decoder)
# restent des détails d'implémentation PRIVATE.
target_link_libraries(noire_resource
    PUBLIC  noire::core noire::render
    PRIVATE cgltf stb noire::audio)

# Implémentations single-header tierces : on coupe les warnings (comme vma_impl).
foreach(_tu src/cgltf_impl.cpp src/stb_image_impl.cpp)
    if(MSVC)
        set_source_files_properties(${_tu} PROPERTIES COMPILE_OPTIONS "/w")
    else()
        set_source_files_properties(${_tu} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endforeach()

noire_set_warnings(noire_resource)
