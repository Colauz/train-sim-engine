# Dépendances tierces du moteur.
#
# FetchContent avec FIND_PACKAGE_ARGS (CMake >= 3.24) : réutilise la version
# système si présente, sinon clone + compile automatiquement.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- Vulkan : loader + headers (fournis par le système ou le SDK) ------------
find_package(Vulkan REQUIRED)

# --- spdlog : journalisation rapide ------------------------------------------
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS NAMES spdlog)

# --- glm : mathématiques (header-only) ---------------------------------------
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS NAMES glm)

# --- GLFW : fenêtrage + entrées ----------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)  # X11 seul (XWayland au besoin) => moins de deps système
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS NAMES glfw3)

# --- vk-bootstrap : réduit le boilerplate Vulkan (instance/device/swapchain) --
# Tag aligné sur les headers Vulkan installés (VK_HEADER_VERSION 341).
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.4.341
    GIT_SHALLOW    ON)

# --- VMA : Vulkan Memory Allocator (indispensable pour le futur streaming) ----
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    ON)

# --- miniaudio : audio spatialisé + Doppler (header unique, backends via dlopen) --
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    ON)

# --- cgltf : lecture des modèles glTF / GLB (header unique) -------------------
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.14
    GIT_SHALLOW    ON)

# --- stb : décodage d'images PNG / JPG (stb_image, header unique) -------------
# Pas de tag stable en amont : on suit master (comme recommandé par le projet).
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    ON)

FetchContent_MakeAvailable(spdlog glm glfw vk-bootstrap VulkanMemoryAllocator miniaudio cgltf stb)

# miniaudio n'a pas de CMakeLists : on expose son include nous-mêmes (en SYSTEM
# pour que ses warnings ne polluent pas notre build strict).
if(NOT TARGET miniaudio)
    add_library(miniaudio INTERFACE)
    target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
endif()

# cgltf / stb : headers uniques sans CMakeLists — même traitement (include SYSTEM).
if(NOT TARGET cgltf)
    add_library(cgltf INTERFACE)
    target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
endif()
if(NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif()

# Conventions Vulkan pour glm, imposées à TOUS les consommateurs (cohérence des
# matrices) : profondeur [0,1] et angles en radians.
target_compile_definitions(glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
