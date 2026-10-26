// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include <Engine/Core/Limits.h>

// RHI-OWNED BINDLESS/DESCRIPTOR BUDGETS. These sizes genuinely belong to
// the RHI: the Vulkan backend validates them against device limits
// (update-after-bind descriptor caps + the aggregate pool budget) when
// it creates the bindless descriptor sets, and the DXIL bounded-table
// form in common.hlsli sizes its register tables from the same values.
//
// Single source of the VALUES: engine/shaders/engine_limits.hlsli (via
// the Core/Limits.h bridge) — CPU allocation, descriptor layout and the
// shader tables must move together. Renderer-owned budgets (materials,
// runtime lights) live in Engine/Renderer/RendererLimits.h instead: the
// RHI never needs to know a material or a light exists.
namespace smvk::limits {

// Bindless descriptor budget for sampled/storage image arrays. Must
// cover the full ShardedPool index space (16 shards * 512 = 8192):
// sharded handle indices double as descriptor-array slots in the
// bindless pools (the pool/descriptor integration static_assert lives
// in Vulkan/detail/vulkan_impl.h).
inline constexpr uint32_t MaxTextures = engine::limits::MaxTextures;

// 3D volume mirror binding (kBinding_VolumeTextures) reuses the SAME
// slot space as MaxTextures (one mirror write per 3D view), so it needs
// the same array bound.
inline constexpr uint32_t MaxVolumeTextures = engine::limits::MaxVolumeTextures;

// Sampler budget + the cross-backend slot contract: 0 = linear-repeat
// default, 1 = shadow comparison (LessEqual), 2 = linear clamp-to-edge;
// shaders + baked materials hardcode samplers[0/1/2] and Initialize()
// populates + VERIFIES the order (misorder is a hard error).
inline constexpr uint32_t MaxSamplers = engine::limits::MaxSamplers;

} // namespace smvk::limits
