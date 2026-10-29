// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// SPD-style single-dispatch HZB build (the FidelityFX SPD idea on the
// existing BDA float-buffer pyramid — Phase 1 of the hzb upgrade).
//
// Replaces the ~12 per-mip dispatches (hzb_max.cs.hlsl for the SSR/AO
// MAX pyramid, hzb.cs.hlsl for the culling MIN pyramid; one rendergraph
// compute pass + barrier per level) with ONE dispatch that builds the
// whole chain, WITHOUT changing the representation: same HzbBuffer BDA
// float store, same gfx::HzbMipCount/HzbMipDims/HzbMipOffsetFloats
// layout, same clamped-2x2 reduction footprints, same sanitize
// contract. Consumers (SSR trace, AO chain, later the gfx cull) are
// untouched — the A/B contract is byte-identical pyramid contents.
//
// Tile geometry (AMD SPD): 256 lanes, one workgroup owns a 64x64 region
// of mip 0. Lane t owns a 4x4 mip-0 block -> 2x2 mip-1 texels (reduced
// from registers, no LDS). Mips 1..6 cascade through LDS (32x32, 16x16,
// 8x8, 4x4, 2x2, 1x1 = 1365 floats ~= 5.5 KB) behind group barriers;
// every level is ALSO stored at its global mip offset as it is made.
// Mip 6 is one texel per tile, so after the local cascade the pyramid
// tail (mips 7..mipCount-1) is a tiny image (<= 64x64 for sources
// <= 4096) — ONE workgroup can build it all.
//
// Cross-workgroup handoff (the part bloom's spd_downsample.cs.hlsl
// cannot do): each group publishes its mips 0..6 (plain stores), bumps a
// 32-bit completion counter (InterlockedAdd on a dedicated BDA counter
// buffer), and exits. The LAST group to bump (prev == groupCount-1)
// acquires everything published and builds mips 7+ from the mip-6
// surface in global memory. This is AMD's counter protocol, NOT a
// global spin barrier — non-resident groups never wait on anyone, so
// there is no deadlock when the dispatch exceeds resident capacity.
//
// Vulkan memory-model note (VERIFIED in the generated SPIR-V, DXC
// 1.9 / vulkan1.3 target): the atomic increment alone does NOT order
// the preceding non-atomic mip stores. The publication path is
//   AllMemoryBarrierWithGroupSync()  <- release, device scope
//   InterlockedAdd(counter)          <- the "done" signal
// and the acquire side is a second AllMemoryBarrierWithGroupSync() in
// the last group before its tail reads. A rendergraph buffer barrier
// CANNOT express this — it is all inside one dispatch.
// DXC emits (decoded): AllMemoryBarrierWithGroupSync ->
//   OpControlBarrier(Workgroup exec, Device memory, AcquireRelease
//   | UniformMemory | WorkgroupMemory | ImageMemory)
// and InterlockedAdd/Exchange -> OpAtomic*(scope=Device, relaxed).
// The module declares OpMemoryModel ... GLSL450 (the CLASSIC memory
// model — no OpCapability VulkanMemoryModel), under which a Device-
// scope AcquireRelease barrier whose semantics include UniformMemory
// fully orders AND makes available the storage-buffer/BDA accesses
// (the Vulkan environment maps UniformMemory to StorageBuffer +
// PhysicalStorageBuffer; CrossWorkgroupMemory is ignored and DXC
// correctly never emits it). This is the same structure glslang
// emits for the canonical memoryBarrierBuffer() pattern — every
// GPU prefix-scan/compaction shader relies on it. Under the classic
// model the barrier pair gives: group X's mip stores (program-ordered
// before its release barrier) happen-before the last group's tail
// reads (program-ordered after its acquire barrier), and the relaxed
// Device-scope counter atomics provide the per-location modification
// order that makes exactly one group observe prev == groupCount-1.
//
// Edge semantics (deliberately NOT stock SPD): AMD's SPD drops odd edge
// texels; Tourmaline's pyramid clamps them (min(2x+1, extent-1) — the
// orphan edge folds into the last texel, gfx_filter.h HzbMipDims
// repeated max((d+1)>>1, 1)). This shader reproduces the clamped
// footprint EXACTLY, mapped into tile-local coordinates: within a tile,
// a clamp can only engage at the GLOBAL mip edge (tile boundaries are
// 64-aligned and never clamp), and a clamped 2x2 target always stays
// inside the owning 4x4 block / parent tile region (ceil-division
// guarantees the base texel of an existing dst texel exists). So the
// per-tile cascade and the global per-mip reduction produce bit-
// identical values — that is the verify-mode contract
// (TOURMALINE_HZB_SPD_VERIFY).
//
// Reduction variant: build with HZB_REDUCE_MAX=1 for the SSR/AO MAX
// pyramid (nearest surface per block, reversed-Z), HZB_REDUCE_MAX=0 for
// the culling MIN pyramid. Same reduction + sanitize discipline as
// hzb_max.cs.hlsl / hzb.cs.hlsl (non-finite -> 0 = far/sky: keep-safe
// for MIN culling, skip-safe for MAX tracing).
//
// C++ mirror: Engine/Renderer/gfx_filter.h (HzbSpdPush, 104 bytes).

#ifndef HZB_REDUCE_MAX
#define HZB_REDUCE_MAX 1 // base output = the SSR/AO MAX twin
#endif

#define HZB_PASS
#include "common.hlsli"
#include "hzb.hlsli"

[[vk::push_constant]]
ConstantBuffer<HzbSpdPush> pcS;

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

// Reversed-Z reduction selector (see the variant comment up top).
float ReduceDepth(float a, float b, float c, float d)
{
#if HZB_REDUCE_MAX
    return max(max(a, b), max(c, d));
#else
    return min(min(a, b), min(c, d));
#endif
}

// Same contract as hzb.cs.hlsl / hzb_max.cs.hlsl: depth is in [0,1] by
// construction; NaN/Inf poison the chain (MIN over-culls, MAX keeps
// NaN) — collapse non-finite to 0 (far/sky under reversed-Z).
float SanitizeDepth(float z)
{
    return (isnan(z) || isinf(z)) ? 0.0f : z;
}

// Exact mirror of gfx_filter.h HzbMipDims: repeated max((d+1)>>1, 1).
uint2 SpdMipDims(uint2 wh, uint mip)
{
    [loop]
    for (uint k = 0; k < mip; ++k) {
        wh.x = max((wh.x + 1u) >> 1u, 1u);
        wh.y = max((wh.y + 1u) >> 1u, 1u);
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
                pcS.hzb.Get().texels[pcS.mipOffsets[L] + gy * dL.x + gx] = r; \
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
                pcS.hzb.Get().texels[(tileY0 + by + j) * pcS.width
                    + (tileX0 + bx + i)] = v;
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
                    pcS.hzb.Get().texels[pcS.mipOffsets[1] + my * d1.x + mx] = r;
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
                    const uint pOff = pcS.mipOffsets[L - 1u];
                    const float a = pcS.hzb.Get().texels[pOff + y0 * dP.x + x0];
                    const float b = pcS.hzb.Get().texels[pOff + y0 * dP.x + x1];
                    const float c = pcS.hzb.Get().texels[pOff + y1 * dP.x + x0];
                    const float d = pcS.hzb.Get().texels[pOff + y1 * dP.x + x1];
                    // Tail reads are raw global loads — sanitize the
                    // stored result like the per-mip shaders do (finite
                    // inputs make it a no-op; it bounds any latent bug to
                    // the keep-safe 0).
                    pcS.hzb.Get().texels[pcS.mipOffsets[L] + y * dL.x + x]
                        = SanitizeDepth(ReduceDepth(a, b, c, d));
                }
                // The next level reads this level's writes — same
                // workgroup, but global (not groupshared) memory.
                DeviceMemoryBarrierWithGroupSync();
            }
        }
    }
}
