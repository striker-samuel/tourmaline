# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Dense grass stress sample: the real PlacementWorldRuntime scatter/cull/
# prefix/visible/indirect chain with 16 x 64m terrain-backed tiles.
project(grass_sample)

add_executable(05_grass 05_grass.cpp)
set_target_properties(05_grass PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_link_libraries(05_grass PRIVATE engine KTX::ktx)

add_custom_command(TARGET 05_grass POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:05_grass>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:05_grass>"
)
tourmaline_stage_sdl3(05_grass)
