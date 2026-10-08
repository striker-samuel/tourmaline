// Task-less mesh-shader gfx filter (main-view opaque path).
//
// ONE mesh workgroup per cluster HALF of a CPU-partitioned batch list:
// the record dispatches DrawMeshTasks(2 * listCount) per FS variant and
// workgroup bx covers list entry bx/2 (batch) and half bx%2. 64 threads,
// one triangle each; halves are <= 64 tris, so the 3-verts-per-triangle
// emission (<= 192 verts, <= 64 prims) fits the 256-vertex mesh output
// limit with NO dedup hash. Batches already had the CPU sphere+cone
// pre-cull (BuildBatches); this shader repeats the per-triangle
// CullTriangle tests (degenerate/backface/frustum/smallprim, verbatim
// port from gfxfilter.cs.hlsl) and compacts via wave prefix sums.
//
// Eligibility lives on the CPU (RecordMeshMain partitions eligible
// batches per FS variant — opaque, single-sided, variant twin exists;
// the indexed loop skips exactly those ranges). The shader trusts its
// list and only bounds-checks. Masked/two-sided/motion/seed/shadow draws
// stay indexed.
//
// Varyings match gfxfiltered.vert.hlsl VSOut EXACTLY (locations 0..6 +
// SV_Position) so this links against the existing lit fragment shaders.
// Vertex fetch mirrors the VS fetch contract (pos heap float3 + 24B
// shading stream) through the params buffer BDAs.
//
// C++ mirror: Engine/Renderer/gfx_filter.h (GfxMeshParams, tail push).

#define GFXMESH_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "gfxmesh.hlsli"

// Single push block [0, 240): the full scene push head (192B,
// preserved) + the SAME 32B tail the indexed VS takes at byte 192
// (draws/renderViews/index/lmXforms — the record issues the identical
// PushViewConstants call) + the 16B mesh tail at byte 224 (shared
// params address + this dispatch's list slice).
struct GfxMeshPush
{
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<RenderViewBuffer> renderViews;
    uint renderViewIndex;
    uint pad;
    vk::BufferPointer<GfxChartXforms> lmXforms;
    vk::BufferPointer<GfxMeshParamsData> params;
    // Mesh tail at byte 224 (record pushes 16B after the VS tail):
    // shared params address + this dispatch's list slice.
    uint listBase;
    uint listCount;
};

[[vk::push_constant]]
ConstantBuffer<GfxMeshPush> pcM;

// Mesh outputs: SV_Position + VSOut locations 0..6 verbatim.
struct MeshVert
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    // Mesh-only: per-draw material for the multi-material dispatch (the
    // mesh FS twin reads this; the classic FS keeps pc.materialId).
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
};

// Per-triangle cull: verbatim port of CullTriangle (gfxfilter.cs.hlsl),
// reading limits from the params blob instead of the filter push.
bool MeshCullTriangle(uint3 tri, float4 clip0, float4 clip1, float4 clip2,
    bool twoSided, uint cullFlags, uint width, uint height)
{
    bool cull = false;

    if ((cullFlags & GFX_CULL_DEGENERATE) != 0)
    {
        if (tri.x == tri.y || tri.y == tri.z || tri.x == tri.z)
            cull = true;
    }

    if ((cullFlags & GFX_CULL_BACKFACE) != 0 && !twoSided)
    {
        if (clip0.w > 0.0f && clip1.w > 0.0f && clip2.w > 0.0f)
        {
            float3x3 m = float3x3(clip0.xyw, clip1.xyw, clip2.xyw);
            bool back = determinant(m) > 0.0f;
            if ((cullFlags & GFX_CULL_FLIPWINDING) != 0)
                back = !back;
            cull = cull || back;
        }
    }

    if ((cullFlags & GFX_CULL_FRUSTUM) != 0)
    {
        float4 c[3] = { clip0, clip1, clip2 };
        bool allBehind = c[0].w < 0.0f && c[1].w < 0.0f && c[2].w < 0.0f;
        bool anyBehind = c[0].w <= 0.0f || c[1].w <= 0.0f || c[2].w <= 0.0f;
        if (allBehind)
        {
            cull = true;
        }
        else if (!anyBehind)
        {
            const float guardX = GFX_CULL_GUARD_PX * 2.0f / max((float) width, 1.0f);
            const float guardY = GFX_CULL_GUARD_PX * 2.0f / max((float) height, 1.0f);
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

    if ((cullFlags & GFX_CULL_SMALLPRIM) != 0 && !cull)
    {
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
                float2 ss = ndc[i] * float2((float) width, (float) height);
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

[outputtopology("triangle")]
[numthreads(GFX_MESH_THREADS, 1, 1)]
void mainMS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
    out vertices MeshVert verts[GFX_MESH_MAX_VERTS],
    out indices uint3 prims[GFX_MESH_MAX_TRIS])
{
    GfxMeshParamsData P = pcM.params.Get();
    // CPU-partitioned list: entry id/2 is the batch, id%2 the half.
    // The list holds exactly this dispatch's variant batches (see
    // RecordMeshMain), so no eligibility test here — only bounds.
    const uint li = gid.x / GFX_MESH_HALVES;
    const uint half = gid.x % GFX_MESH_HALVES;

    uint4 header = uint4(0, 0, 0, 0);
    GfxDraw draw;
    draw.model = float4x4(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    draw.misc = uint4(0, 0, 0, 0);
    draw.misc2 = uint4(0, 0, 0, 0);
    bool gated = true;
    uint triBase = 0;
    uint halfTris = 0;
    if (li < pcM.listCount)
    {
        const uint batch = P.batchList.Get().batch[pcM.listBase + li];
        header = P.batches.Get().batch[batch];
        draw = P.draws.Get().draws[header.y];
        uint triCount = P.clusters.Get().cluster[header.x].a.y;
        uint halfStart = half * GFX_MESH_MAX_TRIS;
        if (halfStart < triCount)
        {
            gated = false;
            triBase = P.clusters.Get().cluster[header.x].a.x + halfStart;
            halfTris = min(GFX_MESH_MAX_TRIS, triCount - halfStart);
        }
    }

    // Per-lane triangles (2 each): fetch indices + clip positions, cull.
    // Wave-scoped prefix sums below span the whole group: 32 lanes.
    bool survive[GFX_MESH_TRIS_PER_LANE];
    uint3 tri[GFX_MESH_TRIS_PER_LANE];
    float4 clip[GFX_MESH_TRIS_PER_LANE][3];
    bool twoSided = (draw.misc.y & GFX_DRAW_TWOSIDED) != 0;
    [unroll]
    for (int r = 0; r < GFX_MESH_TRIS_PER_LANE; ++r)
    {
        survive[r] = false;
        tri[r] = uint3(0, 0, 0);
        clip[r][0] = clip[r][1] = clip[r][2] = float4(0, 0, 0, 0);
        uint ti = gtid.x + r * GFX_MESH_THREADS;
        if (!gated && ti < halfTris)
        {
            uint baseIndex = triBase + ti * 3;
            tri[r] = uint3(
                P.indices.Get().idx[baseIndex + 0],
                P.indices.Get().idx[baseIndex + 1],
                P.indices.Get().idx[baseIndex + 2]);
            [unroll]
            for (int k = 0; k < 3; ++k)
            {
                uint vi = (k == 0) ? tri[r].x : ((k == 1) ? tri[r].y : tri[r].z);
                uint wbase = vi * 3;
                float3 p = float3(
                    asfloat(P.positions.Get().words[wbase + 0]),
                    asfloat(P.positions.Get().words[wbase + 1]),
                    asfloat(P.positions.Get().words[wbase + 2]));
                clip[r][k] = mul(P.viewProj, mul(draw.model, float4(p, 1.0f)));
            }
            survive[r] = !MeshCullTriangle(tri[r], clip[r][0], clip[r][1], clip[r][2],
                twoSided, P.cullFlags, P.width, P.height);
        }
    }

    // Compact surviving triangles densely (wave-uniform counts: the
    // group IS one wave, so both rounds share one dense slot space).
    uint slot0 = WavePrefixCountBits(survive[0]);
    uint off1 = WaveActiveCountBits(survive[0]);
    uint slot1 = off1 + WavePrefixCountBits(survive[1]);
    uint primCount = off1 + WaveActiveCountBits(survive[1]);
    SetMeshOutputCounts(primCount * 3, primCount);

    float3x3 M3 = (float3x3) draw.model;
    float det = M3[0][0] * (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1])
              - M3[0][1] * (M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0])
              + M3[0][2] * (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);
    [unroll]
    for (int r = 0; r < GFX_MESH_TRIS_PER_LANE; ++r)
    {
        if (!survive[r])
            continue;
        uint primSlot = (r == 0) ? slot0 : slot1;
        uint3 triR = tri[r];
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            uint vi = (k == 0) ? triR.x : ((k == 1) ? triR.y : triR.z);
            uint wbase = vi * 3;
            float3 p = float3(
                asfloat(P.positions.Get().words[wbase + 0]),
                asfloat(P.positions.Get().words[wbase + 1]),
                asfloat(P.positions.Get().words[wbase + 2]));
            uint sbase = vi * 6;
            uint cW = P.shading.Get().words[sbase + 0];
            uint uvW = P.shading.Get().words[sbase + 1];
            uint chUW = P.shading.Get().words[sbase + 2];
            uint nW = P.shading.Get().words[sbase + 3];
            uint tW = P.shading.Get().words[sbase + 4];
            uint chW = P.shading.Get().words[sbase + 5];

            MeshVert v;
            float4 world = mul(draw.model, float4(p, 1.0f));
            v.worldPos = world.xyz;
            v.pos = mul(P.viewProj, world);
            v.color = float4(
                (float)(cW & 0xFFu),
                (float)((cW >> 8) & 0xFFu),
                (float)((cW >> 16) & 0xFFu),
                (float)((cW >> 24) & 0xFFu)) * (1.0f / 255.0f);
            v.uv = float2(f16tof32(uvW & 0xFFFFu), f16tof32((uvW >> 16) & 0xFFFFu));
            float2 chartUv = float2(
                (float)(chUW & 0xFFFFu),
                (float)((chUW >> 16) & 0xFFFFu)) * (1.0f / 65535.0f);
            v.lightmapUv = (draw.misc.w == 0xFFFFFFFFu)
                ? float2(0.0f, 0.0f)
                : GfxLightmapUv(chartUv, chW, draw.misc2.x, P.lmXforms);
            v.lmChunk = draw.misc.w;
            v.meshMaterial = draw.misc2.y;

            float3 normal = OctDecodeNormal(nW);
            float4 tangent = OctDecodeTangent(tW);
            float3 worldNormal;
            if (abs(det) > 1e-12f)
            {
                float3x3 adj;
                adj[0][0] = (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1]);
                adj[0][1] = -(M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0]);
                adj[0][2] = (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);
                adj[1][0] = -(M3[0][1] * M3[2][2] - M3[0][2] * M3[2][1]);
                adj[1][1] = (M3[0][0] * M3[2][2] - M3[0][2] * M3[2][0]);
                adj[1][2] = -(M3[0][0] * M3[2][1] - M3[0][1] * M3[2][0]);
                adj[2][0] = (M3[0][1] * M3[1][2] - M3[0][2] * M3[1][1]);
                adj[2][1] = -(M3[0][0] * M3[1][2] - M3[0][2] * M3[1][0]);
                adj[2][2] = (M3[0][0] * M3[1][1] - M3[0][1] * M3[1][0]);
                worldNormal = mul(adj / det, normal);
            }
            else
            {
                worldNormal = mul(M3, normal);
            }
            v.normal = normalize(worldNormal);
            float3 worldTangent = mul(M3, tangent.xyz);
            float handedness = tangent.w * (det < 0.0f ? -1.0f : 1.0f);
            v.tangent = float4(normalize(worldTangent), handedness);

            verts[primSlot * 3 + k] = v;
        }
        prims[primSlot] = uint3(primSlot * 3 + 0, primSlot * 3 + 1, primSlot * 3 + 2);
    }
}
