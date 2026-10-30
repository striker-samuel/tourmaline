// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GfxBatchCull (GPU cull chain): the meshlet-level cull that used to be
// BuildBatches' CPU loop, moved wholesale to the GPU — one 128-thread
// workgroup per material-sorted draw, threads striding the draw's mesh
// clusters. Runs FIRST in the frame (its outputs gate the seed depth pass):
//
//   1. draw-level fast discard (Northlight style): the mesh union bound vs
//      the LIVE view's guard-dilated planes (the seed/remainder semantics
//      view — the FROZEN view belongs to the filter legs' gfxdrawcull).
//      A discarded draw still writes definitive 0 verdicts for every one
//      of its candidate slots (no stale kept bits, no stale commands).
//   2. per-cluster sphere vs the same planes + world-space normal-cone
//      rejection (GfxConeCulled — the C++ CullCluster port; skipped for
//      degenerate cones).
//   3. temporal verdicts, all GPU-resident (zero CPU readbacks): the
//      kept-bit parity ring holds the previous frame chain's verdicts
//      (written smvk::limits::MaxFramesInFlight (2) frames ago — flight-fenced complete).
//        seed      = kept && prevKept  (reused visible set)
//        remainder = kept && !prevKept (disocclusions: drawn the same
//                                       frame — no one-frame holes)
//      Slot-stable identity: conservative under layout churn (a stale
//      match only adds seed depth; a stale miss falls to the remainder
//      pass the same frame).
//   4. writes per candidate slot (positionally — instanceCount is the
//      verdict, 0 = front-end no-op): the Batch entry (publisher flag on
//      the FIRST KEPT slot of the draw, via a groupshared first-kept
//      reduction), the filter's indirect-command static parts, and the
//      seed/remainder command arrays (firstInstance = draw index into
//      THIS frame's draws).
//
// The static cluster tables (meshes / clusters / meshBounds) are load-time
// CPU uploads; the per-frame CPU work is seeding the draw list (with the
// candidate-slot base riding Draw.misc2.z) — everything from here to
// rasterization runs on the GPU.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (BatchCullPush).

#define GFXBATCHCULL_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"
#include "gpu_platform.hlsli"

GPU_PUSH(GfxBatchCullPush, pcB);

groupshared float4 gsPlanes[6];
groupshared bool gsDrawVisible;
groupshared float gsBasis;
groupshared uint gsFirstKept; // min kept cluster index (0xFFFFFFFF = none)

[numthreads(128, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint drawIdx = gid.x;
    const bool drawValid = drawIdx < pcB.drawCount && drawIdx < GFX_MAX_DRAWS;
    GfxDraw d;
    d.model = float4x4(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    d.misc = uint4(0, 0, 0, 0);
    d.misc2 = uint4(0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu); // misc2.w lm sentinel
    GfxMeshEntry me;
    me.clusterStart = 0u;
    me.clusterCount = 0u;
    if (drawValid)
    {
        d = pcB.draws.Get().draws[drawIdx];
        if (d.misc.x < GFX_MAX_MESHES)
            me = pcB.meshes.Get().mesh[d.misc.x];
    }
    // misc2.z = this draw's candidate slot base (the CPU build's prefix);
    // 0xFFFFFFFF = the build dropped the draw (cap) — its slots do not
    // exist in this frame's candidate space.
    const uint base = d.misc2.z;

    if (gtid.x == 0)
    {
        gsFirstKept = 0xFFFFFFFFu;
        gsDrawVisible = false;
        gsBasis = 1.0f;
        if (base != 0xFFFFFFFFu)
        {
            GfxExtractViewPlanes(pcB.viewProj,
                GFX_CULL_GUARD_PX * 2.0f / max((float) pcB.width, 1.0f),
                GFX_CULL_GUARD_PX * 2.0f / max((float) pcB.height, 1.0f),
                gsPlanes);
            gsBasis = GfxMaxBasisScale(d.model);
            // Draw-level fast discard vs the LIVE view. SKINNED draws test
            // the DEFORMED union bound (GfxSkinBounds' drawBound tail —
            // the union over the deformed cluster spheres this pass's
            // per-cluster test below runs on): the static bind-pose mesh
            // union can swing a whole animated mesh clean outside itself,
            // dropping every slot of the draw (the same class of animated
            // hole the per-cluster skinBounds table closes). Falls back to
            // the static union when the bounds pass didn't run
            // (skinBoundsValid = 0 — no skinned cluster existed that frame).
            const bool skinned0 = (d.misc.y & GFX_DRAW_SKINNED) != 0;
            float4 mb = (skinned0 && pcB.skinBoundsValid != 0u)
                ? pcB.skinBounds.Get().drawBound[drawIdx]
                : pcB.meshBounds.Get().centerRadius[d.misc.x];
            float3 worldC = mul(d.model, float4(mb.xyz, 1.0f)).xyz;
            gsDrawVisible = GfxSphereInPlanes(gsPlanes, worldC, mb.w * gsBasis);
        }
        // The filter's filtered-offset counter is allocated atomically
        // downstream; group 0 zeroes it once per frame here (GfxBatchCull
        // always runs before the filter — the rendergraph orders them).
        if (drawIdx == 0)
            pcB.cullState.Get().v[GFX_CULL_STATE_COUNTER] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Skinned draws cull against the DEFORMED bounds (GfxSkinBounds wrote
    // one GfxClusterData per candidate slot earlier this frame), not the
    // static bind-pose sphere: a pose that swings a cluster out of its
    // bind sphere would otherwise drop the cluster and punch a hole the
    // seed depth never covered (sky leak + bloom). skinBoundsValid = 0
    // (no skinned cluster this frame) leaves every draw on the static table.
    const bool skinned = (d.misc.y & GFX_DRAW_SKINNED) != 0;
    const bool useSkinBounds = skinned && pcB.skinBoundsValid != 0u;
    const uint keptOff = pcB.keptParity * GFX_MAX_BATCH_SLOTS;
    const uint prevOff = (1u - pcB.keptParity) * GFX_MAX_BATCH_SLOTS;
    const uint maskOk = (d.misc.z & pcB.drawSetMask) != 0u ? 1u : 0u;

    for (uint ci = gtid.x; ci < me.clusterCount; ci += 128u)
    {
        const uint slot = base + ci;
        if (base == 0xFFFFFFFFu || slot >= GFX_MAX_BATCH_SLOTS)
            continue; // dropped draw / candidate cap: no slot to write

        GfxClusterData c;
        if (useSkinBounds)
            c = pcB.skinBounds.Get().cluster[slot];
        else
            c = pcB.clusters.Get().cluster[me.clusterStart + ci];

        // Sphere + world-space cone (the C++ CullCluster port, LIVE view).
        // CONSERVATIVE for skinned clusters: the cone test is SKIPPED
        // unless skinConeEnabled (TOURMALINE_SKIN_CONE=1). The fit itself is
        // verified honest (the SkinDump CPU-reference cross-check agrees
        // 2000/2000), but the knight's posed clusters are ~42% degenerate
        // (the spread is real) so the reject wins little — and the fog
        // evidence (depth-complete silhouette, Main-missing meshlets)
        // pinned the visible-chunk drops on the lossy filter stages, so
        // skinned geometry runs conservative until the residual dropper
        // is isolated. This mirrors Remedy's own shipped call: Alan Wake
        // 2 rebuilt cones (~0.1 ms) but SHIPPED with meshlet cone culling
        // disabled — their HZB occlusion covered it (as this engine's
        // does for static geometry).
        bool kept = gsDrawVisible;
        if (kept)
        {
            float3 worldC = mul(d.model, float4(c.b.xyz, 1.0f)).xyz;
            kept = GfxSphereInPlanes(gsPlanes, worldC, c.b.w * gsBasis);
        }
        if (kept && (!skinned || pcB.skinConeEnabled != 0u))
            kept = !GfxConeCulled(c, d.model, pcB.eye);
        if (kept)
            InterlockedMin(gsFirstKept, ci);

        // Temporal verdicts against the GPU-resident prev-kept ring.
        const uint keptV = kept ? 1u : 0u;
        const uint prevKept = pcB.kept.Get().kept[prevOff + slot] != 0u ? 1u : 0u;
        const uint seedV = (keptV != 0u && prevKept != 0u && maskOk != 0u) ? 1u : 0u;
        const uint remV = (keptV != 0u && prevKept == 0u && maskOk != 0u) ? 1u : 0u;

        // Every candidate slot gets a definitive write every frame — no
        // clearing, no stale content (the parity region's previous writer
        // was smvk::limits::MaxFramesInFlight (2) frames ago and is fence-complete).
        pcB.kept.Get().kept[keptOff + slot] = keptV;
        pcB.batches.Get().batch[slot] =
            uint4(me.clusterStart + ci, drawIdx, 0u, 0u);
        GfxIndirectCmd indirect;
        indirect.indexCount = 0u;   // the filter writes survivors
        indirect.instanceCount = 1u;
        indirect.firstIndex = 0u;
        indirect.vertexOffset = 0;
        indirect.firstInstance = drawIdx;
        pcB.indirect.Get().cmds[slot] = indirect;
        GfxSeedCmd seed;
        seed.indexCount = c.a.y * 3u;              // whole cluster (seed depth)
        seed.instanceCount = seedV;               // 0 = front-end no-op
        seed.firstIndex = pcB.clusterIndexBase + c.a.x; // absolute monoblock
        seed.vertexOffset = 0;
        seed.firstInstance = drawIdx;             // THIS frame's draws
        pcB.seedCmds.Get().cmd[slot] = seed;
        GfxSeedCmd rem = seed;
        rem.instanceCount = remV;
        pcB.remCmds.Get().cmd[slot] = rem;
    }
    GroupMemoryBarrierWithGroupSync();

    // Publisher flag: the thread that owns the draw's FIRST KEPT cluster
    // rewrites its batch entry with GFX_BATCH_DEMAND (one designated
    // publisher group per draw for the filter's PublishDrawDemand — the
    // containment argument in gfxfilter.hlsli's GfxBatches note holds
    // exactly as it did for the CPU-assigned flag).
    if (base != 0xFFFFFFFFu
        && gsFirstKept != 0xFFFFFFFFu
        && gsFirstKept >= gtid.x
        && (gsFirstKept - gtid.x) % 128u == 0u
        && gsFirstKept < me.clusterCount)
    {
        const uint slot = base + gsFirstKept;
        if (slot < GFX_MAX_BATCH_SLOTS)
            pcB.batches.Get().batch[slot].w = GFX_BATCH_DEMAND;
    }
}
