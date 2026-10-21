// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GfxConeDebug (the cone-axis overlay's GPU leg): one 128-thread workgroup
// per draw (the GfxBatchCull dispatch shape), threads stride the draw's
// clusters; writes TWO packed DebugLineVertex float3s (3 uint words each,
// the GfxPositions packed-stride pattern) per candidate slot:
//   vert[slot*2 + 0] = world apex
//   vert[slot*2 + 1] = apex + axisW * (radius * maxBasis)   (the cone tip)
// the same line the retired CPU builder drew (apex -> apex + axis *
// worldRadius). Main's overlay binds the buffer and Draws
// 2*candidateTotal verts.
//
// The cones are the cull chain's ACTUAL inputs — never a lie:
//   static draws  : the baked bind-pose cones (clusters[]) — their real
//                   cull table
//   skinned draws : THIS frame's deformed cones from the GfxSkinBounds
//                   table (cluster[slot] — the table gfxbatchcull tests),
//                   zero latency, GPU-driven end to end. The retired CPU
//                   path read the table back through the async readback
//                   ticket; its download destinations dangled through
//                   vector reallocation between submit and poll (the host
//                   heap corruption) and the cache never landed — the
//                   overlay went dark on exactly the skinned meshes it
//                   existed to inspect.
// Degenerate cones (axis ~0 — the keep-cone) and zero-radius spheres write
// zero-length lines (degenerate primitives — discarded by rasterization).
//
// The candidate slot space is a DENSE prefix over the pushed draws'
// clusters (misc2.z is the build's prefix sum), so verts
// [0, 2*candidateTotal) is EXACTLY the written range — no compaction pass,
// no per-draw ordinals.
//
// Runs after GfxSkinBounds (the skinBounds read hint pair is the ordering
// edge) and before Main's overlay draw (the compute-write ->
// vertex-attribute-read hint pair). C++ mirror: Engine/Renderer/
// gfx_filter.h (ConeDebugPush) + gfx_filter.cpp PushAndDispatchConeDebug.

#define GFXCONEDEBUG_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

[[vk::push_constant]]
ConstantBuffer<GfxConeDebugPush> pcG;

[numthreads(128, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint drawIdx = gid.x;
    if (drawIdx >= pcG.drawCount || drawIdx >= GFX_MAX_DRAWS)
        return;

    const GfxDraw d = pcG.draws.Get().draws[drawIdx];
    GfxMeshEntry me;
    me.clusterStart = 0u;
    me.clusterCount = 0u;
    if (d.misc.x < GFX_MAX_MESHES)
        me = pcG.meshes.Get().mesh[d.misc.x];
    const uint base = d.misc2.z;

    // Skinned draws read the DEFORMED cone (this frame's skinBounds table,
    // the table the batch cull tests); everything else the baked static
    // table — the actual cull input either way, never the wrong-pose lie.
    const bool skinned = (d.misc.y & GFX_DRAW_SKINNED) != 0
        && pcG.skinBoundsValid != 0u;
    const float bs = GfxMaxBasisScale(d.model);

    for (uint ci = gtid.x; ci < me.clusterCount; ci += 128u)
    {
        const uint slot = base + ci;
        if (base == 0xFFFFFFFFu || slot >= GFX_MAX_BATCH_SLOTS)
            continue; // dropped draw / candidate cap: no slot to write

        // The cone: struct ternaries are not HLSL — spelled out. The
        // `skinned` fold above picked the table.
        GfxClusterData c;
        if (skinned)
            c = pcG.skinBounds.Get().cluster[slot];
        else
            c = pcG.clusters.Get().cluster[me.clusterStart + ci];

        float3 apexW = float3(0.0f, 0.0f, 0.0f);
        float3 tipW = float3(0.0f, 0.0f, 0.0f);
        const float3 axisO = c.d.xyz;
        const float radius = c.b.w;
        if (dot(axisO, axisO) > 1e-12f && radius > 0.0f)
        {
            apexW = mul(d.model, float4(c.c.xyz, 1.0f)).xyz;
            const float3 axW = mul(d.model, float4(axisO, 0.0f)).xyz;
            const float al2 = dot(axW, axW);
            if (al2 > 1e-12f)
                tipW = apexW + axW * (1.0f / sqrt(al2)) * (radius * bs);
            // A degenerate world axis (mirrored/degenerate transform)
            // keeps the zero-length line at the origin — invisible.
        }
        // Degenerate cone (the keep-cone) / no sphere: both verts stay 0 —
        // a zero-length line, discarded by rasterization.

        const uint w = slot * 6u;
        pcG.lines.Get().word[w + 0] = asuint(apexW.x);
        pcG.lines.Get().word[w + 1] = asuint(apexW.y);
        pcG.lines.Get().word[w + 2] = asuint(apexW.z);
        pcG.lines.Get().word[w + 3] = asuint(tipW.x);
        pcG.lines.Get().word[w + 4] = asuint(tipW.y);
        pcG.lines.Get().word[w + 5] = asuint(tipW.z);
    }
}
