// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include <Engine/Core/Limits.h>

// RENDERER-OWNED GPU BUDGETS: the table/binding capacities the
// renderer's passes allocate and the shaders index. These deliberately
// do NOT live in smvk.h — an RHI must not know materials or lights exist
// (they moved out of the RHI header on purpose). Single source of the
// VALUES: engine/shaders/engine_limits.hlsli (the shared CPU/HLSL layout
// contract — the shader array bounds and the CPU upload strides must
// move together, one text).
namespace engine::renderer::limits {

// GPU material-table capacity: MaterialsBuffer.mats[] (common.hlsli) +
// the CPU upload ring (WorldRenderer materialsBuffer, stride
// sizeof(MaterialValues) * MaxMaterials per flight frame; the editor's
// own ring aliases the same value). Exceeding is a LOUD clamp + stderr
// in WorldRenderer (never a silent OOB GPU read); World::ResolveAndFixup
// deduplicates per (cluster, cluster-local material) because one slot
// per INSTANCE could overflow this.
inline constexpr uint32_t MaxMaterials = engine::limits::MaxMaterials;

// RUNTIME SIMULTANEOUS-LIGHT budget: how many lights may be BOUND AT
// ONCE in a frame's light buffers. NOT a per-scene/level limit — the
// world may own any number of light entities; per-frame collection
// clamps what a single frame uploads and binds. Both light paths derive
// from this one value: the legacy smvk::Light ring (the sun/cascade
// entry, LightBuffer.lights[] in common.hlsli) and the Forward+
// ForwardPlusLight ring + per-tile lists (WorldRenderer's FP_MAX_LIGHTS
// = this budget). 1024 = the Forward+ budget this engine actually runs.
inline constexpr uint32_t MaxSimultaneousLights =
	engine::limits::MaxSimultaneousLights;

} // namespace engine::renderer::limits
