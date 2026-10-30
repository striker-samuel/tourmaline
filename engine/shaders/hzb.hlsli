// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Hierarchical-Z (occlusion) shared structs (Vulkan port).
// C++ mirrors: Engine/Renderer/gfx_filter.h (HzbPush, 48 bytes).
//
// Conventions (must match app + forwardplus.cs.hlsl):
//   - Reversed infinite-Z: depth value 1 = near, 0 = far/sky (clear 0).
//   - Reduction is MIN over the footprint: a meshlet with nearest depth
//     value d_near is hidden iff d_near < texel for every covered texel,
//     which the test can only conclude when texel <= min over the region.
//     (Opposite of standard-Z tutorials, which use MAX for the same reason.)
//     One sky pixel (0) poisons its texel to 0 -> keep. Conservative.
//   - Mip 0 is a 1:1 copy of the depth texture (exact); mips 1+ are MIN of
//     the 2x2 footprint below (clamped at edges).

#pragma once
#include "gpu_platform.hlsli"


// Depth-texture source selector for HzbPush.srcMip (mip 0 build).
#define HZB_SRC_DEPTH 0xFFFFFFFFu

// Covers up to ~4K class targets (3840x2160x4/3 ~= 11M floats).
#define HZB_MAX_FLOATS 12582912
struct HzbBuffer { float texels[HZB_MAX_FLOATS]; };

struct HzbPush
{
    GPU_PTR_WRITE(HzbBuffer) hzb;
    uint srcMip; // HZB_SRC_DEPTH = copy from depth texture (mip 0 build)
    uint dstMip;
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    uint srcOffset; // float offset of src mip in buffer (ignored for depth)
    uint dstOffset; // float offset of dst mip in buffer
    uint depthTextureIndex; // bindless depth (mip 0 build only)
    uint pad;
};

// ---- single-dispatch SPD build (hzb_spd.cs.hlsl) ----
//
// Completion counter for the last-group handoff (mips 7+): a DEDICATED
// BDA buffer, never shared between builds that can overlap (the C++
// side slices a per-frame-in-flight ring). Creation-zero-filled AND
// reset by the last group after its bump — never a recycled garbage
// count (the GPU-counter convention, see AGENTS.md).
struct HzbSpdCounter
{
    uint completed; // groups that published mips 0..6 this dispatch
};

// C++ mirror: Engine/Renderer/gfx_filter.h (HzbSpdPush, 104 bytes, DX
// layout — two 64-bit BDAs lead, four scalars, the 16 mip float
// offsets, then the tail). mipOffsets carries gfx::HzbMipOffsetFloats
// per level (only [0, mipCount) is meaningful); groupCount is the
// dispatch's total workgroups (gx*gy) for the last-group test.
struct HzbSpdPush
{
    GPU_PTR_WRITE(HzbBuffer) hzb;
    GPU_PTR(HzbSpdCounter) counter; // this flight's slice
    uint width; // mip 0 extent (== depth extent)
    uint height;
    uint mipCount; // total levels (1..16)
    uint depthTextureIndex; // bindless depth (the mip 0 copy)
    uint mipOffsets[16]; // float offsets per level
    uint groupCount; // dispatch gx*gy (last-group test)
    uint pad;
};
