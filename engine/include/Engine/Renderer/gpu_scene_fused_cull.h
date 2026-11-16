// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// P1 fused multi-view instance cull ABI (#372). Consumes the leaf work queue
// and emits ONE fused verdict per visible instance carrying a per-view mask,
// replacing the four per-cascade cull passes. This header defines the push ABI
// + the fused verdict; the dispatch + downstream work-select wiring are the
// follow-up. No production consumer yet.

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace tour::gpu_scene {

// HLSL: GpuFusedVerdict. x instance slot, y generation, z per-view mask,
// w flags (bit 0 = CastShadow).
struct alignas(16) FusedVerdict {
    glm::uvec4 value{0u};
};
static_assert(sizeof(FusedVerdict) == 16);

// HLSL: GpuSceneFusedCullPush. Six BDA pointers then the counts uint4 (packed
// at offset 48 — see the InstanceCullGpuPush packing note).
struct FusedCullPush {
    uint64_t queue = 0;
    uint64_t members = 0;
    uint64_t instances = 0;
    uint64_t ownerGenerations = 0;
    uint64_t views = 0;
    uint64_t verdicts = 0;
    glm::uvec4 counts{0u}; // x activeLeafCount, y viewCount, z/w reserved
};
static_assert(sizeof(FusedCullPush) == 64);
static_assert(offsetof(FusedCullPush, counts) == 48);

} // namespace tour::gpu_scene
