# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(GiBaker)

# GiBaker as a library (the editor links it and calls gibaker::Run
# directly; the CLI below stays a thin argv interface over the same
# code). The lib build excludes main() via GIBAKER_LIB_BUILD.
add_library(GiBakerLib STATIC main.cpp)

target_compile_definitions(GiBakerLib PRIVATE ENGINE_VULKAN GIBAKER_LIB_BUILD)

target_include_directories(GiBakerLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tools/common
)

target_link_libraries(GiBakerLib
    PUBLIC
        engine
        gltf_import
        volk
        KTX::ktx
)

add_executable(GiBaker main.cpp)

target_compile_definitions(GiBaker PRIVATE ENGINE_VULKAN)

target_include_directories(GiBaker
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/common
)

target_include_directories(GiBaker
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/common
)

target_link_libraries(GiBaker
    PRIVATE
        engine
        gltf_import
        volk
        KTX::ktx
)

set_target_properties(GiBaker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to GiBaker.exe.
# OptickCore is SHARED via engine: stage it too.
add_custom_command(TARGET GiBaker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:GiBaker>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:GiBaker>"
)

tourmaline_stage_sdl3(GiBaker)
