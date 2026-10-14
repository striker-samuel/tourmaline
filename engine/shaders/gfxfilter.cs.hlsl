// GeometryFX-style per-view triangle filter (Vulkan port, per-cluster).
//
// One workgroup (128 threads) per surviving cluster; one thread per
// triangle. Each group writes exactly its own indirect command slot, so no
// cross-group atomics are needed (intra-group compaction only). Port of AMD
// GeometryFX FilterCS/CullTriangle:
//   reference/GeometryFX/amd_geometryfx/src/Shaders/AMD_GeometryFX_Filtering.hlsl
//
// Deviations from the DirectX original:
//   - Units are meshlet clusters (<=128 tris), not 256-tri slices; the CPU
//     pre-culls per (draw x cluster) with sphere + normal-cone tests, so the
//     GPU only sees surviving clusters (GPU cull mode: gfxbatchcull wrote
//     the batch list; this shader re-gates on the kept/draw verdicts and
//     skips the projection work for dropped slots).
//   - Frustum test runs in CLIP space (-w<=x<=w, -w<=y<=w, 0<=z<=w, Vulkan
//     zero-to-one depth) instead of divided NDC 0..1 space. This is exact
//     under the engine's reversed infinite-Z projection (GreaterEqual,
//     clear 0); the original's w<0 bookkeeping is not.
//   - Backface winding sign is selectable (GFX_CULL_FLIPWINDING) because the
//     engine Y-flips projection; verify once with statistics (closed sphere
//     must cull ~50% via backface alone).
//   - Small-primitive test is the original 23.8 fixed-point sample test,
//     skipped conservatively when any vertex has w<=0.
//
// Buffers arrive via buffer-device-address push (vk::BufferPointer, same as
// forwardplus.cs.hlsl) so no descriptor/pipeline-layout changes are needed.
// Indirect-arg clearing is done CPU-side (zero-fill upload before the pass)
// in the candidate layout; the cull chain writes the
// static parts of every candidate slot, replacing both the zero-fill upload
// and the CPU batch upload.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (FilterPush, 256 bytes).

#define GFXFILTER_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

[[vk::push_constant]]
ConstantBuffer<GfxFilterPush> pcF;

groupshared uint gLocalCount;
groupshared uint gClusterCull; // T4: 1 = whole cluster dropped (occlusion/contribution)
groupshared uint gOutBase;     // GPU cull: this group's filtered-offset base (0xFFFFFFFF = dropped)

// HZB context for this dispatch (the T4 occlusion/contribution inputs and
// the demand publisher share one core — see the moved shared helpers in
// gfxfilter.hlsli, also consumed by gfxdrawcull.cs.hlsl).
GfxHzbCtx FilterHzbCtx()
{
    GfxHzbCtx ctx;
    ctx.viewProj = pcF.viewProj;
    ctx.eye = pcF.eye;
    ctx.width = pcF.hizWidth;
    ctx.height = pcF.hizHeight;
    ctx.mips = pcF.hizMips;
    ctx.hzb = pcF.hzb;
    return ctx;
}

// T4 per-cluster occlusion + contribution: thin composition of
// ProjectBound + BoundHzbOccluded (semantics identical to the old inline
// math — the derivations live on those two now). Every failure path
// keeps the cluster ("visible"): a too-large bound merely misses culling;
// a too-small bound could make visible geometry disappear.
bool ClusterOccluded(uint clusterIdx, uint drawIdx)
{
    uint flags = pcF.cullFlags;
    bool wantOcc = (flags & GFX_CULL_OCCLUSION) != 0;
    bool wantCon = (flags & GFX_CULL_CONTRIBUTION) != 0;

    // MASK draws can be excluded from the occlusion test: the MIN-reduced
    // pyramid stores the NEAREST sub-texel per coarse texel, so one leaf
    // texel of a cutout card poisons the whole texel "near" — dense masked
    // foliage then culls clusters still visible through its own cutout
    // holes. Contribution (if enabled) still applies either way.
    if (wantOcc && (flags & GFX_CULL_HZBSKIPMASKED) != 0
        && (pcF.draws.Get().draws[drawIdx].misc.z & GFX_DRAWSET_MASKED) != 0)
        wantOcc = false;

    if (!wantOcc && !wantCon)
        return false;

    if (pcF.hizMips == 0)
        return false;

    float width = (float) pcF.hizWidth;
    float height = (float) pcF.hizHeight;
    if (!(width > 0.0f) || !(height > 0.0f))
        return false;

    GfxHzbCtx ctx = FilterHzbCtx();

    BoundProjection p = ProjectBound(
        pcF.clusters.Get().cluster[clusterIdx].b, // xyz center, w radius
        pcF.draws.Get().draws[drawIdx].model, ctx);

    if (!p.testable)
        return false; // eye inside / near-plane cross / NaN: keep
    if (!p.onTarget)
        return false; // off-target: leave this to the frustum stages

    const float rectW = max(p.rect.z - p.rect.x, 0.0f);
    const float rectH = max(p.rect.w - p.rect.y, 0.0f);

    // Contribution test uses a conservative upper bound on projected area.
    // If even this upper bound is below the threshold, culling is safe.
    if (wantCon && pcF.minContribution > 0.0f
        && rectW * rectH < pcF.minContribution)
    {
        return true;
    }

    if (!wantOcc)
        return false;

    return BoundHzbOccluded(p, ctx);
}

// Draw-level texture-demand publication (the designated publisher group
// of each draw — batch flags bit 0; every group of a draw shares the
// draw's material, so ONE group publishes per draw instead of one
// publication per surviving cluster).
//
// The demand-side rules mirror the cull side exactly:
//   - Provably hidden (draw bound HZB-occluded), fully off-target or
//     below the contribution floor -> nothing of the draw survives the
//     view -> no demand. Containment: the mesh sphere contains every
//     cluster sphere, so its conservative rect covers every cluster rect
//     and its dNear >= every cluster dNear — a draw bound that fails the
//     test means every cluster bound fails it too, so skipping
//     publication is never a missed demand, only an avoided over-request.
//   - Untestable bound (eye inside / near-plane cross / NaN, or no HZB
//     extent) -> publish the SHARPEST mip: the failure direction for
//     demand is over-request, the opposite of the cull side's keep.
//
// wanted = the exact mirror of TextureStreamer::WantedMipForScreenCoverage:
//   clamp(ceil(log2(max(1, baseLongSide) / max(1, coverage))) + mipBias,
//         0, fullMips - 1)
// (4096-texel texture on a 1024px object -> mip 2; on a 256px -> 4.)
// LOD bias comes free: the bound is the SELECTED LOD sibling's own union
// bound, so distant LOD selections naturally demand coarser mips.
void PublishDrawDemand(uint drawIdx)
{
    // Single gate: PushAndDispatch zero-clears textureMetaCount when any
    // demand-side BDA is null (a null BDA dereference is an MMU fault and
    // takes the whole device down; same belt-and-braces rule as the HZB
    // inputs — see the null-BDA guard in gfx_filter.cpp).
    if (pcF.textureMetaCount == 0u)
        return;

    GfxDraw d = pcF.draws.Get().draws[drawIdx];
    const uint material = d.misc2.y;
    if (material == 0xFFFFFFFFu)
        return; // no material: no texture slots to publish

    // Coverage source: the conservative draw-bound rect when testable +
    // on-target; the untestable default below yields mip 0 (sharpest) —
    // the over-request direction.
    float coveragePx = 1e9f; // "sharper than any texture"
    if (pcF.hizWidth != 0u && pcF.hizHeight != 0u)
    {
        GfxHzbCtx ctx = FilterHzbCtx();
        const BoundProjection p = ProjectBound(
            pcF.meshBounds.Get().centerRadius[d.misc.x], d.model, ctx);
        if (p.testable)
        {
            if (!p.onTarget)
                return; // fully off-screen: the frustum stages cull it all
            const uint flags = pcF.cullFlags;
            const bool wantCon = (flags & GFX_CULL_CONTRIBUTION) != 0;
            if (wantCon && pcF.minContribution > 0.0f
                && (p.rect.z - p.rect.x) * (p.rect.w - p.rect.y)
                    < pcF.minContribution)
                return; // below the contribution floor: culled anyway
            bool wantOcc = (flags & GFX_CULL_OCCLUSION) != 0
                && pcF.hizMips != 0u;
            if (wantOcc && (flags & GFX_CULL_HZBSKIPMASKED) != 0
                && (d.misc.z & GFX_DRAWSET_MASKED) != 0)
                wantOcc = false;
            if (wantOcc && BoundHzbOccluded(p, ctx))
                return; // provably hidden: nothing of this draw survives
            coveragePx = max(
                p.rect.z - p.rect.x,
                p.rect.w - p.rect.y);
        }
    }

    // Per-slot wanted FULL-CHAIN mip (residency stays out of this shader:
    // meta carries fullMips, the CPU consumes full-chain mips through
    // TextureStreamer::RequestMip).
    const int4 tex = pcF.materials.Get().mats[material].tex;
    const int emissive = pcF.materials.Get().mats[material].tex2.x;
    int slots[5] = { tex.x, tex.y, tex.z, tex.w, emissive };
    const float coverage = max(1.0f, coveragePx);
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        if (slots[i] < 0)
            continue; // slot unused by the material
        const uint slot = (uint) slots[i];
        if (slot >= pcF.textureMetaCount)
            continue; // outside the uploaded meta range
        const GfxTextureMeta meta = pcF.textureMeta.Get().meta[slot];
        if (meta.fullMips == 0u)
            continue; // no streamed texture at this slot
        const float ratio = max(1.0f, (float) meta.longSide) / coverage;
        const float mip = ceil(log2(ratio)) + pcF.mipBias;
        const uint want = (uint) clamp(mip, 0.0f, (float)(meta.fullMips - 1u));
        InterlockedMin(pcF.textureDemand.Get().demand[slot], want);
    }
}

bool CullTriangle(uint3 tri, float4 clip0, float4 clip1, float4 clip2, bool twoSided)
{
    uint flags = pcF.cullFlags;
    bool cull = false;

    if ((flags & GFX_CULL_DEGENERATE) != 0)
    {
        if (tri.x == tri.y || tri.y == tri.z || tri.x == tri.z)
            cull = true;
    }

    // Backface in homogeneous coordinates (Olano & Greer "2DH").
    // Skipped for camera-plane straddlers (any w <= 0): facing is only
    // well-defined pre-clip for fully-in-front triangles, and the
    // determinant on unclipped coords can disagree with the rasterizer's
    // post-clip result. Straddlers are rare; the rasterizer resolves them.
    // Skipped for two-sided draws (double-sided materials bind with
    // Cull None, so the rasterizer keeps both facings).
    if ((flags & GFX_CULL_BACKFACE) != 0 && !twoSided)
    {
        if (clip0.w > 0.0f && clip1.w > 0.0f && clip2.w > 0.0f)
        {
            float3x3 m = float3x3(clip0.xyw, clip1.xyw, clip2.xyw);
            bool back = determinant(m) > 0.0f;
            if ((flags & GFX_CULL_FLIPWINDING) != 0)
                back = !back;
            cull = cull || back;
        }
    }

    if ((flags & GFX_CULL_FRUSTUM) != 0)
    {
        // Near-plane guard (cf. GeometryFX verticesInFrontOfNearPlane):
        // all-behind (w<0, standard w=-z_view convention) is invisible and
        // culled; all-in-front gets the per-plane tests; straddlers are
        // kept (a plane test mixing w signs can wrongly cull triangles
        // crossing the camera plane).
        float4 c[3] = { clip0, clip1, clip2 };
        bool allBehind = c[0].w < 0.0f && c[1].w < 0.0f && c[2].w < 0.0f;
        bool anyBehind = c[0].w <= 0.0f || c[1].w <= 0.0f || c[2].w <= 0.0f;
        if (allBehind)
        {
            cull = true;
        }
        else if (!anyBehind)
        {
            // Guard band in clip space: 1 px each side (NDC half-width 1 =
            // width/2 px). Near/far stay exact (jitter never moves depth).
            const float guardX = GFX_CULL_GUARD_PX * 2.0f / max((float) pcF.width, 1.0f);
            const float guardY = GFX_CULL_GUARD_PX * 2.0f / max((float) pcF.height, 1.0f);
            bool outL = true, outR = true, outB = true, outT = true;
            bool outN = true, outF = true;
            [unroll]
            for (int i = 0; i < 3; ++i)
            {
                const float ex = c[i].w * guardX;
                const float ey = c[i].w * guardY;
                outL = outL && (c[i].x < -(c[i].w + ex));
                outR = outR && (c[i].x > (c[i].w + ex));
                outB = outB && (c[i].y < -(c[i].w + ey));
                outT = outT && (c[i].y > (c[i].w + ey));
                outN = outN && (c[i].z < 0.0f);
                outF = outF && (c[i].z > c[i].w);
            }
            cull = cull || outL || outR || outB || outT || outN || outF;
        }
    }

    if ((flags & GFX_CULL_SMALLPRIM) != 0 && !cull)
    {
        // Original 23.8 subpixel test on NDC 0..1; conservative skip when
        // any vertex is behind the camera.
        if (clip0.w > 0.0f && clip1.w > 0.0f && clip2.w > 0.0f)
        {
            float2 ndc[3];
            ndc[0] = clip0.xy / clip0.w * 0.5f + 0.5f;
            ndc[1] = clip1.xy / clip1.w * 0.5f + 0.5f;
            ndc[2] = clip2.xy / clip2.w * 0.5f + 0.5f;

            static const uint SUBPIXEL_BITS = 8;
            static const uint SUBPIXEL_MASK = 0xFF;
            static const uint SUBPIXEL_SAMPLES = 1u << SUBPIXEL_BITS;

            int2 minBB = int2(1 << 30, 1 << 30);
            int2 maxBB = int2(-(1 << 30), -(1 << 30));
            bool insideGuardBand = true;
            [unroll]
            for (int i = 0; i < 3; ++i)
            {
                float2 ss = ndc[i] * float2((float) pcF.width, (float) pcF.height);
                if (ss.x < -(1 << 23) || ss.x > (1 << 23) ||
                    ss.y < -(1 << 23) || ss.y > (1 << 23))
                {
                    insideGuardBand = false;
                }
                int2 sp = int2(ss * SUBPIXEL_SAMPLES);
                minBB = min(sp, minBB);
                maxBB = max(sp, maxBB);
            }
            if (insideGuardBand)
            {
                cull = cull
                    || (((minBB.x & SUBPIXEL_MASK) > SUBPIXEL_SAMPLES / 2)
                        && ((maxBB.x - ((minBB.x & ~SUBPIXEL_MASK) + SUBPIXEL_SAMPLES / 2)) < (SUBPIXEL_SAMPLES - 1)))
                    || (((minBB.y & SUBPIXEL_MASK) > SUBPIXEL_SAMPLES / 2)
                        && ((maxBB.y - ((minBB.y & ~SUBPIXEL_MASK) + SUBPIXEL_SAMPLES / 2)) < (SUBPIXEL_SAMPLES - 1)));
            }
        }
    }

    return cull;
}

[numthreads(GFX_THREADS_PER_CLUSTER, 1, 1)]
void mainCS(uint tid : SV_GroupThreadID, uint gid : SV_GroupID)
{
    if (tid == 0)
    {
        gLocalCount = 0;
        gClusterCull = 0;
        gOutBase = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Batch header: (clusterIndex, drawIndex, outputOffset, flags).
    uint4 header = (uint4) 0;
    if (gid < pcF.batchCount)
        header = pcF.batches.Get().batch[gid];

    // Batch-cull verdicts: the batch cull's per-slot verdict (LIVE-view
    // sphere + cone) and the draw cull's per-draw verdict (FROZEN view +
    // HZB draw-bound occlusion). Either 0 -> the whole group exits before
    // any projection work: gfxbatchcull already wrote the slot's indirect
    // statics (indexCount 0), so nothing is left to do.
    bool slotKept = true;
    bool drawOk = true;
    if (gid < pcF.batchCount)
    {
        // kept points at THIS frame's parity region (the CPU adds the
        // region offset to the BDA).
        slotKept = pcF.kept.Get().kept[gid] != 0;
        drawOk = pcF.cullState.Get().v[header.y] != 0;
    }

    uint2 cluster = (uint2) 0;
    uint triCount = 0;
    if (gid < pcF.batchCount)
    {
        cluster = pcF.clusters.Get().cluster[header.x].a.xy;
        triCount = cluster.y;
    }

    // T4: whole-cluster drop computed once (thread 0 has header + table).
    // Skipped for verdict-dropped slots (already gone; the containment
    // argument in PublishDrawDemand covers the draw-bound verdict too).
    if (gid < pcF.batchCount && tid == 0 && slotKept && drawOk)
    {
        if (ClusterOccluded(header.x, header.y))
            gClusterCull = 1;
    }

    // Draw-level texture demand: the designated publisher group of each
    // draw reports once (thread 0; independent of the compaction below —
    // no groupshared use, no barrier interaction, runs parallel to the
    // cluster-occlusion work above).
    if (gid < pcF.batchCount && tid == 0 && (header.w & GFX_BATCH_DEMAND) != 0)
        PublishDrawDemand(header.y);
    GroupMemoryBarrierWithGroupSync();

    bool cull = true;
    uint slot = 0;
    uint3 tri = (uint3) 0;
    if (gid < pcF.batchCount && tid < triCount && slotKept && drawOk)
    {
        uint baseIndex = cluster.x + tid * 3;
        tri = uint3(
            pcF.indices.Get().idx[baseIndex + 0],
            pcF.indices.Get().idx[baseIndex + 1],
            pcF.indices.Get().idx[baseIndex + 2]);
        float4x4 model = pcF.draws.Get().draws[header.y].model;
        float4 clip[3];
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            uint vi = (k == 0) ? tri.x : ((k == 1) ? tri.y : tri.z);
            uint wbase = vi * 3;
            float3 p = float3(
                asfloat(pcF.positions.Get().words[wbase + 0]),
                asfloat(pcF.positions.Get().words[wbase + 1]),
                asfloat(pcF.positions.Get().words[wbase + 2]));
            clip[k] = mul(pcF.viewProj, mul(model, float4(p, 1.0f)));
        }
        bool twoSided =
            (pcF.draws.Get().draws[header.y].misc.y & GFX_DRAW_TWOSIDED) != 0;
        cull = CullTriangle(tri, clip[0], clip[1], clip[2], twoSided)
            || (gClusterCull != 0);
        if (!cull)
            InterlockedAdd(gLocalCount, 3, slot);
    }

    GroupMemoryBarrierWithGroupSync();

    // This group owns command gid exclusively for the indexCount/firstIndex
    // writes. The surviving triangles allocate the group's filtered-index
    // range through the cullState counter ATOMICALLY (candidate slots are
    // positional and pre-sized; survivors are not — the counter turns them
    // into a compacted ring fill, clamped at the flight slice's capacity).
    // firstIndex stays absolute into the static index monoblock (indexBase +
    // output base); the input-assembly index binding never switches buffers.
    if (gid < pcF.batchCount && tid == 0)
    {
        uint outBase = 0;
        bool fits = gLocalCount > 0;
        if (fits)
        {
            InterlockedAdd(pcF.cullState.Get().v[GFX_CULL_STATE_COUNTER],
                gLocalCount, outBase);
            fits = outBase + gLocalCount <= GFX_MAX_FILTERED;
        }
        if (fits)
        {
            gOutBase = outBase;
            pcF.indirect.Get().cmds[gid].indexCount = gLocalCount;
            pcF.indirect.Get().cmds[gid].firstIndex = pcF.indexBase + outBase;
            pcF.indirect.Get().cmds[gid].vertexOffset = 0; // heap-global ids
            pcF.indirect.Get().cmds[gid].firstInstance = header.y;
            pcF.indirect.Get().cmds[gid].instanceCount = 1;
        }
        else
        {
            gOutBase = 0xFFFFFFFFu; // dropped (empty or ring cap): no writes
            pcF.indirect.Get().cmds[gid].indexCount = 0;
        }
    }

    // AllMemory covers BOTH the groupshared gOutBase read below and the
    // device-memory ordering the old DeviceMemoryBarrier provided.
    GroupMemoryBarrierWithGroupSync();
    AllMemoryBarrierWithGroupSync();

    if (gid < pcF.batchCount && !cull && gOutBase != 0xFFFFFFFFu)
    {
        uint outBase = gOutBase + slot; // group base + intra-group slot
        pcF.filtered.Get().idx[outBase + 0] = tri.x;
        pcF.filtered.Get().idx[outBase + 1] = tri.y;
        pcF.filtered.Get().idx[outBase + 2] = tri.z;
    }
}
