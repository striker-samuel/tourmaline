# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Standalone Asteroids game over the downloaded Sponza demonstration scene.
# Reuses the existing .tapp/.pak, without recooking or bundling Sponza.
add_library(asteroids_sim STATIC AsteroidsGame.cpp)
target_compile_features(asteroids_sim PUBLIC cxx_std_20)
target_include_directories(asteroids_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# RandomStream is header-only and CPU-only; no engine/Vulkan linking.
target_include_directories(asteroids_sim PUBLIC
    "${TOURMALINE_SOURCE_DIR}/engine/include")

# Project-local fragment shader: compiled to the exe-relative dev shader tree.
compile_shaders(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${CMAKE_BINARY_DIR}/content/shaders"
    AsteroidsUi
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "${TOURMALINE_SOURCE_DIR}/engine/shaders"
    HEADER_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "${TOURMALINE_SOURCE_DIR}/engine/shaders"
)

add_executable(Asteroids main.cpp PixelArt.cpp)
target_include_directories(Asteroids PRIVATE
    "${TOURMALINE_SOURCE_DIR}/external/stb")
# The HUD reads original CC0 PNG assets from an executable-relative folder.
foreach(art sprites.png font.png sprites.spriteset font.spriteset)
    add_custom_command(TARGET Asteroids POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:Asteroids>/content/asteroids"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/${art}"
            "$<TARGET_FILE_DIR:Asteroids>/content/asteroids/${art}"
        VERBATIM
    )
endforeach()
add_dependencies(Asteroids AsteroidsUi_shaders)
# Reproducible original chiptune: MIDI is editable source, WAV is what
# the engine's existing AudioEngine/SDL3 path plays (no synth dependency).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(ASTEROIDS_AUDIO_DIR "${CMAKE_BINARY_DIR}/content/asteroids/audio")
set(ASTEROIDS_AUDIO_FILES
    "${ASTEROIDS_AUDIO_DIR}/music.mid"
    "${ASTEROIDS_AUDIO_DIR}/music.wav"
    "${ASTEROIDS_AUDIO_DIR}/laser.wav"
    "${ASTEROIDS_AUDIO_DIR}/hit.wav"
    "${ASTEROIDS_AUDIO_DIR}/wave.wav"
    "${ASTEROIDS_AUDIO_DIR}/gameover.wav")
add_custom_command(
    OUTPUT ${ASTEROIDS_AUDIO_FILES}
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_audio.py"
        --output "${ASTEROIDS_AUDIO_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate_audio.py"
    COMMENT "Generate original Asteroids MIDI and WAV soundtrack / SFX"
    VERBATIM)
add_custom_target(AsteroidsAudio DEPENDS ${ASTEROIDS_AUDIO_FILES})
add_dependencies(Asteroids AsteroidsAudio)

target_link_libraries(Asteroids PRIVATE tourmaline_apphost asteroids_sim)
tourmaline_host_setup(Asteroids)

add_executable(asteroids_sim_tests tests.cpp)
target_link_libraries(asteroids_sim_tests PRIVATE asteroids_sim)
if(BUILD_TESTING)
    add_test(NAME asteroids_sim_tests COMMAND asteroids_sim_tests)
    add_test(NAME asteroids_audio_tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate_audio.py" --self-test)
    add_executable(asteroids_spriteset_tests test_spriteset.cpp)
    target_compile_features(asteroids_spriteset_tests PRIVATE cxx_std_20)
    target_include_directories(asteroids_spriteset_tests PRIVATE
        "${TOURMALINE_SOURCE_DIR}/engine/include")
    add_test(NAME asteroids_spriteset_tests
        COMMAND asteroids_spriteset_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME asteroids_art_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_art.py")
    endif()
endif()
