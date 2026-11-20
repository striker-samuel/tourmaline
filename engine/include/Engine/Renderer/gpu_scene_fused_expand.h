// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// P1 production bridge: expand the per-instance fused verdict stream
// (gpu_scene_fused_cull.cs.hlsl, keyed by instance slot + per-view mask) into
// the dense per-candidate InstanceCullVerdict stream that the existing
// compact -> work-select -> raster chain already consumes. One thread per
// instance slot; a slot visible in the target view emits {slot, generation,
// visible=1, bypass=0}, anything else emits the rejected sentinel.

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace tour::gpu_scene {

// HLSL: GpuSceneFusedExpandPush. Two BDA pointers then the counts uint4
// (packed at offset 16 — see the InstanceCullGpuPush packing note).
struct alignas(16) FusedExpandPush {
    uint64_t fused = 0;    // FusedVerdict[instanceSlots] (GPU-written)
    uint64_t verdicts = 0; // InstanceCullVerdict[instanceSlots] (dense output)
    glm::uvec4 counts{0u}; // x instanceCount, y viewBit (1 << viewIndex)
};
static_assert(sizeof(FusedExpandPush) == 32);
static_assert(offsetof(FusedExpandPush, counts) == 16);

} // namespace tour::gpu_scene
