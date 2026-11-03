# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(AssetTests)

add_executable(EditorGameSessionTests test_editor_game_session.cpp)
target_link_libraries(EditorGameSessionTests PRIVATE tourmaline::editor_entry)
set_target_properties(EditorGameSessionTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME editor_game_session_tests COMMAND EditorGameSessionTests)

# Headless editor bake progress parser and live-file tee regression.
add_executable(EditorBakeFeedbackTests test_editor_bake_feedback.cpp)
set_target_properties(EditorBakeFeedbackTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME editor_bake_feedback_tests COMMAND EditorBakeFeedbackTests)

add_executable(AssetTests test_assets.cpp)

target_link_libraries(AssetTests
    PRIVATE
        engine
)

set_target_properties(AssetTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME asset_tests COMMAND AssetTests)

# The compile-time asset-query engine (Engine/Asset/asset_query.h -
# MatchAssets' regex core over a synthetic generated-catalog shape;
# compile-time pins via static_assert + the runtime pattern matrix).
add_executable(AssetQueryTests test_asset_query.cpp)

target_include_directories(AssetQueryTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/include
)

set_target_properties(AssetQueryTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME asset_query_tests COMMAND AssetQueryTests)

# The content registry (Engine/Asset/content_registry.h — the
# consolidated content architecture): the CONVERGENCE CONTRACT (the
# id form + the path form resolve the same effective provider),
# override precedence + toggling, both byte lanes (pager + adopted
# backend), the corrupt-owner diagnostics and the kind gates.
# No GPU, no window: hand-built paks + a hand-written .tapp.
add_executable(ContentRegistryTests test_content_registry.cpp)

target_link_libraries(ContentRegistryTests
    PRIVATE
        engine
)

set_target_properties(ContentRegistryTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME content_registry_tests COMMAND ContentRegistryTests)

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

# The .audiograph authored-format parser + the miniaudio instance
# engine (deviceless, deterministic offline rendering — no device, no
# thread, no wall clock).
add_executable(AudioGraphTests test_audiograph.cpp)

target_link_libraries(AudioGraphTests
    PRIVATE
        engine
)

# The world-bus raw-isolation diagnostics include miniaudio directly
# (the same configuration wrapper the engine TUs use).
target_include_directories(AudioGraphTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/src/Audio
        ${CMAKE_SOURCE_DIR}/external/miniaudio
)

set_target_properties(AudioGraphTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME audiograph_tests COMMAND AudioGraphTests)

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
# The content under F:/content/demo/knight_only lives on the author's
# scratch disk (derived from the knight glTF, too large to carry in
# git) — the test exits 77 WITHOUT it so ctest reports "Skipped", never
# a green pass. A green run on CI means the content WAS present.
set_tests_properties(knight_anim_tests PROPERTIES SKIP_RETURN_CODE 77)

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

# The multi-format MESH-IMPORT lane (LoadModelFromSourceFile +
# ImportMeshSource): the format gate (.gltf/.glb/.obj), the GLB binary
# load (the old ASCII-only parse failed every .glb), EMBEDDED-texture
# extraction (GLB bufferView images + glTF data: URIs -> sidecar files
# the material/digest/staging machinery addresses like authored ones),
# and the OBJ adapter end-to-end (geometry, MTL map refs, .mtl digest
# dependency, missing-library refusal). CPU-only (offline-mode
# derivations into temp dirs).
add_executable(SourceImportTests test_source_import.cpp)

target_include_directories(SourceImportTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/external/stb
)

target_link_libraries(SourceImportTests
    PRIVATE
        gltf_import
        engine
)

set_target_properties(SourceImportTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME source_import_tests COMMAND SourceImportTests)

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

# The namespace/RHI BOUNDARY SCANNER (todo item 8, section 8c - the
# regex early-warning v1+): walks the engine's public headers, GPU-free
# and source-tree-only, failing on NEW boundary violations (Vulkan
# includes, private-impl includes, typed Vk handles in public code) -
# and, since the boundary-ownership gate landed, the audit's confirmed
# findings are no longer invisible: the broad `using namespace
# ::smvk` imports (R6), the smvk::->tour:: compat re-export blocks
# (R7) and the RHI implementation-state exposure shapes (R8:
# App's RHI getters/members, ElementFrame's Framebuffer*, the raw
# RHI pointer types in renderer headers) are all BASELINE-GATED with
# exact, shrink-only exception lists: growth or a new site fails,
# and retirement fails until the baseline row is deleted in the same
# commit. The AST/signature-walking scanner remains the follow-up
# (detecting engine types declared DIRECTLY inside namespace smvk
# still needs it). Runs everywhere CI does (no GPU, no window).
add_executable(ArchitectureTests test_architecture.cpp)
target_compile_definitions(ArchitectureTests PRIVATE
    TOURMALINE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

target_link_libraries(SceneTableTests
    PRIVATE
        engine
)

set_target_properties(SceneTableTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME scene_table_tests COMMAND SceneTableTests)

# The boundary scanner: a CI architecture check (todo 8.8c) - GPU-free,
# so it runs on every matrix runner the suite does. The shrink-only
# baselines (R6/R7/R8) are part of the contract: retiring compat surface
# REQUIRES deleting its baseline row in the same commit.
add_test(NAME architecture_tests COMMAND ArchitectureTests)

# ---------------------------------------------------------------------------
# THE HEADER SMOKE TEST (todo item 8, section 8a): every PUBLIC engine
# header must compile STANDALONE - one generated TU per header, no PCH,
# nothing else included. Catches include-what-you-use breaks (a header
# relying on transitive includes - the camera.h/core.h finding's class)
# at build time instead of at the consumer that trips on it.
#
# GPU-free, compile-only: an OBJECT library of per-header TUs (an
# executable would need a main; the object lib still type-checks every
# header). It is NOT part of the default 'all' target (EXCLUDE_FROM_ALL
# - 100+ TUs would slow every incremental build); CI and the dedicated
# target build it explicitly:
#   cmake --build <build> --target HeaderSmoke
# and ctest runs the (trivial) marker so the suite reports it.
# ---------------------------------------------------------------------------
set(HEADER_SMOKE_DIR "${CMAKE_CURRENT_BINARY_DIR}/header_smoke")
file(REMOVE_RECURSE "${HEADER_SMOKE_DIR}")
file(MAKE_DIRECTORY "${HEADER_SMOKE_DIR}")

file(GLOB_RECURSE _smoke_headers
    "${CMAKE_SOURCE_DIR}/engine/include/Engine/*.h")
set(_smoke_srcs "")
foreach(_hdr ${_smoke_headers})
    # Skip the impl-adjacent + generated headers: vulkan_opaque.h etc.
    # are included from implementation contexts only; engine_limits is
    # the CPU/HLSL contract consumed via Limits.h.
    get_filename_component(_name "${_hdr}" NAME)
    if(_name STREQUAL "engine_limits.hlsli.h")
        continue()
    endif()
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}/engine/include" "${_hdr}")
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _tu "${_rel}")
    set(_src "${HEADER_SMOKE_DIR}/${_tu}.cpp")
    file(WRITE "${_src}" "#include <${_rel}>\n// (generated header smoke TU - see tests/CMakeLists.txt)\nint ${_tu}_marker() { return 0; }\n")
    list(APPEND _smoke_srcs "${_src}")
endforeach()

add_library(HeaderSmoke OBJECT ${_smoke_srcs})
# The engine target's INTERFACE include paths (glm, SDL3, spirv-reflect,
# ...) - the headers legitimately use these third-party deps; the pure
# "no RHI include dirs" form of the smoke test lands with the tour/smvk
# directory split (todo 8.7). Vulkan stays PRIVATE on the engine (stage
# 2), so nothing RHI-only propagates here.
target_link_libraries(HeaderSmoke PRIVATE engine)
# No PCH, no extra defines: the headers must stand on their own.
target_compile_features(HeaderSmoke PRIVATE cxx_std_20)
# In the DEFAULT build (not EXCLUDE_FROM_ALL): the ctest entry would
# otherwise spawn a compiler at TEST time - from a test runner without
# the MSVC environment (plain ctest vs a vcvars shell) the STL includes
# are missing and the entry flakes. In the default build the objects
# exist before ctest in EVERY workflow, and the TUs only rebuild when
# the headers change - which is exactly when the check should run.

add_custom_target(HeaderSmokeCheck
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target HeaderSmoke --config $<CONFIG>
    COMMAND echo "header smoke: all public headers compiled standalone"
)
add_test(NAME header_smoke
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target HeaderSmoke --config $<CONFIG>)

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

# CPU-only deterministic gameplay RNG contract (no Vulkan/window setup).
add_executable(RandomTests test_random.cpp)
target_link_libraries(RandomTests PRIVATE engine)
set_target_properties(RandomTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME random_tests COMMAND RandomTests)

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
	../app/src/ProjectRuntime.cpp
	../app/src/ProjectScenes.cpp
	../app/src/AssetStore.cpp
	../app/src/AssetManager.cpp
	../app/src/DeployCheck.cpp
	# Runtime.cpp's Context lazily builds the audio-graph service
	# (AudioGraphs) — the ctor/dtor live here, not the apphost lib.
	../app/src/AudioGraphs.cpp
	../app/src/Runtime.cpp
)

target_include_directories(PakHostTests
	PRIVATE
		${CMAKE_SOURCE_DIR}/app/src
)

target_link_libraries(PakHostTests
	PRIVATE
		engine
		# DeployCheck's content-address digest (the pak name hash).
		artifact_cache
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

# The project-scoped native filesystem watcher (Engine/Utils/
# file_watcher): the ignore policy (generated artifacts invisible
# wherever they sit), debounce/settle, add/remove/modify, atomic-save
# renames, reconcile recovery, single-file roots. CPU-only (threads +
# the OS event APIs; no GPU, no window).
add_executable(FileWatcherTests test_file_watcher.cpp)

target_link_libraries(FileWatcherTests
	PRIVATE
		engine
)

set_target_properties(FileWatcherTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME file_watcher_tests COMMAND FileWatcherTests)

# Compiled shaders as first-class pak/VFS assets (PakAssetKind::Shader)
# + the unified VFS-backed shader reader (Engine/Utils/
# shader_library.h): the pak round trip (write -> PakBackend serve ->
# validated read), bytecode detection (SPIR-V vs DXBC vs garbage),
# validation failures, dev-override precedence, named diagnostics.
# CPU-only.
add_executable(ShaderLibraryTests test_shader_library.cpp)

target_link_libraries(ShaderLibraryTests
	PRIVATE
		engine
)

set_target_properties(ShaderLibraryTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME shader_library_tests COMMAND ShaderLibraryTests)

# The watched shader hot-reload service (Engine/Utils/
# shader_hot_reload.h): the in-process DXC recompile (includes +
# variant twins), SPIR-V validation, the byte-identical no-op, the
# LAST-KNOWN-GOOD contract, the .hlsli coarse invalidation + the
# watcher-driven flow. CPU-only (bytes landing — no GPU).
add_executable(ShaderHotReloadTests test_shader_hot_reload.cpp)

target_link_libraries(ShaderHotReloadTests
	PRIVATE
		engine
)

set_target_properties(ShaderHotReloadTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME shader_hot_reload_tests COMMAND ShaderHotReloadTests)

# The executor's parity leg shells out to the stage exes beside this
# exe (the shared build root): legacy BuildProject vs ExecuteBuildPlan
# through the subprocess adapter, byte-compared. They are build
# prerequisites of the test, not runtime finds.
add_dependencies(ProjectPipelineTests Cook GiBaker Pak)

# The BUNDLED-PROJECT END-TO-END ACCEPTANCE GATE (CPU-only): the
# checked-in samples/04_project_game content driven through the
# CONTRIBUTOR flow — Project.exe build (the real CLI + the real
# subprocess stage adapter: Cook/GiBaker/Pak spawn exactly as on a
# contributor's machine) cooks + publishes the content-addressed paks
# + the game.tapp manifest; then the deployment halves run headless
# IN-PROCESS: DeployCheck (pak VerifyAll + content-addressed digest)
# over every published pak, the deployed catalog fingerprint vs the
# runtime fold (the compiled-catalog contract GameProject.exe checks at
# boot), every VFS-servable [ids] row reading its bytes, and a scene
# load through the public clusterLoader seam. The GameProject EXE
# itself compiles in every top-level configure (CI build verification
# covers that leg); its GPU launch stays dev-side (golden_capture).
add_executable(ProjectE2eTests test_project_e2e.cpp
	../app/src/ProjectRuntime.cpp
	../app/src/ProjectScenes.cpp
	# PakWorldLoader.cpp owns PostLoadTail (the post-load tail every
	# scene load runs — anim attach, static flags, the resolve fold).
	../app/src/PakWorldLoader.cpp
	../app/src/PakPreload.cpp
	../app/src/DeployCheck.cpp
)

target_include_directories(ProjectE2eTests
	PRIVATE
		${CMAKE_SOURCE_DIR}/app/src
)

target_link_libraries(ProjectE2eTests
	PRIVATE
		engine
		artifact_cache
)

set_target_properties(ProjectE2eTests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME project_e2e_tests COMMAND ProjectE2eTests)

# Project.exe (the CLI under test) + the stage tools it spawns are
# build prerequisites, not runtime finds.
add_dependencies(ProjectE2eTests Project Cook GiBaker Pak)

# SDL3 is linked PUBLIC via engine: stage the DLL next to each test exe.
tourmaline_stage_sdl3(AssetTests)
tourmaline_stage_sdl3(ProjectE2eTests)
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
tourmaline_stage_sdl3(AudioGraphTests)
tourmaline_stage_sdl3(SourceImportTests)
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
tourmaline_stage_sdl3(ContentRegistryTests)
tourmaline_stage_sdl3(ProjectPipelineTests)
tourmaline_stage_sdl3(ProbePremixGpuTests)
tourmaline_stage_sdl3(RhiUploadBarrierGpuTests)

# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(PlacementGpuTests PRIVATE vulkan_headers_iface)
endif()
# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(ProbePremixGpuTests PRIVATE vulkan_headers_iface)
endif()
# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(RhiUploadBarrierGpuTests PRIVATE vulkan_headers_iface)
endif()
# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(WorldFieldsGpuTests PRIVATE vulkan_headers_iface)
endif()

# GPU-scene high/low position transport and precision-safe rebasing.
# Headless: no renderer or Vulkan device required at runtime.
add_executable(DoubleFloatTests test_doublefloat.cpp)
target_link_libraries(DoubleFloatTests PRIVATE engine)
set_target_properties(DoubleFloatTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME doublefloat_tests COMMAND DoubleFloatTests)

# GPU-less contracts for opt-in vertex pulling; no renderer or RHI required.
add_executable(VertexPullTests test_vertex_pull.cpp)
target_include_directories(VertexPullTests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
set_target_properties(VertexPullTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME vertex_pull_tests COMMAND VertexPullTests)

# Independently packed optional chart stream mapping; no RHI required.
add_executable(LightmapPullTests test_lightmap_pull.cpp)
target_include_directories(LightmapPullTests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
set_target_properties(LightmapPullTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME lightmap_pull_tests COMMAND LightmapPullTests)

# GPUIR-style persistent GPU scene tables: pure CPU record/journal
# tests with byte-for-byte fake GPU mirrors (no Vulkan device).
add_executable(GpuSceneDatabaseTests test_gpu_scene.cpp)
target_link_libraries(GpuSceneDatabaseTests PRIVATE engine)
set_target_properties(GpuSceneDatabaseTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gpu_scene_database_tests COMMAND GpuSceneDatabaseTests)

# Shared spatial volume trigger semantics (headless).
add_executable(TriggerVolumeTests test_trigger_volume.cpp)
target_link_libraries(TriggerVolumeTests PRIVATE engine)
set_target_properties(TriggerVolumeTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME trigger_volume_tests COMMAND TriggerVolumeTests)

# GPUIR GPU Scene v2: stable World identity / transform-change bridge.
add_executable(GpuSceneWorldTests test_gpu_scene_world.cpp)
target_link_libraries(GpuSceneWorldTests PRIVATE engine)
set_target_properties(GpuSceneWorldTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gpu_scene_world_tests COMMAND GpuSceneWorldTests)

# Agent tooling unit tests (tools/perf_harness.py's stdlib-only suite).
# Registered through ctest so a broken harness (e.g. the literal-\n syntax
# error PR #24 shipped) fails CI like any C++ test would. The harness tests
# locate their target by __file__, so the working directory is irrelevant;
# discovery just needs explicit -s/-t roots. Skipped with a status note when
# no Python3 interpreter exists (the C++ suite never depends on it).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME perf_harness_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tools/tests
            -t ${CMAKE_SOURCE_DIR}/tools/tests)
    # The SMP8/.tapp pak browser's stdlib-only suite (same registration
    # rationale as perf_harness_tests; PR #26 shipped it discover-only).
    add_test(NAME pak_browser_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests
            -t ${CMAKE_SOURCE_DIR}/tests
            -p test_pak_browser.py)
    # The agent CLI tooling suite (PR #60): tourinfo/pixel_compare/
    # input_emulator, stdlib-only, GPU-free.
    add_test(NAME tourinfo_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests -t ${CMAKE_SOURCE_DIR}/tests
            -p test_tourinfo.py)
    # The release-diff and capture CLIs (PR #65): GPU-free regression
    # suites over synthetic packages / a mocked renderer.
    add_test(NAME tourdiff_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests -t ${CMAKE_SOURCE_DIR}/tests
            -p test_tourdiff.py)
    # Project-init scaffolding checks (PR #65): CPU-only, the Project
    # process is mocked; the separate agent-CLI workflow runs the same
    # entry, ctest keeps the local gate honest.
    add_test(NAME project_init_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests -t ${CMAKE_SOURCE_DIR}/tests
            -p test_project_init.py)
    add_test(NAME tourcapture_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests -t ${CMAKE_SOURCE_DIR}/tests
            -p test_tourcapture.py)
    add_test(NAME pixel_compare_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests -t ${CMAKE_SOURCE_DIR}/tests
            -p test_pixel_compare.py)
    add_test(NAME input_emulator_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests -t ${CMAKE_SOURCE_DIR}/tests
            -p test_input_emulator.py)
else()
    message(STATUS "Python3 not found: perf_harness_tests not registered")
endif()

# GPU-free validation of the 2D atlas and stable Z/atlas batching.
add_executable(World2DTests test_world2d.cpp)
target_link_libraries(World2DTests PRIVATE engine)
set_target_properties(World2DTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME world2d_tests COMMAND World2DTests)

# GPU-free prefab live preview propagation/override/undo semantics.
add_executable(PrefabLiveSyncTests test_prefab_live_sync.cpp)
target_link_libraries(PrefabLiveSyncTests PRIVATE engine)
set_target_properties(PrefabLiveSyncTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME prefab_live_sync_tests COMMAND PrefabLiveSyncTests)

# CPU-only authored sprite-set parsing, bounds, grid and animation validation.
add_executable(SpriteSetTests test_spriteset.cpp)
target_link_libraries(SpriteSetTests PRIVATE engine)
set_target_properties(SpriteSetTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME spriteset_tests COMMAND SpriteSetTests)

# CPU-only state machines, navmesh queries and shared graph topology.
add_executable(GameplayAssetTests test_gameplay_assets.cpp)
target_link_libraries(GameplayAssetTests PRIVATE engine)
set_target_properties(GameplayAssetTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gameplay_asset_tests COMMAND GameplayAssetTests)

# GPU-free layout, clipping and painter-order regression checks for game UI.
add_executable(GameUiTests test_game_ui.cpp)
target_link_libraries(GameUiTests PRIVATE engine)
set_target_properties(GameUiTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME game_ui_tests COMMAND GameUiTests)

# GPU-scene mesh/LOD/submesh metadata: streaming and contiguous-span contracts.
add_executable(GpuSceneLodTests test_gpu_scene_lod.cpp)
target_link_libraries(GpuSceneLodTests PRIVATE engine)
set_target_properties(GpuSceneLodTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gpu_scene_lod_tests COMMAND GpuSceneLodTests)

# GPUIR-style persistent spatial leaf IDs, membership and safe bounds.
add_executable(GpuSceneLeafTests test_gpu_scene_leaves.cpp)
target_link_libraries(GpuSceneLeafTests PRIVATE engine)
set_target_properties(GpuSceneLeafTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gpu_scene_leaf_tests COMMAND GpuSceneLeafTests)

# CPU-only GPUIR coarse leaf selection and reference instance culling.
add_executable(GpuSceneCandidatesTests test_gpu_scene_candidates.cpp)
target_link_libraries(GpuSceneCandidatesTests PRIVATE engine)
set_target_properties(GpuSceneCandidatesTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gpu_scene_candidates_tests COMMAND GpuSceneCandidatesTests)

# Opt-in GPU scene instance-culling view + reference verdict parity.
add_executable(GpuSceneValidationTests test_gpu_scene_validation.cpp)
target_link_libraries(GpuSceneValidationTests PRIVATE engine)
set_target_properties(GpuSceneValidationTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME gpu_scene_validation_tests COMMAND GpuSceneValidationTests)

# Native agent-facing tooling library: no renderer/device required.
add_executable(ToolingApiTests test_tooling_api.cpp)
target_link_libraries(ToolingApiTests PRIVATE tourmaline::tooling)
set_target_properties(ToolingApiTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME tooling_api_tests COMMAND ToolingApiTests)


# GPU-free normalized input serialization and deterministic replay.
add_executable(InputTapeTests test_input_tape.cpp)
# CMAKE_SOURCE_DIR, NOT PROJECT_SOURCE_DIR: tests/CMakeLists.txt
# declares project(AssetTests) above, which repoints PROJECT_SOURCE_DIR
# at tests/ (the same trap the engine-include siblings avoid).
target_include_directories(InputTapeTests PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_compile_features(InputTapeTests PRIVATE cxx_std_20)
set_target_properties(InputTapeTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME input_tape_tests COMMAND InputTapeTests)

# CPU-only SDF artifact and pak representation regression.
add_executable(SdfTests test_sdf.cpp)
target_link_libraries(SdfTests PRIVATE engine)
tourmaline_stage_sdl3(SdfTests)
set_target_properties(SdfTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME sdf_tests COMMAND SdfTests)
