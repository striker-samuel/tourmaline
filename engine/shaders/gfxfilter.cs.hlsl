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
//     GPU only sees surviving clusters.
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
// Indirect-arg clearing is done CPU-side (zero-fill upload before the pass),
// replacing ClearDrawIndirectArgsCS.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (FilterPush, 200 bytes).

#define GFXFILTER_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

[[vk::push_constant]]
ConstantBuffer<GfxFilterPush> pcF;

groupshared uint gLocalCount;
groupshared uint gClusterCull; // T4: 1 = whole cluster dropped (occlusion/contribution)

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

// Projection result shared by the per-cluster occlusion test and the
// draw-level texture-demand publisher (one projection core, two callers).
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
// Deviations-from-exactness notes (carried over from the old inline
// ClusterOccluded math):
//   - Max basis length turns the object-space sphere into a conservative
//     world-space sphere even under non-uniform scale.
//   - Treating numerator/denominator intervals independently widens the
//     projected rect (deliberately conservative; must not be narrower).
//   - dNear may be slightly nearer than the true sphere (safe: reduces
//     culling; on the demand side it can only over-request).
BoundProjection ProjectBound(float4 bound, float4x4 model)
{
    BoundProjection p;
    p.testable = false;
    p.onTarget = false;
    p.rect = float4(0.0f, 0.0f, 0.0f, 0.0f);
    p.dNear = 0.0f;

    float width = (float) pcF.hizWidth;
    float height = (float) pcF.hizHeight;
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
    float3 toEye = pcF.eye - wc;
    float dist = length(toEye);
    if (!(dist > wr))
        return p;

    float4 clipC = mul(pcF.viewProj, float4(wc, 1.0f));

    // For p = wc + delta, |delta| <= wr: each clip component varies by at
    // most wr * length(row_i.xyz). Rows are spelled out with ._RC
    // accessors (row/column by position, independent of matrix majorness):
    // with the push uploaded untransposed, pcF.viewProj[i] would be column
    // i, not row i, so single-subscript indexing must not be used here.
    float xRadius = wr * length(float3(pcF.viewProj._11, pcF.viewProj._12, pcF.viewProj._13));
    float yRadius = wr * length(float3(pcF.viewProj._21, pcF.viewProj._22, pcF.viewProj._23));
    float zRadius = wr * length(float3(pcF.viewProj._31, pcF.viewProj._32, pcF.viewProj._33));
    float wRadius = wr * length(float3(pcF.viewProj._41, pcF.viewProj._42, pcF.viewProj._43));

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
// Mirrors the old inline ClusterOccluded loop exactly.
bool BoundHzbOccluded(BoundProjection p)
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

    while (texel < rectMax && level + 1u < pcF.hizMips)
    {
        texel *= 2.0f;
        ++level;
    }

    // Packed HZB mip dimensions + float offset.
    // Must match the ceil-halved HZB builder/layout (hzb.cs.hlsl clamped
    // 2x2 + HzbMipDims: dst = max((src + 1u) >> 1, 1u)).
    uint lw = pcF.hizWidth;
    uint lh = pcF.hizHeight;
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
            float stored = pcF.hzb.Get().texels[off + ty * lw + tx];

            // Reversed-Z: larger means closer.
            // If the sphere is at or in front of this conservative depth,
            // it is not fully occluded.
            if (testDepth >= stored)
                return false;
        }
    }

    return true;
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

    BoundProjection p = ProjectBound(
        pcF.clusters.Get().cluster[clusterIdx].b, // xyz center, w radius
        pcF.draws.Get().draws[drawIdx].model);

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

    return BoundHzbOccluded(p);
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
        const BoundProjection p = ProjectBound(
            pcF.meshBounds.Get().centerRadius[d.misc.x], d.model);
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
            if (wantOcc && BoundHzbOccluded(p))
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
    }
    GroupMemoryBarrierWithGroupSync();

    // Batch header: (clusterIndex, drawIndex, outputOffset, flags).
    uint4 header = (uint4) 0;
    if (gid < pcF.batchCount)
        header = pcF.batches.Get().batch[gid];

    uint2 cluster = (uint2) 0;
    uint triCount = 0;
    if (gid < pcF.batchCount)
    {
        cluster = pcF.clusters.Get().cluster[header.x].a.xy;
        triCount = cluster.y;
    }

    // T4: whole-cluster drop computed once (thread 0 has header + table).
    if (gid < pcF.batchCount && tid == 0)
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
    if (gid < pcF.batchCount && tid < triCount)
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

    // This group owns command gid exclusively: plain stores, no atomics.
    // firstIndex is absolute into the static index monoblock (indexBase +
    // output slot); the input-assembly index binding never switches buffers.
    if (gid < pcF.batchCount && tid == 0)
    {
        pcF.indirect.Get().cmds[gid].indexCount = gLocalCount;
        pcF.indirect.Get().cmds[gid].firstIndex = pcF.indexBase + header.z;
        pcF.indirect.Get().cmds[gid].vertexOffset = 0; // indices are heap-global ids
        pcF.indirect.Get().cmds[gid].firstInstance = header.y; // draw id for VS
        pcF.indirect.Get().cmds[gid].instanceCount = 1;
    }

    GroupMemoryBarrierWithGroupSync();
    DeviceMemoryBarrierWithGroupSync();

    if (gid < pcF.batchCount && !cull)
    {
        uint outBase = header.z + slot; // reserved slot + intra-group slot
        pcF.filtered.Get().idx[outBase + 0] = tri.x;
        pcF.filtered.Get().idx[outBase + 1] = tri.y;
        pcF.filtered.Get().idx[outBase + 2] = tri.z;
    }
}
