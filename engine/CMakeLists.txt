project(Engine)

set(ENGINE_BACKEND "Vulkan")



file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
)

add_library(engine STATIC ${ENGINE_SRC})

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/tiny_obj_loader
        ${CMAKE_SOURCE_DIR}/external/tinygltf
        ${nlohmann_json_SOURCE_DIR}/single_include
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
        ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect
        ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Vulkan/detail
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RHI/detail
        ${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator
        ${CMAKE_SOURCE_DIR}/external/streamline/include
        # stb_image/stb_image_write (LutIo PNG readback).
        ${CMAKE_SOURCE_DIR}/external/stb
        # gltf_import helper used by the world renderer
        # (GltfPunctualAimAxis): headers only — the lib itself stays with
        # the executables (gltf_import PUBLIC-links engine, so linking it
        # here would be a dependency cycle).
        ${CMAKE_SOURCE_DIR}/tools/gltf_import/include
)


target_compile_definitions(engine 
    PUBLIC
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
        TINYGLTF_NO_STB_IMAGE
        TINYGLTF_NO_STB_IMAGE_WRITE
        TINYGLTF_NO_EXTERNAL_IMAGE
        GLFW_INCLUDE_NONE
)

if(ENGINE_BACKEND STREQUAL "Vulkan")
    target_compile_definitions(engine PRIVATE ENGINE_VULKAN)
endif()

# ---------------------------------------------------------------------------
# Precompiled Header for the engine (moved with the world renderer from
# appcore). CMake's built-in target_precompile_headers works with both
# MSBuild and Ninja (+ MSVC cl.exe). No manual #include "pch.h" needed:
# CMake injects /Yu + /FI. Disable with -DVULKANTOY_PCH=OFF if the PCH
# ever masks a header bug.
# ---------------------------------------------------------------------------
option(VULKANTOY_PCH "Use precompiled headers for engine" ON)
if(VULKANTOY_PCH)
    target_precompile_headers(engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pch.h
    )
endif()

# Deferred-shadow debug visualizers (paper 5.3/5.4): the world renderer
# moved here from appcore, taking its DS_DEBUG_VIZ-gated FrameUniforms
# tail with it. Same auto logic as before (root CMakeLists resolves
# DS_VIZ_ON: Debug trees unless forced); must match the DXC define or
# the CPU/GPU layout diverges.
if(DS_VIZ_ON)
    target_compile_definitions(engine PRIVATE DS_DEBUG_VIZ=1)
endif()

# Nsight Aftermath shader debug tables (crash symbolication): when this
# tree's SPIR-V carries DXC -Zi debug info (root CMakeLists resolves
# SHADER_DEBUG_INFO_ON: Debug trees / VULKANTOY_SHADER_DEBUG=ON preset),
# the engine's device init additionally probes + enables
# VK_NV_device_diagnostics_config with the shader-debug-info bit chained
# into VkDeviceCreateInfo — the NVIDIA-driver half that maps GPU machine
# code back to SPIR-V so Aftermath crash dumps resolve to HLSL source
# lines. Runtime kill switch: VULKANTOY_AFTERMATH=0.
if(SHADER_DEBUG_INFO_ON)
    target_compile_definitions(engine PRIVATE VULKANTOY_SHADER_DEBUG_INFO=1)
endif()

target_link_libraries(engine
    PUBLIC
        glfw
        SDL3::SDL3
        glm
        imgui_core
        meshoptimizer
        xatlas
        OptickCore
        spirv_reflect
    PRIVATE
        directxtex_bc
        volk
        KTX::ktx
        imgui_backend_glfw
        imgui_backend_sdl3
        imgui_backend_vulkan
        lz4
        winhttp
        bcrypt
)

# DXC -- shader compiling HLSL->SPIRV
set(DXC_DIR "${CMAKE_SOURCE_DIR}/external/dxc")

target_include_directories(engine PRIVATE
    "${DXC_DIR}/include"
)

target_link_libraries(engine PRIVATE
    "${DXC_DIR}/lib/x64/dxcompiler.lib"
)


add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_DIR}/bin/x64/dxcompiler.dll"
        "$<TARGET_FILE_DIR:engine>"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_DIR}/bin/x64/dxil.dll"
        "$<TARGET_FILE_DIR:engine>"
)

# OptickCore builds SHARED upstream -> its DLL must sit next to the exe.
# $TARGET_FILE:OptickCore resolves to the DLL on Windows.
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:engine>"
)

# SDL3 is SHARED (vendored prebuilt) -> its DLL must sit next to the exe.
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:engine>"
)

# ---------------------------------------------------------------------------
# Scene shaders (engine/shaders -> content/shaders/*.spv): the world
# renderer's pipeline set, loaded relative to the working directory
# alongside user content. Owned here since the renderer moved into the
# engine (was App's App_shaders target). Emits the ALL target
# "EngineScene_shaders" over the outputs; App + Editor sync them next to
# their exes POST_BUILD (see app/editor CMakeLists). Do NOT add a second
# ALL target over the same .spv files — two independent custom targets
# writing identical outputs race in MSBuild (MSB8066 "exited with code 1"
# on one of them, ~every shader change).
# ---------------------------------------------------------------------------
compile_shaders(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${CMAKE_BINARY_DIR}/content/shaders"
    EngineScene
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    # Fixed-count lit permutations: the Main filtered leg binds the lit FS
    # matching each material range's cooked lobe count (see GfxRenderer::
    # FilteredLitProgram), so the shader's SG evaluation folds to literals.
    # Output: default_lit.frag.hlsl.sg{5,6,9,12}.spv next to the base module.
    VARIANTS
        "default_lit.frag.hlsl|.sg5|SG_LM_LOBE_COUNT=5"
        "default_lit.frag.hlsl|.sg6|SG_LM_LOBE_COUNT=6"
        "default_lit.frag.hlsl|.sg9|SG_LM_LOBE_COUNT=9"
        "default_lit.frag.hlsl|.sg12|SG_LM_LOBE_COUNT=12"
        # GI permutations (SPEC_GI_VARIANT specialization constant)
        "default_lit.frag.hlsl|.gi0|GI_NONE=1"
        "default_lit.frag.hlsl|.gi1|GI_OCTREE=1"
        "default_lit.frag.hlsl|.gi2|GI_REGULAR=1"
        "default_lit.frag.hlsl|.gi3|GI_FULL=1"
        # Standard-material twins (MAT_STANDARD: no layered machinery —
        # the 3 extra MaterialValues + per-layer texture loops compile
        # out; GfxRenderer binds them for layerMaterials.w <= 1, the
        # monolith keeps layered materials — fail-safe fallback).
        "default_lit.frag.hlsl|.std|MAT_STANDARD=1"
        "default_lit.frag.hlsl|.gi0std|GI_NONE=1|MAT_STANDARD=1"
        "default_lit.frag.hlsl|.gi1std|GI_OCTREE=1|MAT_STANDARD=1"
        "default_lit.frag.hlsl|.gi2std|GI_REGULAR=1|MAT_STANDARD=1"
        "default_lit.frag.hlsl|.gi3std|GI_FULL=1|MAT_STANDARD=1"
        # Mesh-path FS twins (gfxmesh_lit: material from a flat varying):
        # same SG/GI permutations, selected exactly like the indexed leg
        # (see GfxRenderer::MeshProgramFor).
        "gfxmesh_lit.frag.hlsl|.sg5|SG_LM_LOBE_COUNT=5"
        "gfxmesh_lit.frag.hlsl|.sg6|SG_LM_LOBE_COUNT=6"
        "gfxmesh_lit.frag.hlsl|.sg9|SG_LM_LOBE_COUNT=9"
        "gfxmesh_lit.frag.hlsl|.sg12|SG_LM_LOBE_COUNT=12"
        "gfxmesh_lit.frag.hlsl|.gi0|GI_NONE=1"
        "gfxmesh_lit.frag.hlsl|.gi1|GI_OCTREE=1"
        "gfxmesh_lit.frag.hlsl|.gi2|GI_REGULAR=1"
        "gfxmesh_lit.frag.hlsl|.gi3|GI_FULL=1"
        "gfxmesh_lit.frag.hlsl|.std|MAT_STANDARD=1"
        "gfxmesh_lit.frag.hlsl|.gi0std|GI_NONE=1|MAT_STANDARD=1"
        "gfxmesh_lit.frag.hlsl|.gi1std|GI_OCTREE=1|MAT_STANDARD=1"
        "gfxmesh_lit.frag.hlsl|.gi2std|GI_REGULAR=1|MAT_STANDARD=1"
        "gfxmesh_lit.frag.hlsl|.gi3std|GI_FULL=1|MAT_STANDARD=1"
)
