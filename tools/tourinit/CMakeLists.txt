# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Thin native adapter over the reusable static tooling archive.
add_executable(tourinit main.cpp)
target_link_libraries(tourinit PRIVATE tourmaline::tooling)
set_target_properties(tourinit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
# The imported canonical project pipeline depends on shared runtimes.
tourmaline_stage_sdl3(tourinit)
add_custom_command(TARGET tourinit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:tourinit>"
)
add_custom_command(TARGET tourinit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:tourinit>"
)
