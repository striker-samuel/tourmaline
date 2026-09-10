#pragma once
// Deferred Shadows — push constants + shared helpers.
//
// Implements the tiled/zBin deferred-shadow pipeline (Square Enix,
// "Deferred Shadows"; CPU builder in Engine/Rendergraph/deferredshadow.h):
//   1. Prepare (CPU): lights sorted by closest view distance; zBins +
//      per-TileGroup light masks uploaded.
//   2. Find Lights Approximate (GPU): per-Shadow-Tile froxel cull ->
//      TileGroup light masks + flexible light-list slot reservation.
//   3. Find Lights Accurate (GPU): per-pixel bounds test -> Early light
//      list (3 packed 10-bit indices per uint, figure 5).
//   4. Generate Deferred Shadows (GPU): per-pixel visibility (PCF spot
//      shadows), all-lit/all-shadow votes, VisibilityBuffer slots
//      (64B = 8x8 pixels x 8 bits) + Final light list (10-bit light
//      index | 22-bit VisibilitySlotID / fully-lit flag, figure 7b).
//   5. HiQuality composite (closeup character shadowmaps): extension
//      point, not implemented — character/light pairs would composite
//      high-quality shadows over the VisibilityBuffer results between
//      phase 4 and the lighting consumer.
//
// Buffer layout notes (must match the C++ builder exactly):
//   - zBins: uint4 per bin = (minIdx, maxIdx, rangedMin, rangedMax).
//     Indices refer to the SORTED light buffer (phase 1 sorts by closest
//     distance to the near plane). Conservative-safe: every light
//     overlapping a bin has sorted index in [minIdx..maxIdx]. Empty bins
//     encode min = lightCount (sentinel), max = 0 -> lo > hi -> "no
//     lights". rangedMin = prefix running min, rangedMax = suffix
//     running max (two-lookup depth-range queries, figure 2b).
//   - Tile light masks ("Light Mask Entry", figure 4): per TileGroup
//     (8x8 Shadow Tiles = 64x64 px):
//       masks[g * (1 + DS_MAX_LIGHT_GROUPS) + 0]       = group bitmask
//       masks[g * (1 + DS_MAX_LIGHT_GROUPS) + 1 + grp] = 32-lights mask
//   - Tile state: 4 uints per Shadow Tile = { listSlotID (uints into the
//     shared light list), light count, approxCount, earlyCount }. The
//     count holds the EARLY count after phase 3 and the FINAL count
//     after phase 4; approxCount (phase 2 froxel candidates) and
//     earlyCount (phase 3 per-pixel survivors) are debug-only telemetry
//     for the count visualizers (paper 5.4, DS_DEBUG_VIZ builds; the
//     4-uint layout itself is unconditional so debug/release shaders
//     share one ABI).
//   - Slot reservation is in uints of the shared light list and sized by
//     the approximated light count (NOT count/3): the early list packs 3
//     indices per uint, but the final list stores one 32-bit entry per
//     light, so the per-tile region must cover max(ceil(early/3), final)
//     = approxCount uints. Phase 4 stages the early indices in
//     groupshared memory between the read pass and the write pass, so
//     the two lists safely share one region.

#include "deferredshadow_types.hlsli"

// ---- Shared push constants for the three GPU phases (C++ mirror:
// smvk::DsPhasePush, 120 bytes) ----

struct DsPhasePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<ForwardPlusLightBuffer> lights; // sorted (phase 1)
    vk::BufferPointer<DsZBinBuffer> zbins;
    vk::BufferPointer<DsTileLightMaskBuffer> tileMasks;      // CPU (2 in)
    vk::BufferPointer<DsTileLightMaskBuffer> tileGroupMasks; // GPU (2/3)
    vk::BufferPointer<DsTileStateBuffer> tileState;
    vk::BufferPointer<DsLightListBuffer> lightList;
    vk::BufferPointer<DsVisibilityBuffer> visibility;
    vk::BufferPointer<DsCounters> counters;
    vk::BufferPointer<SpotShadowBuffer> spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint lightCount;
    uint tileCountX;
    uint tileCountY;
    uint groupCountX;
    uint groupCountY;
    uint depthTextureIndex;
    float spotShadowNear;  // SPOT_SHADOW_NEAR (PCSS eye-depth inversion)
    float pcssLightSize;   // world-space emitting-sphere diameter (PCSS)
};

// ---- Addressing helpers (CPU builder writes the same layout) ----

uint DsGroupMaskAddress(uint groupIdx)
{
    return groupIdx * (1 + DS_MAX_LIGHT_GROUPS);
}

uint DsGroupEntryAddress(uint groupIdx, uint lightGroup)
{
    return groupIdx * (1 + DS_MAX_LIGHT_GROUPS) + 1 + lightGroup;
}

// Restrict a 32-light group mask to the sorted-index window [lo..hi]
// (zBin min/max light indices).
uint DsWindowMask(uint lightGroup, uint lo, uint hi)
{
    const uint a = max(lo, lightGroup * 32u);
    const uint b = min(hi, lightGroup * 32u + 31u);
    if (b < a)
        return 0u;
    const uint n = b - a + 1u;
    const uint ones = (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    return ones << (a - lightGroup * 32u);
}

// zBin lookup for a depth range [bin0..bin1] (both clamped). 1-2
// contiguous bins use the standard zBin (tighter); wider ranges use
// zBin(ranged) (prefix-min at the far bin, suffix-max at the near bin).
// Empty windows return lo > hi.
void DsZBinRange(vk::BufferPointer<DsZBinBuffer> zbins, uint bin0, uint bin1,
    uint lightCount, out uint lo, out uint hi)
{
    bin0 = min(bin0, DS_ZBIN_COUNT - 1u);
    bin1 = min(bin1, DS_ZBIN_COUNT - 1u);
    if (bin0 > bin1) { const uint t = bin0; bin0 = bin1; bin1 = t; }
    const uint4 a = zbins.Get().bins[bin0];
    const uint4 b = zbins.Get().bins[bin1];
    if (bin1 - bin0 <= 1u) {
        // Standard zBin: min/max light indices of the 1-2 bins.
        lo = min(a.x, b.x);
        hi = max(a.y, b.y);
    } else {
        // zBin (ranged): conservative over the whole span.
        lo = b.z; // rangedMin[far bin]   = min over bins [0..bin1]
        hi = a.w; // rangedMax[near bin]  = max over bins [bin0..last]
    }
    if (lo > hi || hi >= lightCount) {
        lo = 1u;
        hi = 0u;
    }
}

// View distance bin index (linear over [near, far]).
uint DsBinFromDist(float dist, float nearP, float farP)
{
    const float span = max(farP - nearP, 1e-4f);
    const float t = saturate((dist - nearP) / span);
    return (uint)(t * (float)(DS_ZBIN_COUNT - 1u));
}

// ---- Depth reconstruction (compute phases; matches the frag consumer
// that uses pixel-center clip coordinates) ----

float3 DsWorldPosFromDepth(float2 pix, float depth, FrameUniforms frame)
{
    const float2 ndc = float2(
        (pix.x + 0.5f) * frame.invViewportSize.x * 2.0f - 1.0f,
        (pix.y + 0.5f) * frame.invViewportSize.y * 2.0f - 1.0f);
    const float4 w = mul(frame.invViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / max(w.w, 1e-9f);
}

// Unproject NDC -> view space (exact, reversed-Z agnostic; near-plane
// points double as origin ray directions for frustum planes). Same math
// as forwardplus.cs.hlsl.
float3 DsUnprojectView(float2 ndcXY, float4x4 invProj)
{
    const float4 v = mul(invProj, float4(ndcXY, 1.0f, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

// ---- Per-pixel light bounds test (phase 3; no normals, distance only) ----

bool DsLightPixelTests(ForwardPlusLight L, float3 worldPos)
{
    const float range = (L.range <= 0.0f) ? (float)DS_INFINITE_RANGE : L.range;
    const float3 toLight = L.position - worldPos;
    const float dist2 = dot(toLight, toLight);
    if (dist2 > range * range)
        return false;
    if (L.type != 1)
        return true;
    // Spot cone (same convention as default_lit: angle at the light
    // between the spot axis and the direction toward the pixel).
    const float dist = sqrt(dist2);
    const float3 dirToPixel = -toLight / max(dist, 1e-6f);
    const float3 axis = normalize(L.direction);
    const float cosA = dot(dirToPixel, axis);
    return cosA >= cos(L.outerAngle) - 1e-4f;
}

// ---- Early list packing (figure 5: 3 light indices x 10 bits per uint) ----

uint DsPackLightIndex(uint packed, uint lightIdx, uint count)
{
    return packed | (lightIdx << (10u * (count % 3u)));
}

// ---- Consumer helpers (default_lit.frag.hlsl) ----

// One visibility byte: slot k occupies bytes [k*64 .. k*64+63]; the pixel
// index inside the tile (0..63) matches phase 4's SV_GroupIndex mapping
// (x + y*8). Out-of-range slots (stale/garbage) read as fully lit.
float DsLoadVisibility(vk::BufferPointer<DsVisibilityBuffer> vis, uint slot, uint localIdx)
{
    if (slot >= DS_MAX_VIS_SLOTS)
        return 1.0f;
    const uint word = vis.Get().words[slot * DS_VIS_WORDS_PER_SLOT + (localIdx >> 2u)];
    const uint b = (word >> (8u * (localIdx & 3u))) & 0xFFu;
    return float(b) * (1.0f / 255.0f);
}

// ---- Debug visualizers (paper 5.3 Light Culling / 5.4 Light Count).
// Debug-build only (DS_DEBUG_VIZ, set by CompileShaders.cmake): the
// per-pixel diagnostic paths compile out of release shaders entirely.
// Rendered in default_lit.frag.hlsl, blended over the lit image (flag
// bit0) or replacing it. App-side mirror: app::DsDebugViz. ----
#if defined(DS_DEBUG_VIZ)

#define DS_VIZ_OFF 0
#define DS_VIZ_COUNT_APPROX 1   // phase-2 approximated light count per tile
#define DS_VIZ_COUNT_ACCURATE 2 // phase-3 early (per-pixel tested) count
#define DS_VIZ_COUNT_FINAL 3   // phase-4 final list count
#define DS_VIZ_COUNT_DIFF 4    // approx - accurate: where phase 2 is weakest
#define DS_VIZ_CULL_STEP 5     // last culling step passed by selected light
#define DS_VIZ_LIGHT_LIST 6    // which light list holds the selected light
#define DS_VIZ_LIGHT_VIS 7     // selected light per-pixel visibility byte

// Heat ramp for per-tile counts (figure 26): 0 = near-black navy, then
// blue -> cyan -> green -> yellow -> red at the ceiling.
float3 DsCountHeat(uint count, uint ceiling)
{
    const float t = saturate((float)count / (float)max(ceiling, 1u));
    const float3 stops[6] = {
        float3(0.03f, 0.03f, 0.10f),
        float3(0.02f, 0.30f, 0.85f),
        float3(0.05f, 0.80f, 0.85f),
        float3(0.20f, 0.90f, 0.20f),
        float3(0.98f, 0.90f, 0.10f),
        float3(0.95f, 0.12f, 0.08f)
    };
    const float s = min(t * 5.0f, 4.999f);
    const uint i = (uint)s;
    return lerp(stops[i], stops[i + 1u], s - float(i));
}

// Final-list lookup for one light (figure 7b entries). Returns
// 0xFFFFFFFF when absent, DS_SLOT_ALL_LIT for a fully-lit entry, else
// the VisibilityBuffer slot ID.
uint DsFinalListFind(vk::BufferPointer<DsTileStateBuffer> tileState,
    vk::BufferPointer<DsLightListBuffer> lightList,
    uint tileIdx, uint lightIdx)
{
    const uint slot = tileState.Get().states[tileIdx * 4u];
    if (slot >= (uint)DS_MAX_LIST_UINTS)
        return 0xFFFFFFFFu;
    const uint count = min(tileState.Get().states[tileIdx * 4u + 1u],
        (uint)DS_MAX_LIGHTS_PER_TILE);
    for (uint i = 0u; i < count; ++i) {
        const uint entry = lightList.Get().entries[slot + i];
        if ((entry & 0x3FFu) == lightIdx)
            return entry >> DS_LIGHT_IDX_BITS;
    }
    return 0xFFFFFFFFu;
}

// Tile-group light-mask membership (phase-1 CPU / phase-2 GPU buffers share
// the "Light Mask Entry" layout, figure 4).
bool DsMaskHasLight(vk::BufferPointer<DsTileLightMaskBuffer> masks,
    uint groupIdx, uint lightIdx)
{
    if (lightIdx >= DS_MAX_LIGHT_GROUPS * 32u)
        return false;
    return (masks.Get().masks[DsGroupEntryAddress(groupIdx, lightIdx >> 5u)]
        >> (lightIdx & 31u)) & 1u;
}

// Per-pixel visualizer color. mode = DS_VIZ_*; lightIdx = SORTED light
// index for the light-scoped modes (phase 1 sorts by near distance, so
// the ImGui index space and the buffer order match only after the sort).
// worldPos: the shaded fragment's world position (ground truth bounds
// tests re-run per pixel, exactly like phase 3).
float3 DsVisualize(FrameUniforms frame,
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights, uint lightCount,
    float3 worldPos, uint2 pix, uint mode, uint lightIdx)
{
    if (mode == DS_VIZ_OFF || frame.dsTileCountX == 0u)
        return float3(0.0f, 0.0f, 0.0f);
    if (any(pix >= (uint2)frame.viewportSize))
        return float3(0.0f, 0.0f, 0.0f);
    const uint tileX = pix.x >> 3; // DS_SHADOW_TILE_PX
    const uint tileY = pix.y >> 3;
    const uint tileIdx = tileY * frame.dsTileCountX + tileX;

    // ---- Light count per Shadow Tile (5.4) ----
    if (mode <= DS_VIZ_COUNT_DIFF) {
        const uint approx = frame.dsTileState.Get().states[tileIdx * 4u + 2u];
        const uint early = frame.dsTileState.Get().states[tileIdx * 4u + 3u];
        float3 heat;
        if (mode == DS_VIZ_COUNT_APPROX) {
            heat = DsCountHeat(approx, frame.dsDebugCountCeil);
        } else if (mode == DS_VIZ_COUNT_ACCURATE) {
            heat = DsCountHeat(early, frame.dsDebugCountCeil);
        } else if (mode == DS_VIZ_COUNT_FINAL) {
            heat = DsCountHeat(
                min(frame.dsTileState.Get().states[tileIdx * 4u + 1u],
                    (uint)DS_MAX_LIGHTS_PER_TILE), frame.dsDebugCountCeil);
        } else {
            // Difference: approx - accurate. Any NEGATIVE value is a hard
            // error (the accurate list is a subset of the approximated one
            // by construction) -> flagged red, out of the heat ramp.
            const int diff = int(approx) - int(early);
            heat = (diff < 0)
                ? float3(1.0f, 0.10f, 0.05f)
                : DsCountHeat(uint(diff), frame.dsDebugCountCeil);
        }
        // Faint 64px TileGroup grid so tile/group granularity reads.
        if ((pix.x & 63u) == 0u || (pix.y & 63u) == 0u)
            heat = max(heat, float3(0.10f, 0.10f, 0.12f));
        return heat;
    }

    // ---- Per-light diagnostics (5.3): selected light vs the pipeline ----
    if (lightIdx >= lightCount || lightIdx >= DS_MAX_LIGHT_GROUPS * 32u)
        return float3(0.30f, 0.05f, 0.30f); // invalid selection: flat magenta

    const ForwardPlusLight L = fpLights.Get().lights[lightIdx];
    // Ground truth (phase 3's exact test): does this light reach THIS pixel?
    const bool truth = DsLightPixelTests(L, worldPos);

    // Pipeline membership: phase-1 CPU mask -> phase-2 GPU mask ->
    // phase-4 final list (early list = phase-2 mask AND truth: phase 3
    // runs this same per-pixel test, so a passing pixel implies the
    // light entered the tile's early list).
    const uint groupCountX = (frame.dsTileCountX + 7u) / 8u; // ceil(ceil(w/8)/8)
    const uint groupIdx = (tileY >> 3) * groupCountX + (tileX >> 3);
    const bool inCpuMask = DsMaskHasLight(frame.dsTileMasks, groupIdx, lightIdx);
    const bool inGroupMask = DsMaskHasLight(frame.dsTileGroupMasks, groupIdx, lightIdx);
    const uint slotInfo = DsFinalListFind(frame.dsTileState, frame.dsLightList,
        tileIdx, lightIdx);
    const bool inFinal = slotInfo != 0xFFFFFFFFu;
    float vis = 1.0f;
    if (inFinal && slotInfo != DS_SLOT_ALL_LIT)
        vis = DsLoadVisibility(frame.dsVisibility, slotInfo,
            (pix.x & 7u) + (pix.y & 7u) * 8u);

    if (mode == DS_VIZ_CULL_STEP) {
        // Figure 25a: last culling step the light successfully passed for
        // this pixel. RED/ORANGE on an in-range pixel = erroneous cull
        // (conservative tests must never reject a pixel inside the
        // bounds); BLUE = dropped by phase 4's all-shadow tile vote.
        if (!truth)
            return inFinal ? float3(0.10f, 0.35f, 0.45f) : float3(0.02f, 0.02f, 0.02f);
        if (!inCpuMask)
            return float3(1.0f, 0.10f, 0.10f);   // phase 1 (CPU mask)
        if (!inGroupMask)
            return float3(1.0f, 0.50f, 0.00f);   // phase 2 (zBin/froxel)
        if (!inFinal)
            return float3(0.15f, 0.30f, 1.0f);   // phase 4 (all-shadow drop)
        return float3(0.10f, 1.0f, 0.15f) * (0.15f + 0.85f * vis);
    }
    if (mode == DS_VIZ_LIGHT_LIST) {
        // Figure 25b: which light list holds the light for this pixel.
        if (truth && !inCpuMask)
            return float3(1.0f, 0.10f, 0.10f);   // in NO list (erroneous)
        if (truth && !inGroupMask)
            return float3(1.0f, 0.50f, 0.00f);   // phase-1 mask only (erroneous)
        if (!inFinal)
            return truth ? float3(0.95f, 0.85f, 0.10f) // early only (shadow-dropped)
                : float3(0.03f, 0.03f, 0.03f);
        return float3(0.10f, 1.0f, 0.15f)
            * (0.15f + 0.85f * vis) * (truth ? 1.0f : 0.35f);
    }
    // DS_VIZ_LIGHT_VIS: the precomputed visibility byte itself (white =
    // lit, black = shadowed); dark blue = light not in the final list.
    if (!inFinal)
        return float3(0.05f, 0.07f, 0.12f);
    return vis * float3(0.95f, 0.95f, 0.95f) + float3(0.05f, 0.05f, 0.05f);
}

#endif // DS_DEBUG_VIZ
