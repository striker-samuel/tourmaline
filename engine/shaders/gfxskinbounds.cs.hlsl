// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GfxSkinBounds (GPU cull chain): rebuilds each SKINNED cluster's
// object-space bounding sphere from THIS frame's deformed heap, so the very
// next pass (GfxBatchCull) culls against the ANIMATED silhouette instead of
// the static bind-pose sphere baked at load time. Runs between the skinning
// dispatch and GfxBatchCull.
//
// Why it exists: the static clusters[] bounds are bind-pose. An animation
// that swings a cluster outside its bind sphere makes the batch cull drop
// the whole cluster -> its triangles never enter the draw list -> the seed
// depth pass (which NEVER covered them) leaves the sky behind, punched into
// the base-pose silhouette and bloomed. Recomputing the sphere per frame
// from the deformed positions closes the hole: the cull keeps exactly the
// clusters the deformed draw would rasterize.
//
// Dispatch shape mirrors GfxBatchCull: one 128-thread workgroup per draw;
// threads stride the draw's clusters. Non-skinned draws return immediately.
// One GfxClusterData per candidate slot (same slot index the batch cull
// already uses), so multi-instance skinned draws stay independent.
//
// The normal cone is NOT rebuilt: the animated normals could yield a
// different cone, but the cone test is a pure fast-reject (a wrong cone can
// only drop visible geometry), so the conservative choice is a DEGENERATE
// cone (axis 0 -> GfxConeCulled's len2 < 1e-12 keep). Skinned draws just
// lose the free reject.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (GfxSkinBoundsPush).

#define GFXSKINBOUNDS_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

[[vk::push_constant]]
ConstantBuffer<GfxSkinBoundsPush> pcS;

[numthreads(128, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint drawIdx = gid.x;
    if (drawIdx >= pcS.drawCount || drawIdx >= GFX_MAX_DRAWS)
        return;

    const GfxDraw d = pcS.draws.Get().draws[drawIdx];
    if ((d.misc.y & GFX_DRAW_SKINNED) == 0)
        return; // static draw: the batch cull reads the static table
    const uint base = d.misc2.z;
    if (base == 0xFFFFFFFFu || d.misc.x >= GFX_MAX_MESHES)
        return; // dropped draw (cap): no slots to write

    const GfxMeshEntry me = pcS.meshes.Get().mesh[d.misc.x];

    for (uint ci = gtid.x; ci < me.clusterCount; ci += 128u)
    {
        const uint slot = base + ci;
        if (slot >= GFX_MAX_BATCH_SLOTS)
            continue;

        const GfxClusterData c = pcS.clusters.Get().cluster[me.clusterStart + ci];
        const uint idxOff = c.a.x;
        const uint triCount = c.a.y;

        GfxClusterData o;
        o.a = c.a; // (indexOffset, triCount): the seed command reads these
        if (triCount == 0u)
        {
            o.b = c.b; // no triangles: keep the static bound
        }
        else
        {
            // Object-space AABB over the cluster's deformed positions. The
            // index stream holds GLOBAL vertex ids; the deformed heap is
            // indexed (vi - deformedBase).
            float3 lo = float3(1e30, 1e30, 1e30);
            float3 hi = float3(-1e30, -1e30, -1e30);
            for (uint t = 0u; t < triCount; ++t)
            {
                [unroll]
                for (uint k = 0u; k < 3u; ++k)
                {
                    const uint vi = pcS.indices.Get().idx[idxOff + t * 3u + k];
                    const uint w = (vi - pcS.deformedBase) * 5u;
                    const float3 p = float3(
                        asfloat(pcS.deformed.Get().word[w + 0]),
                        asfloat(pcS.deformed.Get().word[w + 1]),
                        asfloat(pcS.deformed.Get().word[w + 2]));
                    lo = min(lo, p);
                    hi = max(hi, p);
                }
            }
            // Conservative sphere: AABB centre + half-diagonal. Never
            // smaller than the true bound -> the cull can only over-keep.
            const float3 center = (lo + hi) * 0.5;
            o.b = float4(center, length(hi - center));
        }
        o.c = float4(0, 0, 0, 1); // degenerate cone: GfxConeCulled keeps
        o.d = float4(0, 0, 0, 0);
        pcS.skinBounds.Get().cluster[slot] = o;
    }
}
