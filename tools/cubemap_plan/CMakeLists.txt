# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(CubemapPlan)

# CPU-only cascaded cubemap capture planner (the #338 planning API) — a
# scoping aid for baking tiered reflection captures. Links only the engine
# lib (BuildOctreeProbePositions + the WorldPartition planning header).
add_executable(CubemapPlan main.cpp)

target_compile_definitions(CubemapPlan PRIVATE ENGINE_VULKAN)

target_link_libraries(CubemapPlan PRIVATE engine)

set_target_properties(CubemapPlan PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
