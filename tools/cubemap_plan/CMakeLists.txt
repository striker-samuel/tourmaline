# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(CubemapPlan)

# CPU-only cascaded cubemap capture planner (the #338 planning API): loads
# a cooked .geometry, builds the trace-scene BVH, and uses it as the
# octree's occlusion predicate so probes land in open cells only.
add_executable(CubemapPlan main.cpp)

target_compile_definitions(CubemapPlan PRIVATE ENGINE_VULKAN)

target_link_libraries(CubemapPlan
    PRIVATE
        engine
        gltf_import
        volk
        KTX::ktx
)

set_target_properties(CubemapPlan PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to CubemapPlan.exe.
add_custom_command(TARGET CubemapPlan POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:CubemapPlan>"
)
