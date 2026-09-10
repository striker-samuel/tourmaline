// Deferred Shadows — Phase 3: Find Lights Accurate (paper listing 3).
//
// One work unit = ONE Shadow Tile (8x8 px); each thread evaluates one
// pixel. Narrows the phase-2 approximation: every light flagged in the
// TileGroup mask is re-tested against the pixel's reconstructed world
// position (pure distance/cone bounds, no normals).
//
// Cross-lane "any pixel of the tile wants this light" (WaveActiveAnyTrue
// in the paper): per light, each wave votes and its first lane ORs the
// bit into groupshared — one barrier for the whole tile, wave32-safe.
// Thread 0 then replays the (uniform) iteration, reads the vote bits and
// emits the Early light list packed 3x10-bit indices per uint (figure 5)
// into the tile's flexible slot, plus the per-tile count.
//
// A light valid for some pixel of the tile is always valid for the tile's
// froxel in phase 2 (per-pixel bounds are contained in the froxel bound
// tests), so the early count never exceeds the reserved approx count.

#define DS_PASS
#include "common.hlsli"

[[vk::push_constant]]
ConstantBuffer<DsPhasePush> pcDS;

// Vote bits: ordinal k = the k-th candidate light walked by the (uniform)
// iteration below; bit set when ANY pixel of the tile passes its bounds.
groupshared uint gsBits[32]; // 1024 ordinals

[numthreads(DS_SHADOW_TILE_PX, DS_SHADOW_TILE_PX, 1)]
void mainCS(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint laneIdx = tid.y * DS_SHADOW_TILE_PX + tid.x; // pixel 0..63

    if (laneIdx < 32u)
        gsBits[laneIdx] = 0u;
    GroupMemoryBarrierWithGroupSync();

    const uint2 tileCoord = gid.xy;
    const bool validTile = all(tileCoord < uint2(pcDS.tileCountX, pcDS.tileCountY));

    // ---- Per-pixel inputs ----
    const FrameUniforms frame = pcDS.frameUniforms.Get();
    const uint2 vp = (uint2)frame.viewportSize;
    const uint2 px = tileCoord * DS_SHADOW_TILE_PX + tid.xy;
    const uint depthIdx = NonUniformResourceIndex(pcDS.depthTextureIndex);
    bool validPixel = validTile && all(px < vp);
    float depth = 0.0f;
    float3 worldPos = float3(0.0f, 0.0f, 0.0f);
    if (validPixel) {
        depth = textures[depthIdx].Load(int3(int2(px), 0)).r;
        if (depth <= (float)DS_SKY_DEPTH_EPS)
            validPixel = false; // sky / far (reversed-Z clear 0)
        else
            worldPos = DsWorldPosFromDepth(float2(px), depth, frame);
    }

    // ---- Walk candidates from the phase-2 TileGroup mask ----
    uint groupIdx = 0xFFFFFFFFu; // sentinel: invalid tile walks nothing
    if (validTile) {
        groupIdx = (tileCoord.y / DS_TILE_GROUP_TILES) * pcDS.groupCountX
            + (tileCoord.x / DS_TILE_GROUP_TILES);
    }
    uint lightGroupMask = (groupIdx != 0xFFFFFFFFu)
        ? pcDS.tileGroupMasks.Get().masks[DsGroupMaskAddress(groupIdx)]
        : 0u;

    uint ordinal = 0u;
    uint g = lightGroupMask;
    while (g != 0u) {
        const uint grp = firstbitlow(g);
        g ^= 1u << grp;
        uint lm = pcDS.tileGroupMasks.Get().masks[DsGroupEntryAddress(groupIdx, grp)];
        while (lm != 0u) {
            const uint bit = firstbitlow(lm);
            lm ^= 1u << bit;
            const uint lightIdx = grp * 32u + bit;
            bool bValid = false;
            if (validPixel && lightIdx < pcDS.lightCount) {
                const ForwardPlusLight L = pcDS.lights.Get().lights[lightIdx];
                bValid = DsLightPixelTests(L, worldPos);
            }
            const bool anyW = WaveActiveAnyTrue(bValid);
            if (WaveIsFirstLane() && anyW && ordinal < 1024u)
                InterlockedOr(gsBits[ordinal >> 5u], 1u << (ordinal & 31u));
            ordinal += 1u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Thread 0: replay the walk, read the votes, emit the early list ----
    if (laneIdx == 0) {
        const uint tileIdx = tileCoord.y * pcDS.tileCountX + tileCoord.x;
        const uint slot = pcDS.tileState.Get().states[tileIdx * 2u];
        uint count = 0u;
        uint packed = 0u;
        uint ordinal2 = 0u;
        uint g2 = lightGroupMask;
        while (g2 != 0u) {
            const uint grp = firstbitlow(g2);
            g2 ^= 1u << grp;
            uint lm = pcDS.tileGroupMasks.Get().masks[DsGroupEntryAddress(groupIdx, grp)];
            while (lm != 0u) {
                const uint bit = firstbitlow(lm);
                lm ^= 1u << bit;
                const uint lightIdx = grp * 32u + bit;
                const bool any = ordinal2 < 1024u
                    && ((gsBits[ordinal2 >> 5u] >> (ordinal2 & 31u)) & 1u) != 0u;
                if (any && count < DS_MAX_LIGHTS_PER_TILE
                    && slot + count / 3u < DS_MAX_LIST_UINTS) {
                    packed = DsPackLightIndex(packed, lightIdx, count);
                    count += 1u;
                    if ((count % 3u) == 0u) {
                        pcDS.lightList.Get().entries[slot + count / 3u - 1u] = packed;
                        packed = 0u;
                    }
                }
                ordinal2 += 1u;
            }
        }
        if (count > 0u && (count % 3u) != 0u
            && slot + count / 3u < DS_MAX_LIST_UINTS)
            pcDS.lightList.Get().entries[slot + count / 3u] = packed;
        pcDS.tileState.Get().states[tileIdx * 2u + 1u] = count;
        // Telemetry: total early entries (races across tiles are fine).
        InterlockedAdd(pcDS.counters.Get().sumEarly, count);
    }
}
