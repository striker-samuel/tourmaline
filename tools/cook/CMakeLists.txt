# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Cook)  # cook.exe: the COOK - artifacts + tour in, cooked artifacts out (GI/cubemaps/LODs/meshlets/de-instance). No pak write; pak.exe assembles.
# Cook as a library (the editor links it and calls cooktool::Run
# directly; the CLI stays a thin user interface over the same code).
# The lib build excludes main() via COOK_LIB_BUILD.
add_library(CookLib STATIC src/main.cpp src/lm_png.cpp src/cube_png.cpp)
target_compile_definitions(CookLib PRIVATE ENGINE_VULKAN COOK_LIB_BUILD)
target_include_directories(CookLib
PRIVATE
${CMAKE_SOURCE_DIR}/external/stb
${CMAKE_SOURCE_DIR}/tools/common
PUBLIC
${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(CookLib
PRIVATE
engine
gltf_import
artifact_cache
volk
OptickCore
)
add_executable(Cook src/main.cpp src/lm_png.cpp src/cube_png.cpp)
target_compile_definitions(Cook PRIVATE ENGINE_VULKAN)
# stb_image_write (cook-time atlas debug PNG, see lm_png.cpp; the same
# vendored header the editor uses for its PNG export).
target_include_directories(Cook PRIVATE ${CMAKE_SOURCE_DIR}/external/stb ${CMAKE_SOURCE_DIR}/tools/common)
target_link_libraries(Cook
PRIVATE
engine
gltf_import
artifact_cache
volk
OptickCore
)
set_target_properties(Cook PROPERTIES
RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
