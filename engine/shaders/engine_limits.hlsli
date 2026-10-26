// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// SINGLE SOURCE OF TRUTH for every engine limit that is a SHARED CPU/HLSL
// LAYOUT CONTRACT: an array bound sized identically on the GPU (shader
// struct below) and the CPU (upload ring stride / buffer allocation).
// Before this file those bounds lived as separate #defines in
// common.hlsli + global constexprs in smvk.h and could drift
// independently; the ABI contract is now ONE text both compilers see.
//
// Consumers:
//   - HLSL: #include "engine_limits.hlsli" (common.hlsli, gfxfilter.hlsli)
//   - C++:  Engine/Core/Limits.h — the ONLY C++ includer. It converts the
//     macros to namespaced constexpr and #undefs everything, so no macro
//     ever leaks into C++ TUs. NEVER include this file from C++ anywhere
//     else.
//
// Macros (not constexpr) because DXC and C++ share only the preprocessor.
// Ownership after conversion (the alias headers are the public surface):
//   MAX_MATERIALS / MAX_SIMULTANEOUS_LIGHTS -> engine::renderer::limits
//     (Engine/Renderer/RendererLimits.h) — renderer-owned table/binding
//     budgets; the RHI does not know materials or lights exist.
//   MAX_TEXTURES / MAX_VOLUME_TEXTURES / MAX_SAMPLERS -> smvk::limits
//     (Engine/RHI/DescriptorLimits.h) — RHI bindless descriptor budgets.
//
// Editing a value here recompiles EVERY shader (the SHADER_HEADERS glob
// dependency in cmake/CompileShaders.cmake) and every C++ TU that
// includes the alias headers — the coarse full-rebuild is the point: a
// layout-contract change must never ship half-applied.

// GPU material-table capacity: MaterialsBuffer.mats[] (common.hlsli) and
// the CPU upload ring (WorldRenderer materialsBuffer, stride
// sizeof(MaterialValues) * MAX_MATERIALS per flight frame; the editor's
// own ring aliases the same value). World::ResolveAndFixup deduplicates
// per (cluster, cluster-local material) because one slot per INSTANCE
// could overflow this — the loud clamp lives in WorldRenderer.
#define MAX_MATERIALS 4096

// RUNTIME SIMULTANEOUS-LIGHT budget: how many lights may be BOUND AT ONCE
// in a frame's light buffers. NOT a per-scene/level limit — worlds own
// any number of light entities; per-frame collection clamps what a
// single frame uploads and binds. Both light paths draw from this one
// value: LightBuffer.lights[] below (the legacy ring — the sun/cascade
// entry lives in lights[0]) and the Forward+ ForwardPlusLight ring +
// its per-tile lists (WorldRenderer's FP_MAX_LIGHTS derives from it).
// 1024 = the Forward+ budget this engine actually runs.
#define MAX_SIMULTANEOUS_LIGHTS 1024

// Bindless descriptor budget for sampled/storage image arrays (bindings
// kBinding_Textures / StorageImages / Cubemaps / MsaaTextures). Must
// cover the full ShardedPool index space (kNumPoolShards *
// kShardCapacity = 16 * 512 = 8192): sharded handle indices double as
// descriptor-array slots in the bindless pools (static_assert at the
// pool/descriptor integration site, vulkan_impl.h — NOT in the generic
// Memory/pool.h, which must not know descriptor budgets).
#define MAX_TEXTURES 8192

// 3D volume mirror binding (kBinding_VolumeTextures) reuses the SAME
// slot space as MAX_TEXTURES (one mirror write per 3D view), so it needs
// the same array bound.
#define MAX_VOLUME_TEXTURES 8192

// Sampler descriptor budget. The slot contract is cross-backend and
// SHADER-VISIBLE: 0 = linear-repeat default, 1 = shadow comparison
// (LessEqual), 2 = linear clamp-to-edge; shaders + baked materials
// hardcode samplers[0/1/2] and Initialize() populates + VERIFIES the
// order (misorder is a hard error).
#define MAX_SAMPLERS 8
