// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU SKINNING (the Northlight pipeline's compute box): one workgroup
// per skinned range (mesh), 128 lanes, vertices in strides. Reads:
//   palette  - the per-frame joint matrices (the "GPU bone animation"
//              box's output; the CPU driver builds it per frame - see
//              Cluster::SampleAnimation),
//   skin     - the static per-vertex PackedSkinInfluences pairs (mesh.h binding-2
//              data: RGBA8_UInt joint ids + RGBA8_UNorm weights, top-4),
//   posHeap  - the BIND-POSE positions (the v4 fast-load heap - stays
//              the bind pose forever, the pass's eternal input),
//   shadeHeap- the bind-pose shading words (normal/tangent at words
//              2/3 of each 4-word PackedVertexShading record);
// and WRITES the deformed buffer: 5 words per vertex
//   [0..2] deformed position (asuint floats),
//   [3] deformed packed normal, [4] deformed packed tangent.
// The draw paths (the mesh-shader leg) fetch the deformed buffer for
// skinned draws - one fetch swap, NO per-vertex skin math in the draw
// shaders. The heaps are read-only here (BufferType_Vertex has no
// STORAGE usage - writes go to the deformed BufferType_Storage buffer).
//
// The skin math is mesh.h's binding-2 contract:
//   skinMat = sum(w_i * pal[j_i]) / sum(w_i);
//   pos = skinMat * pos; normal/tangent = mat3(skinMat) * n,
//   renormalized.
// The octahedral ENCODE is the exact HLSL port of the CPU
// vertexpack::PackNormalOct/PackTangentOct (same fold, same sign(0)=+1,
// same round-to-nearest) - keep the two in sync (vertex_pack.h).

#define GFXSKINNING_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"

// Generous declared capacities (the BDA-pointer pattern: the structs
// size the pointer math; the loads stay within the real heaps).
#define SKIN_PALETTE_MAX 256        // mesh.h: the joint-table cap
#define SKIN_STREAM_MAX (64 * 1024 * 1024)
#define SKIN_RANGES_MAX 4096
#define SKIN_DEFORMED_MAX (64 * 1024 * 1024)
#define SKIN_POS_HEAP_MAX (512 * 1024 * 1024 / 4)
#define SKIN_SHADE_HEAP_MAX (512 * 1024 * 1024 / 4)

struct SkinPaletteGpu { float4x4 pal[SKIN_PALETTE_MAX]; };
struct SkinStreamGpu { uint2 pair[SKIN_STREAM_MAX]; };
struct SkinRangesGpu { uint4 row[SKIN_RANGES_MAX]; }; // (heapBase, count, skinBase, pad)
struct SkinDeformedGpu { uint word[SKIN_DEFORMED_MAX]; }; // 5 words/vertex
struct SkinPosHeapGpu { uint word[SKIN_POS_HEAP_MAX]; };  // 3 words/vertex
struct SkinShadeHeapGpu { uint word[SKIN_SHADE_HEAP_MAX]; }; // 4 words/vertex

// Mirrors smvk::skinning::SkinningPush (Rendergraph/skinning.h).
struct SkinningPush
{
    vk::BufferPointer<SkinPaletteGpu> palette;   // 0
    vk::BufferPointer<SkinStreamGpu> skin;        // 8
    vk::BufferPointer<SkinRangesGpu> ranges;       // 16
    vk::BufferPointer<SkinDeformedGpu> deformed;  // 24
    vk::BufferPointer<SkinPosHeapGpu> posHeap;     // 32
    vk::BufferPointer<SkinShadeHeapGpu> shadeHeap; // 40
    uint deformedBase; // 48 the skinned span's heap-id base
    uint paletteCount;  // 52
    uint rangeCount;    // 56
    uint pad0;          // 60
};

[[vk::push_constant]]
ConstantBuffer<SkinningPush> pcS;

// ---------------------------------------------------------------------------
// Octahedral ENCODE (the CPU vertexpack::Pack* port - vertex_pack.hlsli
// only ships the decode side; the skinning pass needs the encode for the
// deformed normal/tangent words). Same fold, same sign(0)=+1, same
// round-to-nearest as Engine/Renderer/vertex_pack.h.
// ---------------------------------------------------------------------------
float2 SkinOctEncode01(float3 n)
{
    float invL1 = 1.0f / (abs(n.x) + abs(n.y) + abs(n.z));
    float2 p = float2(n.x * invL1, n.y * invL1);
    if (n.z < 0.0f) {
        const float tx = (1.0f - abs(p.y)) * ((p.x >= 0.0f) ? 1.0f : -1.0f);
        const float ty = (1.0f - abs(p.x)) * ((p.y >= 0.0f) ? 1.0f : -1.0f);
        p = float2(tx, ty);
    }
    return p;
}

uint SkinOctQuantize16(float v)
{
    const float c = saturate(v * 0.5f + 0.5f);
    return uint(floor(c * 65535.0f + 0.5f));
}

uint SkinOctQuantize15(float v)
{
    const float c = saturate(v * 0.5f + 0.5f);
    return uint(floor(c * 32767.0f + 0.5f));
}

uint SkinPackNormalOct(float3 n)
{
    const float l2 = dot(n, n);
    if (!(l2 > 1e-24f))
        n = float3(0.0f, 0.0f, 1.0f);
    else
        n *= 1.0f / sqrt(l2);
    const float2 p = SkinOctEncode01(n);
    return (SkinOctQuantize16(p.y) << 16) | SkinOctQuantize16(p.x);
}

uint SkinPackTangentOct(float3 t, float w)
{
    const float l2 = dot(t, t);
    if (!(l2 > 1e-24f))
        t = float3(1.0f, 0.0f, 0.0f);
    else
        t *= 1.0f / sqrt(l2);
    const float2 p = SkinOctEncode01(t);
    const uint handed = (w >= 0.0f) ? 1u : 0u; // sign(0) = +1 (the CPU)
    return (SkinOctQuantize15(p.x) << 16)
        | (SkinOctQuantize15(p.y) << 1) | handed;
}

// ---------------------------------------------------------------------------
// The pass: groupId.x = range index; 128 lanes cover the range's
// vertices in strides. Writes the deformed words for every vertex.
// ---------------------------------------------------------------------------
[numthreads(128, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    if (gid.x >= pcS.rangeCount)
        return;
    const uint4 row = pcS.ranges.Get().row[gid.x];
    const uint heapBase = row.x;
    const uint count = row.y;
    const uint skinBase = row.z;

    for (uint v = gtid.x; v < count; v += 128)
    {
        // The skin pair (mesh.h binding-2): RGBA8_UInt joint ids +
        // RGBA8_UNorm weights, top-4 influences.
        const uint2 sw = pcS.skin.Get().pair[skinBase + v];
        const uint jc = sw.x;
        uint4 j = uint4(jc & 0xFFu, (jc >> 8) & 0xFFu,
            (jc >> 16) & 0xFFu, (jc >> 24) & 0xFFu);
        const uint wc = sw.y;
        float4 w8 = float4(wc & 0xFFu, (wc >> 8) & 0xFFu,
            (wc >> 16) & 0xFFu, (wc >> 24) & 0xFFu) * (1.0f / 255.0f);
        float wsum = w8.x + w8.y + w8.z + w8.w;
        if (!(wsum > 1e-6f)) {
            // Degenerate zero weights (the import guard): joint 0 at
            // full weight - never a NaN in the draw fetch.
            j = uint4(0u, 0u, 0u, 0u);
            w8 = float4(1.0f, 0.0f, 0.0f, 0.0f);
            wsum = 1.0f;
        }
        const float inv = 1.0f / wsum;

        // skinMat = sum(w_i * pal[j_i]) / sum(w_i) (mesh.h's contract;
        // the joint ids are guarded to the palette count).
        const uint4 jg = min(j, uint4(pcS.paletteCount - 1u, pcS.paletteCount - 1u, pcS.paletteCount - 1u, pcS.paletteCount - 1u));
        float4x4 m = w8.x * pcS.palette.Get().pal[jg.x]
            + w8.y * pcS.palette.Get().pal[jg.y]
            + w8.z * pcS.palette.Get().pal[jg.z]
            + w8.w * pcS.palette.Get().pal[jg.w];
        m *= inv;

        const uint vi = heapBase + v; // the global heap vertex id
        // Bind-pose pos (3 words at vi*3).
        const uint3 pw = uint3(
            pcS.posHeap.Get().word[vi * 3 + 0],
            pcS.posHeap.Get().word[vi * 3 + 1],
            pcS.posHeap.Get().word[vi * 3 + 2]);
        const float3 bindPos = float3(asfloat(pw.x), asfloat(pw.y), asfloat(pw.z));
        const float3 deformedPos =
            mul(m, float4(bindPos, 1.0f)).xyz;

        // Bind-pose normal/tangent (words 2/3 of the 4-word PackedVertexShading).
        const uint nW = pcS.shadeHeap.Get().word[vi * 4 + 2];
        const uint tW = pcS.shadeHeap.Get().word[vi * 4 + 3];
        float3 n = OctDecodeNormal(nW);
        float4 t = OctDecodeTangent(tW);
        const float3x3 m3 = (float3x3)m;
        n = normalize(mul(m3, n));
        t.xyz = normalize(mul(m3, t.xyz));

        // Write the deformed record: 5 words at (vi - deformedBase) * 5.
        const uint d = (vi - pcS.deformedBase) * 5;
        pcS.deformed.Get().word[d + 0] = asuint(deformedPos.x);
        pcS.deformed.Get().word[d + 1] = asuint(deformedPos.y);
        pcS.deformed.Get().word[d + 2] = asuint(deformedPos.z);
        pcS.deformed.Get().word[d + 3] = SkinPackNormalOct(n);
        pcS.deformed.Get().word[d + 4] = SkinPackTangentOct(t.xyz, t.w);
    }
}
