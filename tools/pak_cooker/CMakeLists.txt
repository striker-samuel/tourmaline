# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Pak)  # pak.exe: the PAK COOK - consumed artifacts only (raw glTF never cooks; import.exe / the MeshImporter produce the artifacts)

# Pak as a library (the editor links it and calls paktool::Run
# directly; the CLI stays a thin user interface over the same code).
# The lib build excludes main() via PAK_LIB_BUILD.
add_library(PakLib STATIC main.cpp terrain_cook.cpp)
target_compile_definitions(PakLib PRIVATE ENGINE_VULKAN PAK_LIB_BUILD)
target_include_directories(PakLib
PRIVATE
${CMAKE_SOURCE_DIR}/external/stb
${CMAKE_SOURCE_DIR}/tools/common
PUBLIC
${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(PakLib
PRIVATE
engine
gltf_import
artifact_cache
volk
OptickCore
)
add_executable(Pak main.cpp terrain_cook.cpp)

target_compile_definitions(Pak PRIVATE ENGINE_VULKAN)

# stb_image_write (cook-time atlas debug PNG, see lm_png.cpp; the same
# vendored header the editor uses for its PNG export).
target_include_directories(Pak PRIVATE ${CMAKE_SOURCE_DIR}/external/stb ${CMAKE_SOURCE_DIR}/tools/common)

target_link_libraries(Pak
    PRIVATE
        engine
        gltf_import
        artifact_cache
        volk
        OptickCore
)

set_target_properties(Pak PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to Pak.exe
# (the --cubemap stage encodes KTX2 in-process, like CubemapBaker).
add_custom_command(TARGET Pak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:Pak>"
)

# OptickCore is SHARED: stage its DLL next to Pak.exe
add_custom_command(TARGET Pak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:Pak>"
)

tourmaline_stage_sdl3(Pak)
