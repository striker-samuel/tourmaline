# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(AssetTests)

add_executable(AssetTests test_assets.cpp)

target_link_libraries(AssetTests
    PRIVATE
        engine
)

set_target_properties(AssetTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME asset_tests COMMAND AssetTests)

# .tourAnim loader/sampler + the Cluster pose driver (headless).
add_executable(TourAnimTests test_touranim.cpp)

target_link_libraries(TourAnimTests
    PRIVATE
        engine
)

set_target_properties(TourAnimTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME touranim_tests COMMAND TourAnimTests)

# Knight walk-clip end-to-end on the REAL derived assets (guarded: SKIPs
# when F:/content/demo/knight_only is absent): .geometry rebuild + clip
# attach + the follow diagnostic (the hip chain must ride the pelvis).
add_executable(KnightAnimTests test_knight_anim.cpp)

target_link_libraries(KnightAnimTests
    PRIVATE
        engine
        gltf_import
)

set_target_properties(KnightAnimTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME knight_anim_tests COMMAND KnightAnimTests)

# Placement editor model (engine-only TU: no ImGui, no EditorApp).
add_executable(PlacementEditorTests test_placement_editor.cpp
    ../editor/src/editor_dg.cpp
    ../editor/src/editor_placement.cpp
    ../editor/src/editor_placement_dg.cpp
)

target_include_directories(PlacementEditorTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/editor/src
)

target_link_libraries(PlacementEditorTests
    PRIVATE
        engine
)

set_target_properties(PlacementEditorTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME placement_editor_tests COMMAND PlacementEditorTests)

# Maya-style DG core + the placement schemas on top of it (editor-only
# TUs: schema registry, plug semantics, dirty propagation, named-attr
# serialization).
add_executable(DgTests test_dg.cpp
    ../editor/src/editor_dg.cpp
    ../editor/src/editor_placement.cpp
    ../editor/src/editor_placement_dg.cpp
)

target_include_directories(DgTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/editor/src
)

target_link_libraries(DgTests
    PRIVATE
        engine
)

set_target_properties(DgTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME dg_tests COMMAND DgTests)

add_executable(SpirvReflectionTests test_spirv_reflection.cpp)

target_link_libraries(SpirvReflectionTests
    PRIVATE
        engine
)

set_target_properties(SpirvReflectionTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME spirv_reflection_tests COMMAND SpirvReflectionTests)

add_executable(GfxFilterTests test_gfxfilter.cpp)

target_link_libraries(GfxFilterTests
    PRIVATE
        engine
)

set_target_properties(GfxFilterTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME gfxfilter_tests COMMAND GfxFilterTests)

add_executable(ExposureTests test_exposure.cpp)

target_link_libraries(ExposureTests
    PRIVATE
        engine
)

set_target_properties(ExposureTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME exposure_tests COMMAND ExposureTests)

add_executable(LightTests test_light.cpp)

target_link_libraries(LightTests
    PRIVATE
        engine
)

set_target_properties(LightTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME light_tests COMMAND LightTests)

add_executable(CameraTests test_camera.cpp)

target_link_libraries(CameraTests
    PRIVATE
        engine
)

set_target_properties(CameraTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME camera_tests COMMAND CameraTests)

add_executable(SmaaJitterTests test_smaajitter.cpp)

target_link_libraries(SmaaJitterTests
    PRIVATE
        engine
)

set_target_properties(SmaaJitterTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME smaajitter_tests COMMAND SmaaJitterTests)

add_executable(SsrTaaTests test_ssrtaa.cpp)

target_link_libraries(SsrTaaTests
    PRIVATE
        engine
)

set_target_properties(SsrTaaTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME ssrtaa_tests COMMAND SsrTaaTests)

add_executable(GiTests test_gi.cpp)

target_link_libraries(GiTests
    PRIVATE
        engine
)

set_target_properties(GiTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME gi_tests COMMAND GiTests)

add_executable(StreamingTests test_streaming.cpp)

target_link_libraries(StreamingTests
    PRIVATE
        engine
)

set_target_properties(StreamingTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME streaming_tests COMMAND StreamingTests)

add_executable(ProfilerTests test_profile.cpp)

target_link_libraries(ProfilerTests
    PRIVATE
        engine
)

set_target_properties(ProfilerTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME profile_tests COMMAND ProfilerTests)

add_executable(BakeAssetTests test_bake_assets.cpp)

target_link_libraries(BakeAssetTests
    PRIVATE
        engine
)

set_target_properties(BakeAssetTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME bake_asset_tests COMMAND BakeAssetTests)

add_executable(DeferredShadowTests test_deferredshadow.cpp)

target_link_libraries(DeferredShadowTests
    PRIVATE
        engine
)

set_target_properties(DeferredShadowTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME deferredshadow_tests COMMAND DeferredShadowTests)

add_executable(CubemapTests test_cubemap.cpp)

target_link_libraries(CubemapTests
    PRIVATE
        engine
)

set_target_properties(CubemapTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME cubemap_tests COMMAND CubemapTests)

add_executable(ParticlesTests test_particles.cpp)

target_link_libraries(ParticlesTests
    PRIVATE
        engine
)

set_target_properties(ParticlesTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME particles_tests COMMAND ParticlesTests)

add_executable(MotionBlurTests test_motionblur.cpp)

target_link_libraries(MotionBlurTests
    PRIVATE
        engine
)

set_target_properties(MotionBlurTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME motionblur_tests COMMAND MotionBlurTests)

project(TourTests)

add_executable(TourTests test_tour.cpp)

target_link_libraries(TourTests
    PRIVATE
        engine
        gltf_import
)

set_target_properties(TourTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME tour_tests COMMAND TourTests)

# Asset-browser ops (editor-only TU, ImGui-free: delete planning rules).
project(AssetOpsTests)

add_executable(AssetOpsTests test_asset_ops.cpp
    ../editor/src/editor_asset_ops.cpp
)

target_include_directories(AssetOpsTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/editor/src
)

target_link_libraries(AssetOpsTests
    PRIVATE
        engine
        gltf_import
)

set_target_properties(AssetOpsTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME asset_ops_tests COMMAND AssetOpsTests)

project(CacheTests)

add_executable(CacheTests test_cache.cpp)

target_link_libraries(CacheTests
    PRIVATE
        artifact_cache
        engine
        gltf_import
)

set_target_properties(CacheTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME cache_tests COMMAND CacheTests)

# Cluster unload + dynamic-cluster contracts (GPU-less value worlds).
add_executable(WorldEntityTests test_world_entities.cpp)

target_link_libraries(WorldEntityTests
    PRIVATE
        engine
)

set_target_properties(WorldEntityTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME world_entity_tests COMMAND WorldEntityTests)

add_executable(PlacementTests test_placement.cpp)

target_link_libraries(PlacementTests
    PRIVATE
        engine
)

set_target_properties(PlacementTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME placement_tests COMMAND PlacementTests)

add_executable(MemoryTests test_memory.cpp)

target_link_libraries(MemoryTests
    PRIVATE
        engine
)

set_target_properties(MemoryTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME memory_tests COMMAND MemoryTests)

# Push-constant mirror contract (C++ Renderer.h <-> HLSL push_constants.hlsli).
# Compile-time static_asserts catch C++ drift; this pins every offset at
# runtime too. CI additionally runs tools/push_constants_idl/codegen.py --check.
add_executable(PushConstantLayoutTests test_push_constants.cpp)

target_link_libraries(PushConstantLayoutTests
    PRIVATE
        engine
)

set_target_properties(PushConstantLayoutTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME push_constant_tests COMMAND PushConstantLayoutTests)

# Central run-config contract (TOURMALINE_* parse/log/warn).
add_executable(RunConfigTests test_runconfig.cpp)

target_link_libraries(RunConfigTests
    PRIVATE
        engine
)

set_target_properties(RunConfigTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME runconfig_tests COMMAND RunConfigTests)

# Frame-renderer contracts (overlay-scratch discipline, mesh eligibility,
# GPU-driven defaults). GPU-less; headless goldens via tools/golden_capture.py.
add_executable(RendergraphContractTests test_rendergraph_contract.cpp)

target_link_libraries(RendergraphContractTests
    PRIVATE
        engine
)

set_target_properties(RendergraphContractTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME rendergraph_contract_tests COMMAND RendergraphContractTests)

# Tiled-terrain RG16 packing contract (GPU-less, header-only).
add_executable(TerrainPackTests test_terrain_pack.cpp)

target_link_libraries(TerrainPackTests
    PRIVATE
        engine
)

set_target_properties(TerrainPackTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME terrain_pack_tests COMMAND TerrainPackTests)

# Tiled-terrain CPU height-field (sampling + meta JSON contract).
add_executable(TerrainHFieldTests test_terrain_hfield.cpp)

target_link_libraries(TerrainHFieldTests
    PRIVATE
        engine
)

set_target_properties(TerrainHFieldTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME terrain_hfield_tests COMMAND TerrainHFieldTests)

# G-block bake contract (counts, winding, skirts, AABB, mask color).
add_executable(TerrainBlocksTests test_terrain_blocks.cpp)

target_link_libraries(TerrainBlocksTests
    PRIVATE
        engine
)

set_target_properties(TerrainBlocksTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME terrain_blocks_tests COMMAND TerrainBlocksTests)

# Level blob contract (PakAssetKind::Level composite: build/parse/pak carry).
add_executable(LevelTests test_level.cpp)

target_link_libraries(LevelTests
    PRIVATE
        engine
)

set_target_properties(LevelTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME level_tests COMMAND LevelTests)

# Unreal-style env volumes (coverage, priority, tour replay).
add_executable(EnvVolumeTests test_env_volumes.cpp)

target_link_libraries(EnvVolumeTests
    PRIVATE
        engine
)

set_target_properties(EnvVolumeTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME env_volume_tests COMMAND EnvVolumeTests)

# Randomized soak for the deferred-destruction retirement system (mesh heap
# ranges, bindless slots, the deferred-task drain) on a REAL headless Vulkan
# context - the fuzzer plays the swapchain's role in the retirement contract
# with its own timeline semaphore (see the file header). Needs engine/src +
# VMA includes for the impl detail header, volk + KTX (PRIVATE on engine).
add_executable(RetirementFuzzTests test_retirement_fuzz.cpp)

target_include_directories(RetirementFuzzTests
	PRIVATE
		${CMAKE_SOURCE_DIR}/engine/src
		${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator
)

target_link_libraries(RetirementFuzzTests
	PRIVATE
		engine
		volk
		KTX::ktx
)

set_target_properties(RetirementFuzzTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME retirement_fuzz_tests COMMAND RetirementFuzzTests)

# Pak-viewer HOST regressions (finding #19: the CLI/pak-resolution/
# corruption surface had no direct coverage): CPU-only, headless — the
# app-host TUs (PakAppArgs.cpp + PakPreload.cpp + PakWorldLoader.cpp,
# whose ApplyCookedStaticFlags seam is driven on hand-built value
# worlds) compile straight into the test against the engine lib.
add_executable(PakHostTests test_pak_host.cpp
	../app/src/PakAppArgs.cpp
	../app/src/PakPreload.cpp
	../app/src/PakWorldLoader.cpp
)

target_include_directories(PakHostTests
	PRIVATE
		${CMAKE_SOURCE_DIR}/app/src
)

target_link_libraries(PakHostTests
	PRIVATE
		engine
)

set_target_properties(PakHostTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME pak_host_tests COMMAND PakHostTests)

# The canonical PROJECT IMPORT pipeline (project_pipeline): copy-in +
# derive + validate + publish, the import fingerprint (settings/
# revision invalidation), idempotent + source-change reimport, failure
# retention, relocation. CPU-only.
add_executable(ProjectPipelineTests test_project_pipeline.cpp)

target_link_libraries(ProjectPipelineTests
	PRIVATE
		project_pipeline
		gltf_import
		engine
)

set_target_properties(ProjectPipelineTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME project_pipeline_tests COMMAND ProjectPipelineTests)

# SDL3 is linked PUBLIC via engine: stage the DLL next to each test exe.
tourmaline_stage_sdl3(AssetTests)
tourmaline_stage_sdl3(SpirvReflectionTests)
tourmaline_stage_sdl3(GfxFilterTests)
tourmaline_stage_sdl3(ExposureTests)
tourmaline_stage_sdl3(LightTests)
tourmaline_stage_sdl3(CameraTests)
tourmaline_stage_sdl3(SmaaJitterTests)
tourmaline_stage_sdl3(SsrTaaTests)
tourmaline_stage_sdl3(GiTests)
tourmaline_stage_sdl3(StreamingTests)
tourmaline_stage_sdl3(DeferredShadowTests)
tourmaline_stage_sdl3(CubemapTests)
tourmaline_stage_sdl3(ParticlesTests)
tourmaline_stage_sdl3(MotionBlurTests)
tourmaline_stage_sdl3(TourTests)
tourmaline_stage_sdl3(KnightAnimTests)
tourmaline_stage_sdl3(AssetOpsTests)
tourmaline_stage_sdl3(WorldEntityTests)
tourmaline_stage_sdl3(PlacementTests)
tourmaline_stage_sdl3(PlacementEditorTests)
tourmaline_stage_sdl3(MemoryTests)
tourmaline_stage_sdl3(PushConstantLayoutTests)
tourmaline_stage_sdl3(RunConfigTests)
tourmaline_stage_sdl3(RendergraphContractTests)
tourmaline_stage_sdl3(TerrainPackTests)
tourmaline_stage_sdl3(TerrainHFieldTests)
tourmaline_stage_sdl3(TerrainBlocksTests)
tourmaline_stage_sdl3(LevelTests)
tourmaline_stage_sdl3(EnvVolumeTests)
tourmaline_stage_sdl3(RetirementFuzzTests)
tourmaline_stage_sdl3(PakHostTests)
tourmaline_stage_sdl3(ProjectPipelineTests)
