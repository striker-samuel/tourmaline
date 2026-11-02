// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Single-pass downsampler for the dedicated volumetric sun-shadow depth.
// Reuses the HZB SPD workgroup/LDS/last-group algorithm, but publishes a
// true R32_Float image mip chain instead of a BDA buffer pyramid. The
// depth attachment remains D32 and is rendered ONCE; this dispatch
// copies mip 0 and MIN-reduces every remaining level down to 1x1.
//
// MIN preserves near occluders in the standard-Z shadow map (0=near,
// 1=far/clear). The fullscreen fog shader samples this chain with a
// footprint-derived LOD. Mip counts and storage-view indices are from
// CascadedShadowAtlas::AddVolumetricShadowSpdPass.
//
// The last group uses the verified HZB SPD device-scope release/counter/
// acquire protocol before reading other groups' stored mip 6 pixels.

#define HZB_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

struct FogShadowSpdCounter { uint completed; };
struct FogShadowSpdPush
{
    uint depthTextureIndex;
    uint mipIndices[16];
    GPU_PTR(FogShadowSpdCounter) counter;
    uint width;
    uint height;
    uint mipCount;
    uint groupCount;
};
GPU_PUSH(FogShadowSpdPush, pcS);

[[vk::binding(2, 0)]]
RWTexture2D<float> fogSpdImages[];

// ---- tile geometry ------------------------------------------------------

#define SPD_TILE 64u     // mip-0 texels per tile side
#define SPD_THREADS 256u // one lane set per tile (16x16 4x4 blocks)

// LDS cascade regions (row-major per level; the level below is read at
// the level above's 2x2 footprint — separate arrays, never overwritten
// in place, so no read/write race inside a level).
groupshared float gsMip1[32 * 32];
groupshared float gsMip2[16 * 16];
groupshared float gsMip3[8 * 8];
groupshared float gsMip4[4 * 4];
groupshared float gsMip5[2 * 2];
groupshared float gsMip6[1 * 1];
groupshared uint gsLast; // completion-counter result broadcast

// ---- helpers ------------------------------------------------------------

// Standard-Z D32 (0 near / 1 far): preserve the nearest caster.
float ReduceDepth(float a, float b, float c, float d)
{
    return min(min(a, b), min(c, d));
}

// Failed/invalid samples are unoccluded rather than falsely shadowed.
float SanitizeDepth(float z)
{
    return (isnan(z) || isinf(z)) ? 1.0f : saturate(z);
}

// Image-mip dimensions: Vulkan floors each successive dimension.
uint2 SpdMipDims(uint2 wh, uint mip)
{
    [loop]
    for (uint k = 0; k < mip; ++k) {
        wh.x = max(wh.x >> 1u, 1u);
        wh.y = max(wh.y >> 1u, 1u);
    }
    return wh;
}

// Valid-region clamp: how many texels of this tile's level-L region
// exist in the GLOBAL pyramid (the tile may run past the mip edge on
// partial right/bottom tiles). mipExtent - tileOrigin, clamped to
// [0, region] — the int math avoids uint underflow at empty tiles.
uint SpdValid(int mipExtent, int tileOrigin, uint region)
{
    return uint(min(max(mipExtent - tileOrigin, 0), int(region)));
}

// ---- the local cascade levels (mips 2..6) --------------------------------
//
// Generic per-level step: lanes tile the level-L region (regionW^2 <=
// 256 texels, one per lane), reduce the 2x2 footprint from the level
// below's LDS region with the GLOBAL-edge clamp (parent valid extent),
// store the result at the global mip offset and publish it into the
// level's own LDS cell for the next step. Out-of-global-bounds lanes
// still write their LDS cell (finite garbage): an existing texel's
// footprint only ever touches existing texels (ceil-division proof in
// the header), so garbage cells are never read — but they must be
// WRITTEN so no lane reads an uninitialized cell.
#define SPD_LOCAL_LEVEL(SRC, DST, L) \
    { \
        const uint regionW = SPD_TILE >> (L); \
        const uint regionP = SPD_TILE >> ((L) - 1u); \
        if (tid < regionW * regionW) { \
            const uint lx = tid % regionW; \
            const uint ly = tid / regionW; \
            const uint2 dL = SpdMipDims(uint2(pcS.width, pcS.height), (L)); \
            const uint2 dP = SpdMipDims(uint2(pcS.width, pcS.height), (L) - 1u); \
            const uint vw = SpdValid(int(dL.x), int(tile.x * regionW), regionW); \
            const uint vh = SpdValid(int(dL.y), int(tile.y * regionW), regionW); \
            const uint vpx = SpdValid(int(dP.x), int(tile.x * regionP), regionP); \
            const uint vpy = SpdValid(int(dP.y), int(tile.y * regionP), regionP); \
            float r = 0.0f; \
            if (vpx != 0u && vpy != 0u && lx < vw && ly < vh) { \
                /* global clamped 2x2, mapped into the parent region */ \
                const uint x0 = lx * 2u; \
                const uint y0 = ly * 2u; \
                const uint x1 = min(lx * 2u + 1u, vpx - 1u); \
                const uint y1 = min(ly * 2u + 1u, vpy - 1u); \
                r = ReduceDepth(SRC[y0 * regionP + x0], SRC[y0 * regionP + x1], \
                                SRC[y1 * regionP + x0], SRC[y1 * regionP + x1]); \
                const uint gx = tile.x * regionW + lx; \
                const uint gy = tile.y * regionW + ly; \
                fogSpdImages[NonUniformResourceIndex(pcS.mipIndices[L])][uint2(gx, gy)] = r; \
            } \
            DST[ly * regionW + lx] = r; \
        } \
        GroupMemoryBarrierWithGroupSync(); \
    }

[numthreads(SPD_THREADS, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint tid = gtid.x;
    const uint2 tile = gid.xy; // this group's 64x64 mip-0 tile (z == 1)

    // ---- phase 1: mip 0 copy + mip 1 (registers only) ------------------
    // Lane t owns the 4x4 mip-0 block at (bx, by); its 2x2 mip-1 texels
    // reduce straight from the 16 loaded values. The global edge clamp
    // (min(2x+1, extent-1)) can only duplicate the base texel of the
    // footprint (ceil division keeps the base in extent), and the base
    // lives in this lane's own block — so clamping = folding onto the
    // already-loaded value, no cross-lane traffic for mip 1.
    const uint bx = (tid % 16u) * 4u;
    const uint by = (tid / 16u) * 4u;
    const uint tileX0 = tile.x * SPD_TILE;
    const uint tileY0 = tile.y * SPD_TILE;

    bool colValid[4];
    bool rowValid[4];
    float blk[4][4];
    [unroll]
    for (uint j = 0; j < 4u; ++j) {
        rowValid[j] = (tileY0 + by + j) < pcS.height;
        [unroll]
        for (uint i = 0; i < 4u; ++i) {
            colValid[i] = (tileX0 + bx + i) < pcS.width;
            // Mip 0: exact 1:1 sanitized copy of the depth texture (the
            // existing mip-0 pass's contract).
            float v = 0.0f;
            if (colValid[i] && rowValid[j]) {
                v = SanitizeDepth(textures[NonUniformResourceIndex(
                    pcS.depthTextureIndex)]
                    .Load(int3(int(tileX0 + bx + i), int(tileY0 + by + j), 0)).r);
                fogSpdImages[NonUniformResourceIndex(pcS.mipIndices[0])]
                    [uint2(tileX0 + bx + i, tileY0 + by + j)] = v;
            }
            blk[j][i] = v;
        }
    }

    if (pcS.mipCount > 1u) {
        const uint2 d1 = SpdMipDims(uint2(pcS.width, pcS.height), 1u);
        [unroll]
        for (uint v = 0; v < 2u; ++v) {
            [unroll]
            for (uint u = 0; u < 2u; ++u) {
                const uint i0 = u * 2u;
                const uint j0 = v * 2u;
                // Odd-edge fold: duplicate the base texel when +1 is past
                // the global extent (== the min(2x+1, extent-1) clamp).
                const uint i1 = colValid[i0 + 1u] ? (i0 + 1u) : i0;
                const uint j1 = rowValid[j0 + 1u] ? (j0 + 1u) : j0;
                const float r = ReduceDepth(blk[j0][i0], blk[j0][i1],
                    blk[j1][i0], blk[j1][i1]);
                const uint mx = tile.x * 32u + (bx >> 1u) + u;
                const uint my = tile.y * 32u + (by >> 1u) + v;
                if (mx < d1.x && my < d1.y)
                    fogSpdImages[NonUniformResourceIndex(pcS.mipIndices[1])][uint2(mx, my)] = r;
                gsMip1[((by >> 1u) + v) * 32u + (bx >> 1u) + u] = r;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        // ---- phases 2..6: the LDS cascade ------------------------------
        if (pcS.mipCount > 2u) SPD_LOCAL_LEVEL(gsMip1, gsMip2, 2)
        if (pcS.mipCount > 3u) SPD_LOCAL_LEVEL(gsMip2, gsMip3, 3)
        if (pcS.mipCount > 4u) SPD_LOCAL_LEVEL(gsMip3, gsMip4, 4)
        if (pcS.mipCount > 5u) SPD_LOCAL_LEVEL(gsMip4, gsMip5, 5)
        if (pcS.mipCount > 6u) SPD_LOCAL_LEVEL(gsMip5, gsMip6, 6)
    }

    // ---- phase 2: the tail (mips 7..mipCount-1, last group only) -------
    if (pcS.mipCount > 7u) {
        // Publication (release, device scope): every plain mip 0..6 store
        // of this group must be visible device-wide BEFORE the counter
        // bump signals completion. The bump is only the "done" marker —
        // it carries no ordering by itself.
        AllMemoryBarrierWithGroupSync();
        if (tid == 0u) {
            uint prev = 0u;
            InterlockedAdd(pcS.counter.Get().completed, 1u, prev);
            gsLast = prev;
        }
        GroupMemoryBarrierWithGroupSync();
        if (gsLast == pcS.groupCount - 1u) {
            // LAST completed group: acquire everything the others
            // published, then build the tail solo.
            AllMemoryBarrierWithGroupSync();
            // Counter reset for the next build. Safe inside this
            // dispatch: every other group bumped BEFORE this one (the
            // per-location order put ours last) and never touches it
            // again. Cross-dispatch visibility is the next use's
            // barrier/queue ordering, and the C++ side slices the ring
            // per frame in flight besides.
            if (tid == 0u) {
                uint orig = 0u;
                InterlockedExchange(pcS.counter.Get().completed, 0u, orig);
            }
            for (uint L = 7u; L < pcS.mipCount; ++L) {
                const uint2 dL = SpdMipDims(uint2(pcS.width, pcS.height), L);
                const uint2 dP = SpdMipDims(uint2(pcS.width, pcS.height), L - 1u);
                const uint n = dL.x * dL.y;
                for (uint i = tid; i < n; i += SPD_THREADS) {
                    const uint x = i % dL.x;
                    const uint y = i / dL.x;
                    const uint x0 = x * 2u;
                    const uint y0 = y * 2u;
                    const uint x1 = min(x * 2u + 1u, dP.x - 1u);
                    const uint y1 = min(y * 2u + 1u, dP.y - 1u);
                    const uint srcMip = NonUniformResourceIndex(pcS.mipIndices[L - 1u]);
                    const float a = fogSpdImages[srcMip][uint2(x0, y0)];
                    const float b = fogSpdImages[srcMip][uint2(x1, y0)];
                    const float c = fogSpdImages[srcMip][uint2(x0, y1)];
                    const float d = fogSpdImages[srcMip][uint2(x1, y1)];
                    // Tail reads are raw global loads — sanitize the
                    // stored result like the per-mip shaders do (finite
                    // inputs make it a no-op; it bounds any latent bug to
                    // the keep-safe 0).
                    fogSpdImages[NonUniformResourceIndex(pcS.mipIndices[L])][uint2(x, y)]
                        = SanitizeDepth(ReduceDepth(a, b, c, d));
                }
                // The next level reads this level's writes — same
                // workgroup, but global (not groupshared) memory.
                DeviceMemoryBarrierWithGroupSync();
            }
        }
    }
}
