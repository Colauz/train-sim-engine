# Les cibles importées par find_package sont scopées au répertoire courant : il faut
# donc (re)chercher Threads ici, car il est lié directement plus bas (miniaudio l'utilise).
find_package(Threads REQUIRED)

add_library(noire_audio
    src/audio_engine.cpp
    src/rail_audio.cpp
    src/miniaudio_impl.cpp)
add_library(noire::audio ALIAS noire_audio)

target_include_directories(noire_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# miniaudio est un détail d'implémentation PRIVATE (masqué par PIMPL). Sous Linux
# il faut dl (dlopen des backends), pthread et m ; CMAKE_DL_LIBS + Threads couvrent.
target_link_libraries(noire_audio
    PUBLIC  noire::core
    PRIVATE miniaudio Threads::Threads ${CMAKE_DL_LIBS})

# L'implémentation de miniaudio (TU dédiée) : on coupe les warnings du code tiers.
if(MSVC)
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

noire_set_warnings(noire_audio)
