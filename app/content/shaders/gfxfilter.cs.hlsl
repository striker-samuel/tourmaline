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
// C++ mirror: Engine/Renderer/gfx_filter.h (GfxFilterPush, 168 bytes).

#define GFXFILTER_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

[[vk::push_constant]]
ConstantBuffer<GfxFilterPush> pcF;

groupshared uint gLocalCount;
groupshared uint gClusterCull; // T4: 1 = whole cluster dropped (occlusion/contribution)

// T4 per-cluster occlusion + contribution (Alan Wake 2 style), evaluated
// once per workgroup (thread 0). Facing-agnostic: also applies to
// two-sided draws. Mirrors the CPU ProjectClusterFootprint /
// HzbMipForRect / HzbOccluded helpers (same formulas, same conservatism:
// eye-inside/behind-camera/off-target keep; all overlapped texels at the
// covering mip must occlude; reversed-Z hidden iff d_near < stored closest).
bool ClusterOccluded(uint clusterIdx, uint drawIdx)
{
    uint flags = pcF.cullFlags;
    bool wantOcc = (flags & GFX_CULL_OCCLUSION) != 0;
    bool wantCon = (flags & GFX_CULL_CONTRIBUTION) != 0;
    if (!wantOcc && !wantCon)
        return false;
    if (pcF.hizMips == 0)
        return false; // no pyramid (CPU force-disables with hzb == 0)
    float width = (float)pcF.hizWidth;
    float height = (float)pcF.hizHeight;
    if (width <= 0.0f || height <= 0.0f)
        return false;
    float4 bounds = pcF.clusters.Get().cluster[clusterIdx].b; // center, radius
    float4x4 model = pcF.draws.Get().draws[drawIdx].model;
    // World sphere (max-basis uniform-scale approx, mirrors the CPU cone
    // path's MaxBasisScale).
    float3 wc = mul(model, float4(bounds.xyz, 1.0f)).xyz;
    float bs = max(length(model[0].xyz),
        max(length(model[1].xyz), length(model[2].xyz)));
    float wr = bounds.w * bs;
    float3 toEye = pcF.eye - wc;
    float dist = length(toEye);
    if (dist <= wr)
        return false; // eye inside the sphere: keep
    float4 clipC = mul(pcF.viewProj, float4(wc, 1.0f));
    if (clipC.w <= 0.0f)
        return false;
    float3 nearest = wc + (toEye / dist) * wr;
    float4 clipN = mul(pcF.viewProj, float4(nearest, 1.0f));
    if (clipN.w <= 0.0f)
        return false;
    // Framebuffer y-down == NDC-down post-Y-flip (cf. forwardplus.cs.hlsl).
    float cx = (clipC.x / clipC.w * 0.5f + 0.5f) * width;
    float cy = (clipC.y / clipC.w * 0.5f + 0.5f) * height;
    float fx = pcF.viewProj[0][0];
    float fy = abs(pcF.viewProj[1][1]);
    // Extent from the nearest-point depth (clipN.w <= dist): radial dist
    // underestimates off-axis footprints (smaller rect, finer mip) and
    // over-culls. Nearest depth overestimates -> conservative.
    float depthForExtent = max(clipN.w, 1e-6f);
    float rx = wr * fx / depthForExtent * (width * 0.5f);
    float ry = wr * fy / depthForExtent * (height * 0.5f);
    if (wantCon && pcF.minContribution > 0.0f
        && (rx * 2.0f) * (ry * 2.0f) < pcF.minContribution)
        return true;
    if (!wantOcc)
        return false;
    // Rect missing the target: the frustum stages own this case.
    if (cx + rx < 0.0f || cx - rx > width || cy + ry < 0.0f || cy - ry > height)
        return false;
    // Mip whose texel covers the rect (ceil(log2(max))).
    uint level = 0;
    float texel = 1.0f;
    float rectMax = max(rx * 2.0f, ry * 2.0f);
    while (texel < rectMax && level + 1 < pcF.hizMips)
    {
        texel *= 2.0f;
        ++level;
    }
    // Mip dims + float offset (mirrors HzbMipDims/HzbMipOffsetFloats).
    uint lw = pcF.hizWidth, lh = pcF.hizHeight, off = 0;
    for (uint k = 0; k < level; ++k)
    {
        off += lw * lh;
        lw = max(lw >> 1, 1u);
        lh = max(lh >> 1, 1u);
    }
    // Conservative cover: the rect can straddle up to 2x2 texels at the
    // covering mip (texel >= rect, but alignment is arbitrary). A single
    // center texel over-culls whenever the center is occluded but the
    // edges see far texels. Hidden only if EVERY overlapped texel occludes.
    float dNear = clipN.z / clipN.w; // reversed-Z value of nearest point
    uint tx0 = (uint)clamp(floor((cx - rx) / texel), 0.0f, (float)(lw - 1u));
    uint tx1 = (uint)clamp(floor((cx + rx) / texel), 0.0f, (float)(lw - 1u));
    uint ty0 = (uint)clamp(floor((cy - ry) / texel), 0.0f, (float)(lh - 1u));
    uint ty1 = (uint)clamp(floor((cy + ry) / texel), 0.0f, (float)(lh - 1u));
    // Bounded: rect <= texel guarantees at most 2 per axis; cap anyway.
    for (uint ty = ty0; ty <= ty1 && ty < ty0 + 3u; ++ty)
    {
        for (uint tx = tx0; tx <= tx1 && tx < tx0 + 3u; ++tx)
        {
            float stored = pcF.hzb.Get().texels[off + ty * lw + tx];
            if (dNear >= stored)
                return false;
        }
    }
    return true;
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
            bool outL = true, outR = true, outB = true, outT = true;
            bool outN = true, outF = true;
            [unroll]
            for (int i = 0; i < 3; ++i)
            {
                outL = outL && (c[i].x < -c[i].w);
                outR = outR && (c[i].x >  c[i].w);
                outB = outB && (c[i].y < -c[i].w);
                outT = outT && (c[i].y >  c[i].w);
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
                float2 ss = ndc[i] * float2((float)pcF.width, (float)pcF.height);
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

    // Batch header: (clusterIndex, drawIndex, outputOffset, unused).
    uint4 header = (uint4)0;
    if (gid < pcF.batchCount)
        header = pcF.batches.Get().batch[gid];

    uint2 cluster = (uint2)0;
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
    GroupMemoryBarrierWithGroupSync();

    bool cull = true;
    uint slot = 0;
    uint3 tri = (uint3)0;
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
    if (gid < pcF.batchCount && tid == 0)
    {
        pcF.indirect.Get().cmds[gid].indexCount = gLocalCount;
        pcF.indirect.Get().cmds[gid].firstIndex = header.z;
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
