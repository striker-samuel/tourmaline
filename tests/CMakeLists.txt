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

# Forward+ hierarchical culling (FFXVI tiled/zBin phase-1 artifacts on the
# DEFAULT forward+ path): the no-missing-contributor contract — the zBin
# windows are conservative supersets of the cull's exact depth cuts, and no
# light that reaches any point a shaded surface / march sample can occupy
# may lack its coarse-mask bit (the paper's invariant: no missing lights,
# false positives acceptable). Pure CPU.
add_executable(ForwardPlusMaskTests test_forwardplus_masks.cpp)

target_link_libraries(ForwardPlusMaskTests
    PRIVATE
        engine
)

set_target_properties(ForwardPlusMaskTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME forwardplus_mask_tests COMMAND ForwardPlusMaskTests)

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

# Jolt physics smoke test (scene-tables migration Epoch 2): the vendored
# library simulating STANDALONE — links ONLY Jolt, no engine/renderer/
# editor/pak/Scene dependency (the acceptance gate). Stays as the
# permanent headless physics regression when the integration lands.
add_executable(PhysicsJoltTests test_physics_jolt.cpp)

target_link_libraries(PhysicsJoltTests
    PRIVATE
        Jolt
)

set_target_properties(PhysicsJoltTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME physics_jolt_tests COMMAND PhysicsJoltTests)

# Typed scene-table maintenance contract (scene-tables migration
# Epoch 3): insertion/lookup/replacement/removal/swap-and-pop repair/
# rebuild compaction — GPU-less, pure value scenes.
add_executable(SceneTableTests test_scene_tables.cpp)

target_link_libraries(SceneTableTests
    PRIVATE
        engine
)

set_target_properties(SceneTableTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME scene_table_tests COMMAND SceneTableTests)

add_executable(PlacementTests test_placement.cpp)

target_link_libraries(PlacementTests
    PRIVATE
        engine
)

set_target_properties(PlacementTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME placement_tests COMMAND PlacementTests)

# PlacementGpuTests: the DEVICE half of the placement system's Phase-0
# safety baseline — real (hidden-window) Vulkan context driving the
# actual scatter kernels through the rendergraph (swapchain frame), with
# blocking readbacks proving CPU-oracle multiset parity for grid/mesh/
# heightfield, the capacity-guard/overflow contract, and the constraint
# trio's survivor parity. Mirrors the world-fields GPU test's bootstrap
# (volk + hidden window + RM), plus a Swapchain for compute execution
# (the 03_fuzz precedent). CI-excluded like world_fields_gpu_tests
# (hosted runners lack a display/ICD); plain local ctest runs it on a
# real GPU.
add_executable(PlacementGpuTests test_placement_gpu.cpp)

target_link_libraries(PlacementGpuTests
    PRIVATE
        engine
        volk
)

set_target_properties(PlacementGpuTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME placement_gpu_tests COMMAND PlacementGpuTests)

# ProbePremixGpuTests: the DEVICE half of the probe pre-mix module's
# frame discipline - the rendergraph pool-discipline regression
# (AppendPasses must never mint logical handles per frame; the
# pre-fix leak exhausted the 8192-entry buffer pool in ~2 minutes at
# 60fps), the Ensure/Destroy reconfigure discipline, and an execute
# smoke with a live probe entry + selection readback. Mirrors the
# placement GPU test's bootstrap (hidden window + RM + swapchain +
# graph; the shaders load from content/shaders relative to the build
# root). CI-excluded like the other GPU tests (hosted runners lack a
# display/ICD).
add_executable(ProbePremixGpuTests test_probepremix_gpu.cpp)

target_link_libraries(ProbePremixGpuTests
    PRIVATE
        engine
        volk
)

if(TARGET EngineScene_shaders)
    # Ensure() loads content/shaders/probepremix_*.cs.hlsl.spv from the
    # working directory (the build root): never build the test before
    # the shaders exist.
    add_dependencies(ProbePremixGpuTests EngineScene_shaders)
endif()

set_target_properties(ProbePremixGpuTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME probepremix_gpu_tests COMMAND ProbePremixGpuTests)

# RhiUploadBarrierGpuTests: the DEVICE-level regression for the
# split-writer upload hazard (todo.txt's stranded-upload-write class,
# retired from production placement by PR1 and reproduced here
# INTENTIONALLY at the RHI level - independent of the placement
# renderer): a BufferType_Indirect command whose static halves come
# from UploadDataToBuffer and whose dynamic halves a same-frame compute
# pass rewrites, feeding an immediate DrawIndexedIndirect. First-frame
# command + pixel readbacks; the tiny include-free test shaders live in
# engine/shaders (rhitest_*). Mirrors the placement GPU test's
# bootstrap. CI-excluded like the other GPU tests (no display/ICD).
add_executable(RhiUploadBarrierGpuTests test_rhi_upload_barriers_gpu.cpp)

target_link_libraries(RhiUploadBarrierGpuTests
    PRIVATE
        engine
        volk
)

if(TARGET EngineScene_shaders)
    add_dependencies(RhiUploadBarrierGpuTests EngineScene_shaders)
endif()

set_target_properties(RhiUploadBarrierGpuTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME rhi_upload_barriers_gpu_tests COMMAND RhiUploadBarrierGpuTests)

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

# World voxel fields (Engine/WorldFields): virtual addressing, atlas slot
# math, decode packings, pool bookkeeping, residency churn under forced
# eviction, and the paged-atlas == dense-reference seam proof (the
# milestone 1+2 acceptance suite; GPU-side publish path is smoke-tested by
# WorldFieldsGpuTests).
add_executable(WorldFieldsTests test_world_fields.cpp)

target_link_libraries(WorldFieldsTests
    PRIVATE
        engine
)

set_target_properties(WorldFieldsTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME world_fields_tests COMMAND WorldFieldsTests)

# The shared spatial-streaming core (Engine/WorldPartition): world-cell
# addressing, interest flow, the provider frame sequence (synthetic
# cells, no terrain — docs/world_partition.md milestone 1).
add_executable(WorldPartitionTests test_world_partition.cpp)

target_link_libraries(WorldPartitionTests
    PRIVATE
        engine
)

set_target_properties(WorldPartitionTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME world_partition_tests COMMAND WorldPartitionTests)

# The cooked-brick asset form (milestone 4): BC4 codec error bounds, the
# .wfb builder/reader roundtrip, sqrt-density fidelity vs a dense
# reference, and the CookedBrickSource through the paging system.
add_executable(WorldFieldAssetTests test_world_field_assets.cpp)

target_link_libraries(WorldFieldAssetTests
    PRIVATE
        engine
)

set_target_properties(WorldFieldAssetTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME world_field_asset_tests COMMAND WorldFieldAssetTests)

# WorldFieldsGpuTests: the DEVICE half — real (hidden-window) Vulkan
# context, batched region uploads through the streamer, blocking
# readbacks proving atlas bytes/table roundtrip/page-slot semantics
# (milestone 2's GPU acceptance). Mirrors the retirement fuzzer's
# bootstrap needs (volk linked directly, PRIVATE on engine), INCLUDING
# the CI exclusion: hosted runners lack a display (Linux) / a Vulkan ICD
# (Windows), so ci.yml's ctest skips it via -E (see the header there).
# Plain local ctest runs it wherever a real window + GPU exist.
add_executable(WorldFieldsGpuTests test_world_fields_gpu.cpp)

target_link_libraries(WorldFieldsGpuTests
    PRIVATE
        engine
        volk
)

set_target_properties(WorldFieldsGpuTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME world_fields_gpu_tests COMMAND WorldFieldsGpuTests)

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
		# The EDT-01 parity leg: the IN-PROCESS stage adapter (the linked
		# Cook/GiBaker/Pak Runs — exactly what the editor drives) builds
		# the same plan the subprocess adapter does; the paks must be
		# byte-identical.
		CookLib
		GiBakerLib
		PakLib
)

# The stage libs' headers (cook_tool.h/gi_baker.h/pak_tool.h ride their
# PRIVATE include dirs) + the stb headers the test's impl TU needs.
target_include_directories(ProjectPipelineTests
	PRIVATE
		${CMAKE_SOURCE_DIR}/tools/cook/src
		${CMAKE_SOURCE_DIR}/tools/gi_baker
		${CMAKE_SOURCE_DIR}/tools/pak_cooker
		${CMAKE_SOURCE_DIR}/tools/common
		${CMAKE_SOURCE_DIR}/external/stb
)

set_target_properties(ProjectPipelineTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME project_pipeline_tests COMMAND ProjectPipelineTests)

# The executor's parity leg shells out to the stage exes beside this
# exe (the shared build root): legacy BuildProject vs ExecuteBuildPlan
# through the subprocess adapter, byte-compared. They are build
# prerequisites of the test, not runtime finds.
add_dependencies(ProjectPipelineTests Cook GiBaker Pak)

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
tourmaline_stage_sdl3(ForwardPlusMaskTests)
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
tourmaline_stage_sdl3(WorldFieldsTests)
tourmaline_stage_sdl3(WorldFieldsGpuTests)
tourmaline_stage_sdl3(WorldPartitionTests)
tourmaline_stage_sdl3(WorldFieldAssetTests)
tourmaline_stage_sdl3(EnvVolumeTests)
tourmaline_stage_sdl3(RetirementFuzzTests)
tourmaline_stage_sdl3(PakHostTests)
tourmaline_stage_sdl3(ProjectPipelineTests)
tourmaline_stage_sdl3(ProbePremixGpuTests)
tourmaline_stage_sdl3(RhiUploadBarrierGpuTests)
