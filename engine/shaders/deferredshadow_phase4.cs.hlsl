// Deferred Shadows — Phase 4: Generate Deferred Shadows (paper listing 4).
//
// One work unit = ONE Shadow Tile (8x8 px); each thread evaluates one
// pixel. Iterates the Early light list (phase 3) and computes the per
// pixel visibility of each light with the standard spot-shadow path
// (PCF atlas, same sampling as the inline forward+ shader — the paper
// uses PCSS, any technique substitutes). Results are classified per
// light over the WHOLE tile:
//   - entirely in shadow  -> dropped from the final list,
//   - entirely lit        -> final entry with the fully-lit flag,
//   - partial (penumbra)  -> one 64B VisibilityBuffer slot (8 bits per
//     pixel) allocated from the shared counter (listing 1
//     AllocateVisibilitySlotID) + final entry (10-bit light index |
//     22-bit slot ID, figure 7b).
//
// Wave notes: 'WaveActiveAllTrue' votes only span one wave; an 8x8 group
// is a single wave on wave64 HW but TWO waves on wave32. Each wave's
// first lane ANDs its vote into a groupshared accumulator (initialized
// all-ones), so the tile-wide classification is correct on both. The
// same reason puts the early-index staging in groupshared: the final
// list is written in-place over the early list's region (both fit the
// approx-count reservation), so all reads complete (barrier) before any
// write.
//
// The consumer (default_lit.frag.hlsl) reads the final list + visibility
// bytes instead of sampling shadows inline.

#define DS_PASS
#include "common.hlsli"
#include "shadows.hlsli" // GetSpotShadowFactorTile (pc-free)

[[vk::push_constant]]
ConstantBuffer<DsPhasePush> pcDS;

groupshared uint gsLitAcc[2];   // AND of per-wave all-lit votes (double-buffered
groupshared uint gsShadowAcc[2]; // by light parity: lane 0 resets slot (e+1)&1
                                 // while all lanes read slot e&1 — no race)
groupshared uint gsSlotV;       // visibility slot for the current light
groupshared uint gsVizBytes[64]; // per-pixel visibility, byte-quantized
groupshared uint gsEarlyIdx[DS_MAX_LIGHTS_PER_TILE];

// Per-light visibility: spots with an assigned shadow tile run PCSS
// over the spot-shadow atlas (paper 2.4 — Percentage Closer Soft Shadows;
// same guards as the inline path); everything else is fully lit.
float DsComputeLightVisibility(ForwardPlusLight L, float3 worldPos, float3 faceNormal)
{
    if (L.type != 1)
        return 1.0f;
    if (L.shadowTile < 0)
        return 1.0f;
    if (pcDS.spotShadowCount == 0u || pcDS.spotShadowMapIndex == 0xFFFFFFFFu)
        return 1.0f;
    if ((uint)L.shadowTile >= pcDS.spotShadowCount ||
        pcDS.spotShadowCount > (uint)MAX_SPOT_SHADOWS)
        return 1.0f;
    const uint atlasIdx = NonUniformResourceIndex(pcDS.spotShadowMapIndex);
    const GpuCascadeDescription t = pcDS.spotShadowBuffer.Get().tiles[L.shadowTile];
    return PcssSpotShadow(1u, worldPos, faceNormal, L.direction, t, atlasIdx,
        pcDS.pcssLightSize, pcDS.spotShadowNear, L.outerAngle);
}

// Geometric face normal from depth neighbors (same idea as the frag
// path's ddx/ddy geometric normal; used for the receiver-side shadow
// bias). Degenerate (silhouette/sky-adjacent) pixels fall back to the
// view-facing normal.
float3 DsFaceNormalFromDepth(float2 pix, float d0, FrameUniforms frame,
    uint depthIdx, uint2 vp)
{
    const float2 pa = float2(min(pix.x + 1.0f, float(vp.x - 1u)), pix.y);
    const float2 pb = float2(pix.x, min(pix.y + 1.0f, float(vp.y - 1u)));
    float da = textures[depthIdx].Load(int3(int2(pa), 0)).r;
    float db = textures[depthIdx].Load(int3(int2(pb), 0)).r;
    if (da <= (float)DS_SKY_DEPTH_EPS)
        da = d0;
    if (db <= (float)DS_SKY_DEPTH_EPS)
        db = d0;
    const float3 p0 = DsWorldPosFromDepth(pix, d0, frame);
    const float3 pA = DsWorldPosFromDepth(pa, da, frame);
    const float3 pB = DsWorldPosFromDepth(pb, db, frame);
    const float3 n = cross(pA - p0, pB - p0);
    const float len = length(n);
    if (len < 1e-9f)
        return normalize(frame.cameraPos - p0);
    const float3 nn = n / len;
    return (dot(nn, frame.cameraPos - p0) < 0.0f) ? -nn : nn;
}

[numthreads(DS_SHADOW_TILE_PX, DS_SHADOW_TILE_PX, 1)]
void mainCS(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint laneIdx = tid.y * DS_SHADOW_TILE_PX + tid.x; // pixel 0..63

    if (laneIdx == 0u) {
        gsLitAcc[0] = 0xFFFFFFFFu;
        gsLitAcc[1] = 0xFFFFFFFFu;
        gsShadowAcc[0] = 0xFFFFFFFFu;
        gsShadowAcc[1] = 0xFFFFFFFFu;
        gsSlotV = 0u;
    }
    GroupMemoryBarrierWithGroupSync(); // #0 init

    const uint2 tileCoord = gid.xy;
    const bool validTile = all(tileCoord < uint2(pcDS.tileCountX, pcDS.tileCountY));
    const uint tileIdx = tileCoord.y * pcDS.tileCountX + tileCoord.x;

    const uint slot = validTile ? pcDS.tileState.Get().states[tileIdx * 4u] : 0u;
    const uint earlyCount = validTile
        ? min(pcDS.tileState.Get().states[tileIdx * 4u + 1u], (uint)DS_MAX_LIGHTS_PER_TILE)
        : 0u;

    // ---- Per-pixel inputs ----
    const FrameUniforms frame = pcDS.frameUniforms.Get();
    const uint2 vp = (uint2)frame.viewportSize;
    const uint2 px = tileCoord * DS_SHADOW_TILE_PX + tid.xy;
    const uint depthIdx = NonUniformResourceIndex(pcDS.depthTextureIndex);
    bool validPixel = validTile && all(px < vp);
    float3 worldPos = float3(0.0f, 0.0f, 0.0f);
    float3 faceNormal = float3(0.0f, 1.0f, 0.0f);
    if (validPixel) {
        const float d = textures[depthIdx].Load(int3(int2(px), 0)).r;
        if (d <= (float)DS_SKY_DEPTH_EPS) {
            validPixel = false; // sky / far
        } else {
            worldPos = DsWorldPosFromDepth(float2(px), d, frame);
            faceNormal = DsFaceNormalFromDepth(float2(px), d, frame, depthIdx, vp);
        }
    }

    // ---- Loop A (thread 0): stage the early indices in groupshared so
    // loop B can overwrite the same list region in place ----
    if (laneIdx == 0u) {
        for (uint e = 0; e < earlyCount; ++e) {
            uint idx = 0xFFFFFFFFu;
            if (slot + e / 3u < DS_MAX_LIST_UINTS) {
                const uint packed = pcDS.lightList.Get().entries[slot + e / 3u];
                idx = (packed >> (10u * (e % 3u))) & 0x3FFu;
            }
            gsEarlyIdx[e] = idx;
        }
    }
    GroupMemoryBarrierWithGroupSync(); // between A and B

    // ---- Loop B: classify + emit ----
    uint finalCount = 0u; // uniform across lanes (decisions are tile-wide)
    for (uint e = 0; e < earlyCount; ++e) {
        const uint lightIdx = gsEarlyIdx[e];
        if (lightIdx == 0xFFFFFFFFu || lightIdx >= pcDS.lightCount)
            continue;

        const ForwardPlusLight L = pcDS.lights.Get().lights[lightIdx];
        float viz = 1.0f;
        if (validPixel)
            viz = DsComputeLightVisibility(L, worldPos, faceNormal);
        // Invalid (sky/outside) pixels count as lit: they never shade.

        // Tile-wide votes (per wave, ANDed through groupshared). Light e
        // uses slot e&1; slot (e+1)&1 is reset below for the next light.
        const uint parity = e & 1u;
        const bool litW = WaveActiveAllTrue(viz > 0.9999f);
        const bool shW = WaveActiveAllTrue(viz < 0.0001f);
        if (WaveIsFirstLane()) {
            InterlockedAnd(gsLitAcc[parity], litW ? 1u : 0u);
            InterlockedAnd(gsShadowAcc[parity], shW ? 1u : 0u);
        }
        GroupMemoryBarrierWithGroupSync(); // votes visible
        const bool allLit = gsLitAcc[parity] != 0u;
        const bool allShadow = gsShadowAcc[parity] != 0u;
        // Reset the OTHER accumulator pair for the next light (ordered
        // against its InterlockedAnd by the trailing barrier below).
        if (laneIdx == 0u) {
            gsLitAcc[parity ^ 1u] = 0xFFFFFFFFu;
            gsShadowAcc[parity ^ 1u] = 0xFFFFFFFFu;
        }

        if (!allShadow) {
            if (laneIdx == 0u) {
                // Telemetry histogram (atomics; races across tiles fine).
                if (allLit)
                    InterlockedAdd(pcDS.counters.Get().clsAllLit, 1u);
                else
                    InterlockedAdd(pcDS.counters.Get().clsPenumbra, 1u);
            }
            uint slotV = DS_SLOT_ALL_LIT;
            if (!allLit) {
                // Partial shadow: allocate one 64B slot for the tile
                // (single atomic; overflow degrades to fully lit so no
                // store goes out of range) and quantize this pixel's
                // visibility into the byte staging.
                if (laneIdx == 0u) {
                    uint s = 0u;
                    InterlockedAdd(pcDS.counters.Get().visSlots, 1u, s);
                    gsSlotV = (s < DS_MAX_VIS_SLOTS) ? s : DS_SLOT_ALL_LIT;
                }
                gsVizBytes[laneIdx] = uint(saturate(viz) * 255.0f + 0.5f);
            }
            GroupMemoryBarrierWithGroupSync(); // slot + bytes visible
            if (!allLit) {
                slotV = gsSlotV;
                if (slotV != DS_SLOT_ALL_LIT && (laneIdx & 3u) == 0u) {
                    // Pack 4 pixel bytes per uint (16 uints = 64B slot).
                    const uint w = gsVizBytes[laneIdx]
                        | (gsVizBytes[laneIdx + 1u] << 8u)
                        | (gsVizBytes[laneIdx + 2u] << 16u)
                        | (gsVizBytes[laneIdx + 3u] << 24u);
                    pcDS.visibility.Get().words[slotV * DS_VIS_WORDS_PER_SLOT + (laneIdx >> 2u)] = w;
                }
            }
            // Final entry: 10-bit light index | 22-bit slot/flag. Written
            // in place over the early list region (writes <= reads done
            // above; the inter-loop barrier keeps it safe).
            if (laneIdx == 0u && slot + finalCount < DS_MAX_LIST_UINTS)
                pcDS.lightList.Get().entries[slot + finalCount] =
                lightIdx | (slotV << 10u);
            finalCount += 1u;
        } else {
            GroupMemoryBarrierWithGroupSync(); // match the penumbra path
            if (laneIdx == 0u)
                InterlockedAdd(pcDS.counters.Get().clsAllShadow, 1u);
        }
    }

    if (laneIdx == 0u && validTile) {
        pcDS.tileState.Get().states[tileIdx * 4u + 1u] = finalCount;
        // Telemetry: total final entries.
        InterlockedAdd(pcDS.counters.Get().sumFinal, finalCount);
    }
}
