# Dépendances tierces du moteur.
#
# On utilise FetchContent avec FIND_PACKAGE_ARGS (CMake >= 3.24) : si la lib est
# déjà installée sur le système, on la réutilise ; sinon on la clone et on la
# compile automatiquement. Zéro configuration manuelle pour le premier build.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# --- spdlog : journalisation rapide -------------------------------------------
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS NAMES spdlog)

# --- glm : mathématiques (vecteurs, matrices, quaternions), header-only -------
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    ON
    FIND_PACKAGE_ARGS NAMES glm)

FetchContent_MakeAvailable(spdlog glm)

# À venir, au fil des modules :
#   - glfw        (fenêtrage + entrées, multi-plateforme)
#   - Vulkan SDK  (find_package(Vulkan)) + VulkanMemoryAllocator + vk-bootstrap
#   - EnTT        (ECS)
#   - JoltPhysics (corps rigides : bogies, suspensions)
#   - miniaudio / OpenAL Soft (audio spatialisé)
#   - Tracy       (profiler temps réel)
