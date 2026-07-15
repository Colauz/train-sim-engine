# Compilation des shaders GLSL -> SPIR-V -> en-tête C++ embarqué.
#
# Avantage de l'embarquement : le binaire final n'a aucune dépendance à un
# dossier « shaders/ » au runtime, et le format d'embarquement (file(READ HEX))
# est indépendant du compilateur (glslc OU glslangValidator).

if(NOT DEFINED NOIRE_GLSL_COMPILER)
    find_program(NOIRE_GLSL_COMPILER
        NAMES glslc glslangValidator glslang
        HINTS "${Vulkan_GLSLC_EXECUTABLE}" "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}"
              "$ENV{VULKAN_SDK}/bin")
endif()

if(NOT NOIRE_GLSL_COMPILER)
    message(FATAL_ERROR
        "Aucun compilateur de shaders trouvé (glslc / glslangValidator).\n"
        "  Ubuntu/Debian : sudo apt install glslang-tools\n"
        "  ou installez le Vulkan SDK LunarG (fournit glslc).")
endif()

get_filename_component(_noire_glsl_name "${NOIRE_GLSL_COMPILER}" NAME_WE)
message(STATUS "Compilateur de shaders : ${NOIRE_GLSL_COMPILER}")

# noire_add_shaders(<cible> <shader1> <shader2> ...)
# Chemins des shaders relatifs au CMakeLists.txt appelant.
#
# #include : glslc COMME glslangValidator le gèrent nativement via -I (extension
# GL_GOOGLE_include_directive). Aucun préprocesseur maison n'est donc nécessaire — les
# shaders incluent depuis `shaders/`, ce qui permet de ne déclarer le bloc GlobalUBO et
# le coeur PBR QU'UNE FOIS (ils étaient dupliqués dans 5 shaders, et il fallait patcher
# les 5 à chaque évolution de l'UBO : la divergence n'était qu'une question de temps).
function(noire_add_shaders target)
    set(_gen_root ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(_gen_dir  ${_gen_root}/shaders)
    file(MAKE_DIRECTORY ${_gen_dir})

    # Les .glsl inclus ne se compilent pas seuls, mais TOUT shader en dépend : sans ça,
    # modifier le coeur PBR ne déclencherait aucune recompilation.
    file(GLOB _includes CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/*.glsl)

    foreach(_src IN LISTS ARGN)
        get_filename_component(_name "${_src}" NAME)   # triangle.vert
        string(REPLACE "." "_" _var "${_name}")        # triangle_vert
        set(_abs ${CMAKE_CURRENT_SOURCE_DIR}/${_src})
        set(_spv ${CMAKE_CURRENT_BINARY_DIR}/${_name}.spv)
        set(_hdr ${_gen_dir}/${_name}.spv.h)
        set(_inc_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)

        if(_noire_glsl_name STREQUAL "glslc")
            set(_compile_cmd ${NOIRE_GLSL_COMPILER} "${_abs}" -I "${_inc_dir}" -o "${_spv}")
        else()  # glslangValidator / glslang
            set(_compile_cmd ${NOIRE_GLSL_COMPILER} -V "${_abs}" -I"${_inc_dir}" -o "${_spv}")
        endif()

        add_custom_command(
            OUTPUT  ${_hdr}
            COMMAND ${_compile_cmd}
            COMMAND ${CMAKE_COMMAND}
                    -DSPV_FILE=${_spv} -DHEADER_FILE=${_hdr} -DVAR_NAME=${_var}_spv
                    -P ${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake
            DEPENDS ${_abs} ${_includes} ${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake
            COMMENT "Shader : ${_name} -> SPIR-V embarqué"
            VERBATIM)

        target_sources(${target} PRIVATE ${_hdr})
    endforeach()

    # Les shaders sont inclus via #include "shaders/<nom>.spv.h"
    target_include_directories(${target} PRIVATE ${_gen_root})
endfunction()
