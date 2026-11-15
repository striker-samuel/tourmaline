// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Byte-identical mirror of gpu_scene_skinning.h::SkinWorkRow.
// This is a staging ABI only; no GPU-scene raster path reads it yet.
// The packed output is a single contiguous buffer of five uint words per
// vertex (float3 position, packed oct normal, packed oct tangent).
struct GpuSceneSkinWorkRow {
    uint4 identity; // instance slot, generation, reserved, reserved
    uint4 palette;  // palette base, joint count, influence base, vertex count
    uint4 vertices; // bind-pose vertex base, compact output base, reserved, reserved
};

#define GPU_SCENE_SKIN_WORK_INVALID 0xffffffffu

// Return false rather than accessing an uninitialized/out-of-range deformed
// vertex. Both arithmetic operations use subtraction to avoid overflow.
bool GpuSceneSkinVertexAddress(GpuSceneSkinWorkRow work,
                               uint bindVertexId,
                               uint outputCapacityVertices,
                               out uint outputWordOffset) {
    outputWordOffset = 0u;
    const uint base = work.vertices.x;
    const uint count = work.palette.w;
    if (bindVertexId < base || bindVertexId - base >= count)
        return false;
    const uint local = bindVertexId - base;
    const uint outputBase = work.vertices.y;
    if (outputBase > outputCapacityVertices ||
        local >= outputCapacityVertices - outputBase)
        return false;
    // A uint word offset can represent up to UINT_MAX / 5 vertices.
    const uint outVertex = outputBase + local;
    if (outVertex > 0x33333333u)
        return false;
    outputWordOffset = outVertex * 5u;
    return true;
}
