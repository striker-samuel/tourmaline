# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(triangle_sample)

set(SAMPLE_DIR         "${CMAKE_CURRENT_SOURCE_DIR}")
set(SAMPLE_SHADERS_DIR "${SAMPLE_DIR}/content/shaders")
set(SAMPLE_INCLUDE_DIR "${SAMPLE_DIR}/include")

message(STATUS "SAMPLE_DIR = ${SAMPLE_DIR}")
message(STATUS "SAMPLE_SHADERS_DIR = ${SAMPLE_SHADERS_DIR}")
message(STATUS "SAMPLE_INCLUDE_DIR = ${SAMPLE_INCLUDE_DIR}")


file(GLOB SAMPLE_SOURCES CONFIGURE_DEPENDS
    ${SAMPLE_DIR}/*.cpp
)

add_executable(01_triangle ${SAMPLE_SOURCES})

add_custom_command(TARGET 01_triangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:01_triangle>"
)

message(STATUS "Triangle source files: ${SAMPLE_SOURCES}")
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan_FOUND: ${Vulkan_FOUND}")
message(STATUS "Vulkan_INCLUDE_DIRS: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "Vulkan_LIBRARIES: ${Vulkan_LIBRARIES}")

set_target_properties(01_triangle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(01_triangle
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(01_triangle
    PRIVATE
        engine
        engine
        KTX::ktx
)

add_custom_command(TARGET 01_triangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:01_triangle>"
)

tourmaline_stage_sdl3(01_triangle)
