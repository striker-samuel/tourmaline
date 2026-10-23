# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(fuzz_sample)

set(SAMPLE_DIR         "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB SAMPLE_SOURCES CONFIGURE_DEPENDS
    ${SAMPLE_DIR}/*.cpp
)

add_executable(03_fuzz ${SAMPLE_SOURCES})

add_custom_command(TARGET 03_fuzz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:03_fuzz>"
)

find_package(Vulkan REQUIRED)

set_target_properties(03_fuzz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_link_libraries(03_fuzz
    PRIVATE
        engine
        KTX::ktx
)

add_custom_command(TARGET 03_fuzz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:03_fuzz>"
)

tourmaline_stage_sdl3(03_fuzz)
