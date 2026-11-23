// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_cull.hlsli — the shared structs for the PR03 visible-
// binning chain (cull -> prefix -> scatter -> drawbuild). The persistent
// 40-byte instances NEVER move per camera frame; only this per-view
// scratch does. DXC-2018 compatible; 32-bit math only.

#ifndef PLACEMENT_CULL_HLSLI
#define PLACEMENT_CULL_HLSLI

#include "placement_common.hlsli"
#include "placement_constraint.hlsli"
#include "renderview.hlsli"
#include "gpu_platform.hlsli"

// Must match C++ kPlacementMaxProtos.
#define PLACEMENT_MAX_PROTOS 1024

// Cull flag bits (PlacementCullPush.cullFlags + the VS debug tail).
#define PLACEMENT_CULL_FRUSTUM  1
#define PLACEMENT_CULL_ALIVE    2
#define PLACEMENT_CULL_DISTANCE 4

// The bitpacked cull verdict: one bit per instance slot. Cull sets bits
// with InterlockedOr; scatter reads whole words.
#define PLACEMENT_MASK_BITS 32
struct PlacementMaskBuffer {
    uint w[PLACEMENT_MAX_TILE_INSTANCES / PLACEMENT_MASK_BITS];
};

// Must match C++ PlacementBinsGpu. DXC cannot emit atomics on
// STRUCT-ARRAY members through vk::BufferPointer (ICE: "Trying to
// generate a type that we cannot generate"), so the HLSL side is a FLAT
// word view with named word offsets — layout-identical to the C++ struct
// (counts, offsets, appendNext, total, pad[7]).
#define PLACEMENT_BINS_WORDS (3 * PLACEMENT_MAX_PROTOS + 8)
#define PLACEMENT_BINS_COUNT_BASE 0                     // counts[]
#define PLACEMENT_BINS_OFFSET_BASE PLACEMENT_MAX_PROTOS // offsets[]
#define PLACEMENT_BINS_APPEND_BASE (2 * PLACEMENT_MAX_PROTOS) // appendNext[]
#define PLACEMENT_BINS_TOTAL (3 * PLACEMENT_MAX_PROTOS) // total (+ pad[7])
struct PlacementBinsBuffer { uint v[PLACEMENT_BINS_WORDS]; };

// The per-(slot, view) visible-index list: source instance slots,
// grouped per proto bin (offsets[proto]..offsets[proto]+counts[proto]).
struct PlacementVisibleBuffer { uint v[PLACEMENT_MAX_TILE_INSTANCES]; };

// Must match C++ PlacementIndirectCmd (VkDrawIndexedIndirectCommand —
// the gfxfilter GfxIndirectCmd precedent). The CPU seeds the immutable
// halves (indexCount/firstIndex/vertexOffset) per prototype table;
// drawbuild writes the dynamic halves (instanceCount/firstInstance)
// per frame.
struct PlacementIndirectCmd
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct PlacementIndirectBuffer { PlacementIndirectCmd c[PLACEMENT_MAX_PROTOS]; };

// Must match C++ PlacementProtoRow (32 bytes): the cull's conservative
// sphere + species distance and the drawbuild's mesh addressing.
struct PlacementProtoRow
{
    float4 bounds;      // 0  center.xyz, radius.w
    float drawDistance; // 16 (0 = unlimited)
    uint indexCount;    // 20
    uint firstIndex;    // 24
    int vertexOffset;   // 28
};
struct PlacementProtoTableBuffer { PlacementProtoRow rows[PLACEMENT_MAX_PROTOS]; };

// Must match C++ PlacementCullPush (96 bytes, dx-layout).
struct PlacementCullPush
{
    GPU_PTR_READ(PlacementInstanceBuffer) instances; // 0
    GPU_PTR(PlacementCountBuffer) count;        // 8
    GPU_PTR_WRITE(PlacementMaskBuffer) mask;          // 16
    GPU_PTR_WRITE(PlacementBinsBuffer) bins;          // 24 (flat word view)
    GPU_PTR_READ(PlacementFlag2Buffer) alive;       // 32 (0 = skip)
    GPU_PTR_READ(RenderViewBuffer) renderViews;      // 40
    GPU_PTR_READ(PlacementProtoTableBuffer) protoTable; // 48
    uint renderViewIndex;  // 56
    uint cullFlags;         // 60
    uint protoCount;        // 64
    uint padD;              // 68 (was liveCount — the kernels self-bound
                            //      from the count buffer: min(count, capacity))
    uint2 padE;             // 72 (align camPos to 16 in the dx-layout)
    float3 camPos;          // 80 species-distance origin (per-view CPU data)
    float maxInstanceScale; // 92 radius scaling bound (config.maxScale)
};

// ---------------------------------------------------------------------------
// The conservative visibility test (the cull kernel): the per-PROTO
// cooked-mesh sphere (center + radius), scaled by the instance's
// uniform scale, transformed by position — never a made-up guard
// factor. Masked grass is treated conservatively (the sphere is the
// bound; alpha-tested cutout stays inside it). camPos comes from the
// push (per-view CPU data — no matrix-column extraction guesswork).
// NOTE: takes the pieces, NOT the push struct by value — DXC ICEs
// ("Trying to generate a type that we cannot generate") on big push
// structs with BufferPointer members as value parameters.
// ---------------------------------------------------------------------------
bool PlacementSphereVisible(
    GPU_PTR(RenderViewBuffer) renderViews, uint view,
    uint cullFlags, float4 protoBound, float protoMaxDist, float3 camPos,
    float3 pos, float3 scale)
{
    const float s = max(scale.x, max(scale.y, scale.z));
    const float r = protoBound.w * s;
    const float3 center = pos + protoBound.xyz * s;
    // Species distance (0 = unlimited): measured to the sphere's NEAR
    // edge (conservative — the bound may keep, never wrongly culls).
    if ((cullFlags & PLACEMENT_CULL_DISTANCE) != 0 && protoMaxDist > 0.0f)
    {
        float d = length(center - camPos) - r;
        if (d > protoMaxDist)
            return false;
    }
    if ((cullFlags & PLACEMENT_CULL_FRUSTUM) != 0)
    {
        [unroll]
        for (uint p = 0; p < 6; ++p)
        {
            float4 plane = renderViews.Get().views[view].m_viewPlanes[p];
            if (dot(plane.xyz, center) + plane.w < -r)
                return false;
        }
    }
    return true;
}

#endif
