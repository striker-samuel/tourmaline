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

bool CullTriangle(uint3 tri, float4 clip0, float4 clip1, float4 clip2)
{
    uint flags = pcF.cullFlags;
    bool cull = false;

    if ((flags & GFX_CULL_DEGENERATE) != 0)
    {
        if (tri.x == tri.y || tri.y == tri.z || tri.x == tri.z)
            cull = true;
    }

    // Backface in homogeneous coordinates (Olano & Greer "2DH").
    if ((flags & GFX_CULL_BACKFACE) != 0)
    {
        float3x3 m = float3x3(clip0.xyw, clip1.xyw, clip2.xyw);
        bool back = determinant(m) > 0.0f;
        if ((flags & GFX_CULL_FLIPWINDING) != 0)
            back = !back;
        cull = cull || back;
    }

    if ((flags & GFX_CULL_FRUSTUM) != 0)
    {
        float4 c[3] = { clip0, clip1, clip2 };
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
        gLocalCount = 0;
    GroupMemoryBarrierWithGroupSync();

    // Batch header: (clusterIndex, drawIndex, outputOffset, unused).
    uint4 header = (uint4)0;
    if (gid < pcF.batchCount)
        header = pcF.batches.Get().batch[gid];

    uint2 cluster = (uint2)0;
    uint triCount = 0;
    if (gid < pcF.batchCount)
    {
        cluster = pcF.clusters.Get().cluster[header.x].xy;
        triCount = cluster.y;
    }

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
            float3 p = pcF.positions.Get().pos[vi];
            clip[k] = mul(pcF.viewProj, mul(model, float4(p, 1.0f)));
        }
        cull = CullTriangle(tri, clip[0], clip[1], clip[2]);
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
