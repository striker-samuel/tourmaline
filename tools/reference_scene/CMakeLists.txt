# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
project(ReferenceScene)
add_executable(ReferenceScene main.cpp)
target_compile_definitions(ReferenceScene PRIVATE ENGINE_VULKAN)
target_link_libraries(ReferenceScene PRIVATE engine gltf_import volk KTX::ktx)
set_target_properties(ReferenceScene PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_custom_command(TARGET ReferenceScene POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>" "$<TARGET_FILE_DIR:ReferenceScene>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>" "$<TARGET_FILE_DIR:ReferenceScene>"
)
tourmaline_stage_sdl3(ReferenceScene)
