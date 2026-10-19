// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Shared GeometryFX-filter structs (Vulkan port, per-cluster).
// Included by gfxfilter.cs.hlsl (compute) and gfxfiltered.vert.hlsl.
// C++ mirrors: Engine/Renderer/gfx_filter.h (static_asserted sizes).

#pragma once

// Must match C++ smvk::gfx::CullFlagBits.
#define GFX_CULL_DEGENERATE  0x1
#define GFX_CULL_BACKFACE    0x2
#define GFX_CULL_FRUSTUM     0x8
#define GFX_CULL_SMALLPRIM   0x20
#define GFX_CULL_OCCLUSION   0x40 // T4: HZB sphere test (facing-agnostic)
#define GFX_CULL_CONTRIBUTION 0x80 // T4: footprint below minContribution
#define GFX_CULL_FLIPWINDING 0x100
#define GFX_CULL_HZBSKIPMASKED 0x200 // T4: exclude MASK draws from the HZB test

// Conservative culling guard band (px, internal resolution) for the main
// view. The SMAA T2x projection jitter swings clip edges by +/-0.25 px per
// frame; exact boundary tests would cull edge clusters/triangles on
// alternating frames (visible popping). The guard only ever keeps more
// (never punches holes): the frustum test widens, the occlusion footprint
// covers more texels (all must occlude), the contribution area grows.
// Mirrors kMainViewCullGuardPx (app) and the RenderView plane dilation.
#define GFX_CULL_GUARD_PX 1.0f

#include "hzb.hlsli"

#define GFX_THREADS_PER_CLUSTER 128
#define GFX_MAX_DRAWS 4096
#define GFX_MAX_BATCHES 65536
// GPU-cull candidate capacity: per (draw x cluster) slot pushed by the
// CPU candidate build (batches + indirect + seed/remainder command arrays
// are all pre-sized by this). Survivors stay bounded by GFX_MAX_BATCHES —
// the filter's atomic filtered-offset allocation clamps at the ring
// capacity (the GPU twin of the old CPU kMaxBatches cap + warning).
#define GFX_MAX_BATCH_SLOTS 262144
// Explicit bounds (this DXC rejects unsized struct members; mirrors the
// C++ capacities in Engine/Renderer/gfx_filter.h).
#define GFX_MAX_VERTICES 44739242   // 512MB pos heap / 12B (see mesh.cpp)
#define GFX_POS_WORDS 134217726     // GFX_MAX_VERTICES * 3 (uint view below)
#define GFX_MAX_CLUSTERS 262144
#define GFX_MAX_FILTERED (65536 * 128 * 3)
#define GFX_MAX_MESHES 8192         // mirrors kMaxMeshes
#define GFX_MAX_TEXTURES 8192       // mirrors smvk::MAX_TEXTURES

// Positions as uint words (3 per vertex): the heap is packed float3
// (stride 12), which a float3 array view cannot express under block layout
// rules (member-array stride must align to 16). Load via asfloat.
struct GfxPositions { uint words[GFX_POS_WORDS]; }; // static pos heap (global ids)
struct GfxIndices   { uint idx[GFX_MAX_FILTERED]; };   // cluster-ordered global ids (owned)
// Per cluster: a = (indexOffset, triCount, 0, 0), b = (center.xyz, radius),
// c = (coneApex.xyz, coneCutoff), d = (coneAxis.xyz, pad). The cone tail
// feeds the cull chain's world-space normal-cone test (gfxbatchcull;
// C++ CullCluster mirrors the same math as the tested CPU reference).
struct GfxClusterData { uint4 a; float4 b; float4 c; float4 d; };
struct GfxClusters  { GfxClusterData cluster[GFX_MAX_CLUSTERS]; };
// DEFORMED vertex heap (the Northlight compute-skinning output; the
// skinning pass - gfxskinning.cs.hlsl - wrote it this frame): 5 words per
// vertex [pos3 asuint, packed normal, packed tangent]. Skinned draws
// (GFX_DRAW_SKINNED) and the cull chain's GfxSkinBounds pass fetch at
// (vi - deformedBase) * 5; everything else reads the bind-pose heaps.
// Declared here (not gfxmesh.hlsli, which already includes this header) so
// both the VS legs and the cull-chain push mirror can reference it.
// Generous declared capacity (the BDA-pointer pattern - loads stay in
// bounds).
struct GfxSkinDeformedHeap { uint word[64 * 1024 * 1024]; };

// One uint4 per batch: (clusterIndex, drawIndex, outputOffset, flags).
// Written by gfxbatchcull (outputOffset stays 0 — the filter allocates
// real filtered offsets through the cullState counter atomically).
// flags bit 0 (GFX_BATCH_DEMAND): this group is the DESIGNATED PUBLISHER
// for its draw's texture demand — the first KEPT batch of the draw
// (gfxbatchcull assigns it via a groupshared first-kept reduction; the
// CPU-cull path's BuildBatches sets it on the first surviving batch).
// Every group of a draw shares the draw's material, so exactly one
// designated group publishes per draw instead of one publication per
// surviving cluster; see PublishDrawDemand for the containment argument
// (draw sphere contains every cluster sphere -> a designated group that
// skips publishing implies no sibling group survives either).
struct GfxBatches { uint4 batch[GFX_MAX_BATCH_SLOTS]; };
#define GFX_BATCH_DEMAND 0x1 // batch.w bit 0

// misc.y bit 1: Scene::ditherCloseup fade band this frame (C++ mirror
// DrawFlagBit_DitherCloseup): the filtered VS / mesh MS forward it flat
// (VS location 8) to the lit FS family + the masked seed FSs so they
// screen-door with CloseupDitherKeep (common.hlsli). Set at PushDraw time
// together with the MASKED bucket routing (the alpha-tested depth legs).
#define GFX_DRAW_DITHER_CLOSEUP 0x2
// misc.y bit 2 (C++ mirror DrawFlagBit_Skinned): this draw's mesh is
// SKINNED (the Northlight compute path): the mesh-shader leg fetches the
// deformed pos/normal/tangent words from the per-frame DEFORMED buffer
// (params.deformed, indexed vi - params.deformedBase) instead of the
// bind-pose heaps. The flag is only set when params.deformed != 0.
#define GFX_DRAW_SKINNED 0x4

// Draw-set buckets (T1; mirrors C++ DrawSetBit_*). The seed depth pass
// issues one PSO per bucket; views mask which buckets they accept.
#define GFX_DRAWSET_OPAQUE 0x1
#define GFX_DRAWSET_MASKED 0x2
#define GFX_DRAWSET_ALL (GFX_DRAWSET_OPAQUE | GFX_DRAWSET_MASKED)

// Temporal visibility list (T1 state; consumed by the seed pass in T3).
// One uint2 per surviving (cluster, draw): xy = (clusterIndex, drawIndex).
// Draws resolve through GfxDraws (model matrices); clusters through
// GfxClusters + GfxIndices (cluster-ordered global vertex ids).
#define GFX_MAX_VISIBLE 65536 // mirrors kMaxBatches
struct GfxVisible { uint2 vis[GFX_MAX_VISIBLE]; };

struct GfxDraw
{
    float4x4 model;
    // x = mesh index, y = flags (GFX_DRAW_*), z = draw-set bucket
    // (GFX_DRAWSET_*), w = lightmap chunk (0xFFFFFFFF = none; passed flat
    // to the lit fragment for the static/dynamic GI separation).
    uint4 misc;
    // x = base offset into the scene-wide chart transform buffer for this
    // draw's mesh (0 when the mesh carries no lightmap); y = material id
    // (the designated publisher's PublishDrawDemand resolves the 5 texture
    // slots through the materials buffer); z = GPU-cull candidate slot
    // base (this draw's first (draw x cluster) candidate slot, 0xFFFFFFFF
    // = the build dropped the draw); w reserved.
    // The VS resolves the atlas UV as
    // chartUv * xforms[misc2.x + chartId].xy + xforms[misc2.x + chartId].zw.
    uint4 misc2;
};
struct GfxDraws { GfxDraw draws[GFX_MAX_DRAWS]; };

// Scene-wide lightmap chart atlas transforms (one float4 per mesh chart:
// xy = scale, zw = offset; atlasUv = chartUv * xy + zw). Concatenated
// sorted by mesh then chart; each draw's misc2.x is its mesh's base.
// Sized generously: the runtime uploads exactly the cooked count.
#define GFX_MAX_CHART_XFORMS 1048576
struct GfxChartXforms { float4 xform[GFX_MAX_CHART_XFORMS]; };

// Resolves the atlas-space lightmap UV for one vertex. chartId ==
// 0xFFFFFFFF (uncharted) yields 0 — the fragment gates on the draw's
// lobe count anyway, so the sample lands in the atlas margin.
float2 GfxLightmapUv(float2 chartUv, uint chartId, uint chartOffset,
    vk::BufferPointer<GfxChartXforms> xforms)
{
    if (chartId == 0xFFFFFFFFu)
        return float2(0.0f, 0.0f);
    float4 xf = xforms.Get().xform[chartOffset + chartId];
    return chartUv * xf.xy + xf.zw;
}

struct GfxFiltered { uint idx[GFX_MAX_FILTERED]; };

struct GfxIndirectCmd
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct GfxIndirect { GfxIndirectCmd cmds[GFX_MAX_BATCH_SLOTS]; };

// ---------------------------------------------------------------------------
// GPU cull chain (Northlight-style; mirrors C++ gfx_filter.h). CPU seeds
// the draw list + candidate slot layout; gfxbatchcull -> GfxSeedDepth ->
// GfxHiz -> gfxdrawcull -> gfxfilter all run GPU-side with zero mid-pipeline
// CPU readbacks. The temporal link (seed = prev-kept AND curr-kept,
// remainder = curr-kept AND NOT prev-kept) stays GPU-resident through the
// kept-bit parity ring.
// ---------------------------------------------------------------------------

// GPU-written seed/remainder command arrays (VkDrawIndexedIndirectCommand
// shape). Positional: one command per candidate slot, instanceCount = the
// temporal verdict (0 = front-end no-op draw). The static parts
// (indexCount = cluster tris, firstIndex = cluster-ordered heap offset,
// firstInstance = draw index into this frame's draws) are written by
// gfxbatchcull every frame, so dropped slots never leak stale content.
struct GfxSeedCmd
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct GfxSeedCmds { GfxSeedCmd cmd[GFX_MAX_BATCH_SLOTS]; };

// Kept-bit ring: TWO parity regions in one buffer per flight. The frame
// whose parity is P writes region P (every candidate slot gets a
// definitive 0/1 store — no clearing needed) and reads region 1-P, which
// was written exactly MAX_FRAMES_IN_FLIGHT frames earlier by the same
// flight's previous user (the flight fence guarantees that write is
// complete; the current frame cannot race a concurrent one because the
// parity regions alternate per flight use).
//
// The prev-kept identity is SLOT-STABLE (not key-stable like the CPU
// temporal path): candidate slots are (draw-major, mesh clusters), so a
// stable scene layout means slot N means the same (cluster, transform)
// frame to frame. Layout churn (LOD flips, draws entering/leaving the
// frustum-filtered push list) makes slot identity drift — in BOTH drift
// directions the verdicts stay conservative: a stale-kept slot seeds
// geometry the remainder would have covered anyway (extra depth only),
// and a stale-dropped slot misses the seed and the remainder pass draws
// it the same frame (no holes). This is the standard GPU-driven temporal
// reuse trade (slot-stable instance reuse), the price of zero readbacks.
struct GfxKeptBits { uint kept[GFX_MAX_BATCH_SLOTS * 2]; };

// Draw verdicts (gfxdrawcull: mesh-bound frustum + HZB occlusion, the
// frozen cull view) + the filter's atomic filtered-offset counter, packed
// in one small buffer per flight. Verdict words are [0, GFX_MAX_DRAWS);
// the counter is word GFX_MAX_DRAWS.
#define GFX_CULL_STATE_COUNTER GFX_MAX_DRAWS
struct GfxCullState { uint v[GFX_MAX_DRAWS + 1]; };

// Static per-mesh cluster range (mirror of C++ gfx::Mesh /
// ClusterConstants' layout counterpart). Load-time upload.
struct GfxMeshEntry { uint clusterStart; uint clusterCount; };
struct GfxMeshes { GfxMeshEntry mesh[GFX_MAX_MESHES]; };

// ---------------------------------------------------------------------------
// Shared cull math (CPU mirrors: renderview.h ExtractViewPlanes /
// RenderViewIntersectsSphere / MaxBasisScale, gfx_filter.cpp CullCluster).
// ---------------------------------------------------------------------------

// Gribb-Hartmann plane extraction from the (untransposed, GLM-bytes) view
// projection, Vulkan zero-to-one depth (works for reversed infinite-Z).
// planes are normalized; a sphere is outside iff
// dot(plane.xyz, c) + plane.w < -r for any plane. guardNdcX/Y dilate the
// L/R/B/T half-spaces (callers convert pixels via 2*guardPx/extent — the
// same strictly-conservative dilation the CPU RenderView planes take).
// Math rows via _RC swizzles (see ProjectBound's majorness note).
void GfxExtractViewPlanes(float4x4 vp, float guardNdcX, float guardNdcY,
    out float4 planes[6])
{
    float4 row0 = float4(vp._11, vp._12, vp._13, vp._14);
    float4 row1 = float4(vp._21, vp._22, vp._23, vp._24);
    float4 row2 = float4(vp._31, vp._32, vp._33, vp._34);
    float4 row3 = float4(vp._41, vp._42, vp._43, vp._44);
    planes[0] = row3 * (1.0 + guardNdcX) + row0; // left   (x_ndc = -1)
    planes[1] = row3 * (1.0 + guardNdcX) - row0; // right  (x_ndc = +1)
    planes[2] = row3 * (1.0 + guardNdcY) + row1; // bottom (y_ndc = -1)
    planes[3] = row3 * (1.0 + guardNdcY) - row1; // top    (y_ndc = +1)
    planes[4] = row2;                            // z_ndc = 0
    planes[5] = row3 - row2;                      // z_ndc = 1
    [unroll]
    for (int i = 0; i < 6; ++i) {
        float len = length(planes[i].xyz);
        if (len > 1e-8)
            planes[i] /= len;
    }
}

bool GfxSphereInPlanes(float4 planes[6], float3 center, float radius)
{
    [unroll]
    for (int i = 0; i < 6; ++i) {
        if (dot(planes[i].xyz, center) + planes[i].w < -radius)
            return false;
    }
    return true;
}

// Max basis-column length: turns an object-space sphere radius into a
// conservative world radius under non-uniform scale (CPU MaxBasisScale).
float GfxMaxBasisScale(float4x4 m)
{
    return max(
        length(float3(m._11, m._21, m._31)),
        max(length(float3(m._12, m._22, m._32)),
            length(float3(m._13, m._23, m._33))));
}

// World-space normal-cone rejection (verbatim port of C++ CullCluster's
// cone stage; meshoptimizer convention: dot(normalize(apex - eye), axis)
// >= cutoff culls). The world axis uses the cofactor transform — the
// same math the mesh shader uses for world normals: mul(cof/det, v)
// == (M^-1)^T * v, so mirrored / non-uniform-scale instances cull the
// same side the rasterizer keeps. Degenerate cones / apex-at-eye keep.
bool GfxConeCulled(GfxClusterData c, float4x4 model, float3 eye)
{
    float3x3 M3 = (float3x3) model;
    float det = M3[0][0] * (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1])
              - M3[0][1] * (M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0])
              + M3[0][2] * (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);
    if (abs(det) < 1e-12)
        return false; // degenerate transform: keep (raster resolves)
    float3x3 cof;
    cof[0][0] = (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1]);
    cof[0][1] = -(M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0]);
    cof[0][2] = (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);
    cof[1][0] = -(M3[0][1] * M3[2][2] - M3[0][2] * M3[2][1]);
    cof[1][1] = (M3[0][0] * M3[2][2] - M3[0][2] * M3[2][0]);
    cof[1][2] = -(M3[0][0] * M3[2][1] - M3[0][1] * M3[2][0]);
    cof[2][0] = (M3[0][1] * M3[1][2] - M3[0][2] * M3[1][1]);
    cof[2][1] = -(M3[0][0] * M3[1][2] - M3[0][2] * M3[1][0]);
    cof[2][2] = (M3[0][0] * M3[1][1] - M3[0][1] * M3[1][0]);
    float3 axisW = mul(cof / det, c.d.xyz);
    float len2 = dot(axisW, axisW);
    if (len2 < 1e-12)
        return false; // degenerate cone (fallback clusters): keep
    axisW /= sqrt(len2);
    float3 worldApex = mul(model, float4(c.c.xyz, 1.0)).xyz;
    float3 dvec = worldApex - eye;
    float dl2 = dot(dvec, dvec);
    if (dl2 < 1e-12)
        return false; // eye at the apex: keep (avoids NaN)
    return dot(dvec / sqrt(dl2), axisW) >= c.c.w;
}

// Per-mesh union bound over its clusters (xyz center, w radius, OBJECT
// space — the publisher transforms it by the draw's model exactly like
// ClusterOccluded does with cluster bounds). Static, load-time upload.
struct GfxMeshBounds { float4 centerRadius[GFX_MAX_MESHES]; };

// ---------------------------------------------------------------------------
// Conservative bound projection + HZB occlusion (moved here from
// gfxfilter.cs.hlsl so the GPU cull chain's draw-level test
// (gfxdrawcull.cs.hlsl) reuses the exact filter math; every derivation
// comment lives with the functions, both callers share them verbatim).
// ---------------------------------------------------------------------------

// HZB context: the projection/occlusion inputs every ProjectBound /
// BoundHzbOccluded caller supplies (the filter builds it from its push;
// the draw cull from its own; the extents are the HZB target's).
struct GfxHzbCtx
{
    float4x4 viewProj;
    float3 eye;
    uint width;
    uint height;
    uint mips;
    vk::BufferPointer<HzbBuffer> hzb;
};

// Conservative interval for n / w when:
//   n in [nCenter - nRadius, nCenter + nRadius]
//   w in [wMin, wMax], with wMin > 0.
//
// Treating numerator/denominator intervals independently is deliberately
// conservative. The returned interval may be wider than the true projected
// sphere, but it must not be narrower.
float2 ConservativeRatioInterval(
    float nCenter,
    float nRadius,
    float wMin,
    float wMax)
{
    float nMin = nCenter - nRadius;
    float nMax = nCenter + nRadius;

    float r0 = nMin / wMin;
    float r1 = nMin / wMax;
    float r2 = nMax / wMin;
    float r3 = nMax / wMax;

    float lo = min(min(r0, r1), min(r2, r3));
    float hi = max(max(r0, r1), max(r2, r3));
    return float2(lo, hi);
}

// Projection result shared by the per-cluster occlusion test, the
// draw-level cull test (gfxdrawcull) and the texture-demand publisher
// (one projection core, three callers).
struct BoundProjection
{
    bool testable;  // false: eye inside the sphere / near-plane cross / NaN
                    // -> cull side KEEPS, demand side publishes the sharpest
    bool onTarget;  // false: rect fully off-screen -> cull side keeps
                    // (frustum stages handle it), demand side skips
    float4 rect;   // (sx0, sy0, sx1, sy1) px, clamped + jitter-guard widened
    float dNear;   // conservative nearest reversed-Z depth (0..1)
};

// Conservative projection of an object-space bound sphere (xyz center, w
// radius) by `model` into the framebuffer rect + nearest depth. All
// failure paths return testable=false — every caller's failure direction
// is the conservative one (keep geometry / over-request texture detail).
//
// Deviations-from-exactness notes:
//   - Max basis length turns the object-space sphere into a conservative
//     world-space sphere even under non-uniform scale.
//   - Treating numerator/denominator intervals independently widens the
//     projected rect (deliberately conservative; must not be narrower).
//   - dNear may be slightly nearer than the true sphere (safe: reduces
//     culling; on the demand side it can only over-request).
BoundProjection ProjectBound(float4 bound, float4x4 model, GfxHzbCtx ctx)
{
    BoundProjection p;
    p.testable = false;
    p.onTarget = false;
    p.rect = float4(0.0f, 0.0f, 0.0f, 0.0f);
    p.dNear = 0.0f;

    float width = (float) ctx.width;
    float height = (float) ctx.height;
    if (!(width > 0.0f) || !(height > 0.0f))
        return p; // no extent: untestable

    // World-space bounding sphere.
    float3 wc = mul(model, float4(bound.xyz, 1.0f)).xyz;
    // Explicit columns (._RC is row/column by position, independent of the
    // matrix majorness): basis vectors for the world-sphere scale.
    float bs = max(
        length(float3(model._11, model._21, model._31)),
        max(length(float3(model._12, model._22, model._32)),
            length(float3(model._13, model._23, model._33))));
    float wr = bound.w * bs;

    if (!(wr >= 0.0f))
        return p; // catches NaN

    // Eye inside the sphere -> never occlusion-cull / always sharpest.
    float3 toEye = ctx.eye - wc;
    float dist = length(toEye);
    if (!(dist > wr))
        return p;

    float4 clipC = mul(ctx.viewProj, float4(wc, 1.0f));

    // For p = wc + delta, |delta| <= wr: each clip component varies by at
    // most wr * length(row_i.xyz). Rows are spelled out with ._RC
    // accessors (row/column by position, independent of matrix majorness):
    // with the push uploaded untransposed, viewProj[i] would be column
    // i, not row i, so single-subscript indexing must not be used here.
    float xRadius = wr * length(float3(ctx.viewProj._11, ctx.viewProj._12, ctx.viewProj._13));
    float yRadius = wr * length(float3(ctx.viewProj._21, ctx.viewProj._22, ctx.viewProj._23));
    float zRadius = wr * length(float3(ctx.viewProj._31, ctx.viewProj._32, ctx.viewProj._33));
    float wRadius = wr * length(float3(ctx.viewProj._41, ctx.viewProj._42, ctx.viewProj._43));

    float wMin = clipC.w - wRadius;
    float wMax = clipC.w + wRadius;

    // Sphere touches/crosses the camera plane, or invalid math.
    if (!(wMin > 1e-6f) || !(wMax >= wMin))
        return p;

    // Conservative projected NDC intervals for the complete world sphere.
    float2 ndcX = ConservativeRatioInterval(
        clipC.x, xRadius, wMin, wMax);
    float2 ndcY = ConservativeRatioInterval(
        clipC.y, yRadius, wMin, wMax);

    // Reversed-Z nearest depth = maximum projected z/w over the sphere.
    float2 ndcZ = ConservativeRatioInterval(
        clipC.z, zRadius, wMin, wMax);
    float dNear = ndcZ.y;

    // NaN / invalid depth / near-plane intersection -> keep.
    if (!(dNear >= 0.0f))
        return p;
    if (dNear > 1.0f)
        return p;

    // Framebuffer coordinates. The projection already contains the Y flip,
    // so no additional texture-space flip is applied.
    float x0 = (ndcX.x * 0.5f + 0.5f) * width;
    float x1 = (ndcX.y * 0.5f + 0.5f) * width;
    float y0 = (ndcY.x * 0.5f + 0.5f) * height;
    float y1 = (ndcY.y * 0.5f + 0.5f) * height;

    if (!(x1 >= x0) || !(y1 >= y0))
        return p;

    // Completely off-target: leave this to the frustum stages (cull side
    // keeps; the demand side skips publication).
    if (x1 < 0.0f || x0 > width || y1 < 0.0f || y0 > height) {
        p.testable = true;
        p.onTarget = false;
        return p;
    }

    // Clamp only after the off-target test, then widen by the jitter guard
    // (conservative: more overlapped texels must ALL occlude, so this only
    // keeps; both jitter phases test a superset and agree instead of
    // flickering boundary clusters).
    float sx0 = clamp(clamp(x0, 0.0f, width) - GFX_CULL_GUARD_PX, 0.0f, width);
    float sx1 = clamp(clamp(x1, 0.0f, width) + GFX_CULL_GUARD_PX, 0.0f, width);
    float sy0 = clamp(clamp(y0, 0.0f, height) - GFX_CULL_GUARD_PX, 0.0f, height);
    float sy1 = clamp(clamp(y1, 0.0f, height) + GFX_CULL_GUARD_PX, 0.0f, height);

    p.testable = true;
    p.onTarget = true;
    p.rect = float4(sx0, sy0, sx1, sy1);
    p.dNear = dNear;
    return p;
}

// HZB verdict for a projected bound: true = provably fully occluded (the
// nearest conservative depth sits behind EVERY covered MIN-reduced texel).
// Empty footprints/ranges keep (callers decide their own keep direction).
bool BoundHzbOccluded(BoundProjection p, GfxHzbCtx ctx)
{
    const float rectW = max(p.rect.z - p.rect.x, 0.0f);
    const float rectH = max(p.rect.w - p.rect.y, 0.0f);

    // Empty / subnormal projected footprint: keep.
    if (!(rectW > 0.0f) || !(rectH > 0.0f))
        return false;

    // Choose the first mip whose nominal source footprint covers the
    // rectangle's longest axis.
    uint level = 0;
    float texel = 1.0f;
    float rectMax = max(rectW, rectH);

    while (texel < rectMax && level + 1u < ctx.mips)
    {
        texel *= 2.0f;
        ++level;
    }

    // Packed HZB mip dimensions + float offset.
    // Must match the ceil-halved HZB builder/layout (hzb.cs.hlsl clamped
    // 2x2 + HzbMipDims: dst = max((src + 1u) >> 1, 1u)).
    uint lw = ctx.width;
    uint lh = ctx.height;
    uint off = 0;

    for (uint k = 0; k < level; ++k)
    {
        off += lw * lh;
        lw = max((lw + 1u) >> 1, 1u);
        lh = max((lh + 1u) >> 1, 1u);
    }

    if (lw == 0u || lh == 0u)
        return false;

    // Ceil-halved chain covers the full extent (lw * texel >= width and
    // lh * texel >= height; the builder's clamped 2x2 folds the odd edge
    // into the last texel), so there is no dropped row/column and no
    // represented-extent early-out. A floor-halved chain would need one.

    uint tx0 = (uint) clamp(
        floor(p.rect.x / texel), 0.0f, (float) (lw - 1u));
    uint tx1 = (uint) clamp(
        floor(p.rect.z / texel), 0.0f, (float) (lw - 1u));
    uint ty0 = (uint) clamp(
        floor(p.rect.y / texel), 0.0f, (float) (lh - 1u));
    uint ty1 = (uint) clamp(
        floor(p.rect.w / texel), 0.0f, (float) (lh - 1u));

    // Empty range must NEVER mean "occluded".
    if (tx0 > tx1 || ty0 > ty1)
        return false;

    // Tiny conservative reversed-Z bias. Moving dNear slightly toward the
    // camera makes borderline comparisons fail toward VISIBLE.
    float depthBias = max(1e-7f, abs(p.dNear) * 1e-5f);
    float testDepth = p.dNear + depthBias;

    // MIN HZB under reversed-Z:
    //
    // stored = farthest/smallest depth in that coarse texel.
    //
    // The sphere is occluded only when its nearest possible depth is
    // strictly behind EVERY overlapped HZB texel.
    //
    // Do not cap this to an assumed 2x2 range. The chosen mip normally
    // produces <=2 samples/axis, but sampling the actual range is safer.
    for (uint ty = ty0; ty <= ty1; ++ty)
    {
        for (uint tx = tx0; tx <= tx1; ++tx)
        {
            float stored = ctx.hzb.Get().texels[off + ty * lw + tx];

            // Reversed-Z: larger means closer.
            // If the sphere is at or in front of this conservative depth,
            // it is not fully occluded.
            if (testDepth >= stored)
                return false;
        }
    }

    return true;
}

// Draw-level texture-demand source (replaces the retired per-fragment
// TS_ReportUsage feedback at ~1/1000 the publication traffic):
//
//   visible draw (designated publisher group)
//     -> material id (GfxDraw.misc2.y)
//     -> MaterialValues tex/tex2.x = 5 bindless slots
//     -> conservative projected rect (same math as ClusterOccluded, but on
//        the mesh sphere — containment guarantees a draw-culled bound
//        implies every cluster bound culled, so skipping publication is
//        never a missed demand, only an over-request)
//     -> per-slot wanted FULL-CHAIN mip (mirror of
//        TextureStreamer::WantedMipForScreenCoverage, incl. mipBias +
//        clamp to fullMips-1)
//     -> InterlockedMin(demand[slot], want) — one uint per bindless slot,
//        0xFFFFFFFF = "no draw reported this texture this frame".
//
// The CPU consumes the ring slice every 4th frame (batched with the
// exposure/DS readback ticket), skips 0xFFFFFFFF, and forwards via
// TextureStreamer::RequestMip(slotMap[slot], wantedFullMip, frame) —
// full-chain mips keep residency state out of this shader entirely.
struct GfxTextureMeta
{
    uint longSide;  // max(baseWidth, baseHeight) texels
    uint fullMips; // 0 = slot holds no streamed texture (skip)
};
struct GfxTextureMetaTable { GfxTextureMeta meta[GFX_MAX_TEXTURES]; };
struct GfxDemand { uint demand[GFX_MAX_TEXTURES]; };

struct GfxFilterPush
{
    vk::BufferPointer<GfxPositions> positions;
    vk::BufferPointer<GfxIndices>   indices;
    vk::BufferPointer<GfxClusters>  clusters;
    vk::BufferPointer<GfxBatches>   batches;
    vk::BufferPointer<GfxDraws>      draws;
    vk::BufferPointer<GfxFiltered>   filtered;
    vk::BufferPointer<GfxIndirect>   indirect;
    uint2 pad0; // 8 bytes: keeps viewProj 16B-aligned (raw uint64 would emit Capability Int64)
    float4x4 viewProj;
    float3 eye;
    float pad1;
    uint cullFlags;
    uint width;
    uint height;
    uint batchCount;
    // Cull chain: kept-bit parity (0/1) for this frame's kept region
    // lookup.
    uint keptParity;
    uint pad2;
    // T4 occlusion/contribution inputs (0/empty disables the tests).
    vk::BufferPointer<HzbBuffer> hzb;
    uint hizWidth;
    uint hizHeight;
    uint hizMips;
    float minContribution; // px^2 footprint threshold (0 = off)
    // Filtered-output region base (uint32 indices into the static index
    // monoblock) for this dispatch's flight slice. Indirect firstIndex is
    // written as indexBase + outputOffset (absolute), so draws address the
    // monoblock directly and the index binding never switches buffers.
    uint indexBase;
    uint pad3;
    // Draw-level texture-demand inputs (0/empty disables the publisher in
    // the shader; PushAndDispatch zero-gates the whole set on any null
    // BDA — same belt-and-braces rule as the HZB inputs above).
    vk::BufferPointer<GfxMeshBounds> meshBounds;      // per-mesh union bounds
    vk::BufferPointer<MaterialsBuffer> materials;    // THIS frame's slice
    vk::BufferPointer<GfxTextureMetaTable> textureMeta; // per-bindless-slot meta
    vk::BufferPointer<GfxDemand> textureDemand;      // this flight's ring slice
    uint textureMetaCount; // live slot count (upper bound for slots/atomics)
    float mipBias;        // mirrors TextureStreamerConfig::mipBias
    // Cull chain inputs (always the kept-bit region + cullState):
    //   kept      — this frame's kept region base (the CPU adds the parity
    //               region offset to the BDA; the shader indexes from it
    //               directly)
    //   cullState — draw verdicts (gfxdrawcull) + the filtered-offset
    //               counter (word GFX_CULL_STATE_COUNTER) this dispatch
    //               allocates through atomically
    vk::BufferPointer<GfxKeptBits> kept;
    vk::BufferPointer<GfxCullState> cullState;
};
//static_assert(sizeof(GfxFilterPush) == 256);

// ---------------------------------------------------------------------------
// GPU cull chain pushes (C++ mirrors: gfx_filter.h BatchCullPush /
// DrawCullPush, static_asserted sizes).
// ---------------------------------------------------------------------------

// GfxBatchCull: one 128-thread workgroup per material-sorted draw; threads
// stride the draw's mesh clusters. Runs FIRST in the frame (before the seed
// pass — the seed/remainder commands it writes gate the seed depth). LIVE
// view (seed/remainder semantics; the frozen view belongs to the filter
// legs). Writes per candidate slot: batches, indirect statics, seed/remainder
// commands (instanceCount = temporal verdicts against the prev kept region),
// kept bits (current parity region).
struct GfxBatchCullPush
{
    vk::BufferPointer<GfxDraws> draws;        // this frame (misc2.z = candidate slot base)
    vk::BufferPointer<GfxMeshes> meshes;       // static per-mesh cluster ranges
    vk::BufferPointer<GfxClusters> clusters;   // static (bounds + cones)
    vk::BufferPointer<GfxMeshBounds> meshBounds; // static (draw-level gate)
    vk::BufferPointer<GfxBatches> batches;     // out
    vk::BufferPointer<GfxIndirect> indirect;   // out (static parts)
    vk::BufferPointer<GfxSeedCmds> seedCmds;   // out
    vk::BufferPointer<GfxSeedCmds> remCmds;    // out
    vk::BufferPointer<GfxKeptBits> kept;       // out (parity region) + in (prev region)
    vk::BufferPointer<GfxCullState> cullState; // out: offset-counter zero (group 0)
    float4x4 viewProj;   // LIVE view (seed/remainder semantics)
    float3 eye;
    float pad1;
    uint drawCount;
    uint drawSetMask;    // TemporalView::drawSetMask (seed/remainder bucket gate)
    uint clusterIndexBase; // absolute firstIndex base for seed/remainder commands
    uint keptParity;    // 0/1: the region this frame writes (prev = 1-parity)
    uint width;         // guard-band extent (LIVE view)
    uint height;
    // Deformed per-cluster bounds (GfxSkinBounds output, one GfxClusterData
    // per candidate slot): skinned draws cull against THIS instead of the
    // static clusters[]. skinBoundsValid = 0 leaves every draw static.
    vk::BufferPointer<GfxClusters> skinBounds;
    uint skinBoundsValid;
    uint pad2;
    uint2 pad3;
};
//static_assert(sizeof(GfxBatchCullPush) == 208);

// GfxSkinBounds: one 128-thread workgroup per draw (the GfxBatchCull dispatch
// shape); threads stride the draw's clusters and rebuild each SKINNED
// cluster's object-space bounding sphere from the frame's deformed heap, so
// the batch cull sees the ANIMATED silhouette (the static clusters[] stays
// bind-pose). Writes into skinBounds (indexed by candidate slot). C++ mirror:
// gfx_filter.h State::skinBounds* / gfx_filter.cpp PushAndDispatchSkinBounds.
struct GfxSkinBoundsPush
{
    vk::BufferPointer<GfxDraws> draws;      // this frame (misc2.z = slot base)
    vk::BufferPointer<GfxMeshes> meshes;    // static per-mesh cluster ranges
    vk::BufferPointer<GfxClusters> clusters; // static (bind-pose a/b/c/d)
    vk::BufferPointer<GfxIndices> indices;  // cluster-ordered global ids (clusterIndexBase baked in)
    vk::BufferPointer<GfxSkinDeformedHeap> deformed; // this frame's deformed heap
    vk::BufferPointer<GfxClusters> skinBounds; // out (per candidate slot)
    uint deformedBase;  // 0xFFFFFFFF = none
    uint drawCount;
    uint pad0;
    uint pad1;
};

// GfxDrawCull: one thread per draw; the Northlight-style mesh-bound fast
// discard against the FROZEN cull view (the gfx view record matrix — held
// while the debug culling freeze holds the filter) + the HZB occlusion test
// (the same ProjectBound/BoundHzbOccluded math the demand publisher uses on
// mesh bounds; containment makes the verdict a strict fast-reject: a draw
// that fails here has every cluster failing the filter's per-cluster tests).
// Runs after GfxHiz, before the filter dispatch.
struct GfxDrawCullPush
{
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<GfxMeshBounds> meshBounds; // static (object-space union bounds)
    vk::BufferPointer<GfxCullState> cullState;   // out: verdict words [0, drawCount)
    vk::BufferPointer<HzbBuffer> hzb;
    float4x4 viewProj;  // FROZEN view (the gfx view record matrix)
    float3 eye;
    float pad1;
    uint cullFlags;    // GFX_CULL_OCCLUSION | GFX_CULL_HZBSKIPMASKED consumers
    uint drawCount;
    uint width;         // guard-band extent (== HZB target)
    uint height;
    uint hizMips;
    uint pad2;
};
//static_assert(sizeof(GfxDrawCullPush) == 136);
