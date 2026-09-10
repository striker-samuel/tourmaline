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
//   - Tile state: 2 uints per Shadow Tile = { listSlotID (uints into the
//     shared light list), light count }. The count holds the EARLY count
//     after phase 3 and the FINAL count after phase 4.
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
