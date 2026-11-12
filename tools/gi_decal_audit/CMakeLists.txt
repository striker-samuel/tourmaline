# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(GiDecalAudit)

# CPU-only diagnostic: classify a cooked .geometry's meshes by name
# substring + material alphaMode/castsShadows (see main.cpp) to audit
# what enters the GI bake BVH as a solid occluder.
add_executable(GiDecalAudit main.cpp)

target_compile_definitions(GiDecalAudit PRIVATE ENGINE_VULKAN)

target_link_libraries(GiDecalAudit
    PRIVATE
        engine
        gltf_import
        volk
        KTX::ktx
)

set_target_properties(GiDecalAudit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to GiDecalAudit.exe.
add_custom_command(TARGET GiDecalAudit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:GiDecalAudit>"
)
