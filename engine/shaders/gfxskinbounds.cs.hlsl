// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GfxSkinBounds (GPU cull chain): rebuilds each SKINNED cluster's
// object-space bounding sphere AND normal cone from THIS frame's deformed
// heap, plus the per-draw deformed union bound (the tail region of the same
// table), so the very next pass (GfxBatchCull) culls against the ANIMATED
// silhouette instead of the static bind-pose data baked at load time. Runs
// between the skinning dispatch and GfxBatchCull — the Northlight pipeline's
// "dynamic meshlet bounds update" box (Remedy's GPU compute step: skin
// early, update the dynamic spatial metadata, then cull).
//
// Why it exists: the static clusters[] bounds are bind-pose. An animation
// that swings a cluster outside its bind sphere makes the batch cull drop
// the whole cluster -> its triangles never enter the draw list -> the seed
// depth pass (which NEVER covered them) leaves the sky behind, punched into
// the base-pose silhouette and bloomed. Recomputing the sphere per frame
// from the deformed positions closes the hole: the cull keeps exactly the
// clusters the deformed draw would rasterize.
//
// The cone is rebuilt from the deformed FACE normals (cross products of the
// deformed triangle corners — conservative with respect to what the
// rasterizer actually shades; smooth vertex normals would NOT bound the
// face normals, the classic cone-fit trap), replicating meshoptimizer's
// meshlet cone convention (the baked static cones' exact math —
// meshopt_computeMeshletBounds, meshletutils.cpp), so GfxConeCulled consumes
// it unchanged:
//   axis  = normalize(sum of raw cross products) — area-weighted
//           representative axis (Remedy's "wave reduction over the normals";
//           ANY axis + min-dot cutoff is conservative, only tightness varies)
//   mindp = min over triangles of dot(axis, unit face normal)
//           (<= 0.1 — cone wider than ~168 degrees — keeps the degenerate
//           keep-cone, exactly meshopt's cutoff)
//   apex  = sphereCenter - axis * maxt, maxt = max(0, max over triangles of
//           dot(center - p0, n) / dot(axis, n)) — the apex lands behind every
//           face plane by construction (denominator >= mindp > 0), so the
//           keep/cull verdict can only over-keep
//   cutoff = sqrt(1 - mindp^2) — the 90-degree-both-sides widening meshopt's
//           perspective camera test requires
// A wrong rebuilt cone could only drop VISIBLE geometry, so every failure
// direction (axis ~0, wide cone, no triangles) writes the DEGENERATE cone
// (axis 0 -> GfxConeCulled's len2 < 1e-12 keep) and loses only the reject.
//
// The per-draw tail (drawBound[drawIdx], one float4 (center.xyz, radius),
// object space) is the union sphere over the draw's rebuilt cluster
// spheres — the deformed twin of the static meshBounds union the draw-level
// fast discards (GfxBatchCull / GfxDrawCull) and the demand publisher use.
// Written DEFINITIVELY for every skinned draw index every frame the pass
// runs (dropped/capped draws get a keep-sphere radius 1e30 — the same
// convention FinishMesh's NaN guard uses); static draws never read their
// slot (consumers gate per-draw on GFX_DRAW_SKINNED, which is only set in
// frames whose skinning driver ran this pass).
//
// Dispatch shape mirrors GfxBatchCull: one 128-thread workgroup per draw;
// threads stride the draw's clusters, each owning whole clusters (serial
// per-cluster loops), then a groupshared 128-box reduction folds the union
// (thread 0, fixed 128 iterations — no float atomics). One
// GfxSkinBoundsTable per (view, flight) slice (same slot indexing the batch
// cull already uses), so multi-instance skinned draws stay independent.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (GfxSkinBoundsPush /
// FitDeformedCone — the tested CPU reference of the cone fit).

#define GFXSKINBOUNDS_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

[[vk::push_constant]]
ConstantBuffer<GfxSkinBoundsPush> pcS;

groupshared float3 gsUnionMin[128];
groupshared float3 gsUnionMax[128];

[numthreads(128, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint drawIdx = gid.x;
    if (drawIdx >= pcS.drawCount || drawIdx >= GFX_MAX_DRAWS)
        return;

    const GfxDraw d = pcS.draws.Get().draws[drawIdx];
    if ((d.misc.y & GFX_DRAW_SKINNED) == 0)
        return; // static draw: the batch cull reads the static tables
    const uint base = d.misc2.z;
    GfxMeshEntry me;
    me.clusterStart = 0u;
    me.clusterCount = 0u;
    if (d.misc.x < GFX_MAX_MESHES)
        me = pcS.meshes.Get().mesh[d.misc.x];

    // Per-thread union box over this thread's strided clusters (the
    // thread's clusters' rebuilt spheres). +inf/-inf when the thread saw
    // none; thread 0's fold then yields the keep-sphere.
    float3 boxMin = float3(1e30, 1e30, 1e30);
    float3 boxMax = float3(-1e30, -1e30, -1e30);

    for (uint ci = gtid.x; ci < me.clusterCount; ci += 128u)
    {
        const uint slot = base + ci;
        if (base == 0xFFFFFFFFu || slot >= GFX_MAX_BATCH_SLOTS)
            continue; // dropped draw / candidate cap: no slot to write

        const GfxClusterData c = pcS.clusters.Get().cluster[me.clusterStart + ci];
        const uint idxOff = c.a.x;
        const uint triCount = c.a.y;

        GfxClusterData o;
        o.a = c.a; // (indexOffset, triCount): the seed command reads these
        float3 center = c.b.xyz;
        float radius = c.b.w;
        if (triCount == 0u)
        {
            o.b = c.b; // no triangles: keep the static bound
            o.c = float4(0.0f, 0.0f, 0.0f, 1.0f); // degenerate keep-cone
            o.d = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        else
        {
            // Walk 1: object-space AABB over the cluster's deformed
            // positions + the area-weighted face-normal sum (the raw cross
            // products — degenerate slivers contribute zero on their own).
            // The index stream holds GLOBAL vertex ids; the deformed heap
            // is indexed (vi - deformedBase).
            float3 lo = float3(1e30, 1e30, 1e30);
            float3 hi = float3(-1e30, -1e30, -1e30);
            float3 nsum = float3(0.0f, 0.0f, 0.0f);
            for (uint t = 0u; t < triCount; ++t)
            {
                float3 pv[3];
                [unroll]
                for (uint k = 0u; k < 3u; ++k)
                {
                    const uint vi = pcS.indices.Get().idx[idxOff + t * 3u + k];
                    const uint w = (vi - pcS.deformedBase) * 5u;
                    pv[k] = float3(
                        asfloat(pcS.deformed.Get().word[w + 0]),
                        asfloat(pcS.deformed.Get().word[w + 1]),
                        asfloat(pcS.deformed.Get().word[w + 2]));
                    lo = min(lo, pv[k]);
                    hi = max(hi, pv[k]);
                }
                nsum += cross(pv[1] - pv[0], pv[2] - pv[0]);
            }
            // Conservative sphere: AABB centre + half-diagonal. Never
            // smaller than the true bound -> the cull can only over-keep.
            center = (lo + hi) * 0.5;
            radius = length(hi - center);
            o.b = float4(center, radius);

            // Cone fit on the same deformed corners (meshopt's
            // meshopt_computeMeshletBounds convention — see the header).
            // coneEnabled = 0 (TOURMALINE_SKIN_CONE=0, the A/B lever) keeps
            // the degenerate keep-cone — the pre-rebuild behavior, so a
            // suspect cone can be isolated from the sphere rebuild in one
            // headless run.
            const float nlen2 = dot(nsum, nsum);
            if (pcS.coneEnabled != 0u && nlen2 > 1e-12f)
            {
                const float3 axis = nsum * (1.0f / sqrt(nlen2));
                // Walk 2: min dot(axis, unit face normal). Also skips
                // zero-area triangles for the apex walk below (they shade
                // nothing, so the cone may ignore their planes).
                float mindp = 1.0f;
                for (uint t = 0u; t < triCount; ++t)
                {
                    float3 pv[3];
                    [unroll]
                    for (uint k = 0u; k < 3u; ++k)
                    {
                        const uint vi = pcS.indices.Get().idx[idxOff + t * 3u + k];
                        const uint w = (vi - pcS.deformedBase) * 5u;
                        pv[k] = float3(
                            asfloat(pcS.deformed.Get().word[w + 0]),
                            asfloat(pcS.deformed.Get().word[w + 1]),
                            asfloat(pcS.deformed.Get().word[w + 2]));
                    }
                    const float3 nRaw = cross(pv[1] - pv[0], pv[2] - pv[0]);
                    const float l2 = dot(nRaw, nRaw);
                    if (l2 > 1e-24f)
                        mindp = min(mindp, dot(axis, nRaw * (1.0f / sqrt(l2))));
                }
                // Cone wider than ~168 degrees (meshopt's cutoff): no
                // useful reject left — keep the degenerate keep-cone.
                if (mindp > 0.1f)
                {
                    // Walk 3: the apex — the last point on the
                    // center-t*axis ray still behind EVERY face plane.
                    // dot(axis, n) >= mindp > 0.1 by construction, so the
                    // divisions are well-defined; t < 0 (center already
                    // behind a plane) folds into maxt's 0 floor, exactly
                    // meshopt's maxt = max(0, ...) clamp.
                    float maxt = 0.0f;
                    for (uint t = 0u; t < triCount; ++t)
                    {
                        float3 pv[3];
                        [unroll]
                        for (uint k = 0u; k < 3u; ++k)
                        {
                            const uint vi = pcS.indices.Get().idx[idxOff + t * 3u + k];
                            const uint w = (vi - pcS.deformedBase) * 5u;
                            pv[k] = float3(
                                asfloat(pcS.deformed.Get().word[w + 0]),
                                asfloat(pcS.deformed.Get().word[w + 1]),
                                asfloat(pcS.deformed.Get().word[w + 2]));
                        }
                        const float3 nRaw = cross(pv[1] - pv[0], pv[2] - pv[0]);
                        const float l2 = dot(nRaw, nRaw);
                        if (l2 <= 1e-24f)
                            continue; // zero-area: shades nothing
                        const float3 n = nRaw * (1.0f / sqrt(l2));
                        const float dn = dot(axis, n);
                        const float dc = dot(center - pv[0], n);
                        maxt = max(maxt, dc / dn);
                    }
                    // sqrt(1 - mindp^2) with mindp clamped against float
                    // overshoot (NaN cutoffs are ugly even though
                    // comparisons against NaN never cull).
                    const float m = min(mindp, 1.0f);
                    o.c = float4(center - axis * maxt, sqrt(1.0f - m * m));
                    o.d = float4(axis, 0.0f);
                }
                else
                {
                    o.c = float4(0.0f, 0.0f, 0.0f, 1.0f); // degenerate keep-cone
                    o.d = float4(0.0f, 0.0f, 0.0f, 0.0f);
                }
            }
            else
            {
                o.c = float4(0.0f, 0.0f, 0.0f, 1.0f); // degenerate keep-cone
                o.d = float4(0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
        pcS.skinBounds.Get().cluster[slot] = o;

        // Fold this cluster's rebuilt sphere into the thread's union box.
        boxMin = min(boxMin, o.b.xyz - o.b.w);
        boxMax = max(boxMax, o.b.xyz + o.b.w);
    }

    // Per-draw union bound: 128 per-thread boxes -> groupshared fold ->
    // thread 0's serial 128-step reduce (fixed cost, no float atomics).
    // Every thread reaches this barrier: the loop above only `continue`s
    // per-cluster; the early-outs above are workgroup-uniform.
    gsUnionMin[gtid.x] = boxMin;
    gsUnionMax[gtid.x] = boxMax;
    GroupMemoryBarrierWithGroupSync();

    if (gtid.x == 0)
    {
        float3 mn = gsUnionMin[0];
        float3 mx = gsUnionMax[0];
        [unroll]
        for (uint i = 1u; i < 128u; ++i)
        {
            mn = min(mn, gsUnionMin[i]);
            mx = max(mx, gsUnionMax[i]);
        }
        // Empty / NaN box (no cluster slots this frame — dropped or capped
        // draw, invalid mesh): the definitive keep-sphere, the same
        // convention FinishMesh's NaN guard uses (radius 1e30 keeps every
        // test; such draws issue no batches anyway).
        if (mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)
            pcS.skinBounds.Get().drawBound[drawIdx] =
                float4((mn + mx) * 0.5f, length((mx - mn) * 0.5f));
        else
            pcS.skinBounds.Get().drawBound[drawIdx] = float4(0.0f, 0.0f, 0.0f, 1e30f);
    }
}
