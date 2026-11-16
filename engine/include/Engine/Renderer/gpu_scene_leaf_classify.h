// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// P1 flat GPU multi-view leaf classifier ABI (#372). The CPU publishes the
// leaf table + per-view frustum planes; the GPU classifies each leaf against
// every active view and writes a per-leaf view mask. This header defines the
// push ABI + view/mask layouts; the dispatch + per-view compaction land in
// the P1 renderer integration. No production consumer yet — the CPU
// SelectPassedInstanceRangesFrame stays authoritative until the compaction
// and safe-overflow policy are in place.

#include <Engine/Core/double_float.h>

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace tour::gpu_scene {

inline constexpr uint32_t LeafClassifyMaxViews = 32; // per-leaf mask width

// HLSL: GpuSceneCullView. Byte-identical to InstanceCullGpuView — one
// origin-relative DoubleFloat3 + six inward planes (128 B).
struct alignas(16) LeafClassifyView {
    math::DoubleFloat3 origin;
    glm::vec4 inwardPlanes[6]{};
};
static_assert(sizeof(LeafClassifyView) == 128);
static_assert(offsetof(LeafClassifyView, inwardPlanes) == 32);

// HLSL: GpuSceneLeafClassifyPush. Three BDA pointers then the counts uint4.
// PACKING RULE (see InstanceCullGpuPush): after an odd run of 8-byte
// pointers the counts uint4 is NOT rounded to 16, so it sits at offset 24.
struct LeafClassifyPush {
    uint64_t leaves = 0;
    uint64_t views = 0;
    uint64_t masks = 0;
    glm::uvec4 counts{0u}; // x leafCount, y viewCount, z/w reserved
};
static_assert(sizeof(LeafClassifyPush) == 40);
static_assert(offsetof(LeafClassifyPush, counts) == 24);

} // namespace tour::gpu_scene
