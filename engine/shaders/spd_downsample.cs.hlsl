// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// SPD (Single Pass Downsampler) for the bloom chain.
//
// Replaces the five fullscreen-triangle 13-tap Karis downsample passes
// (bloom_down.frag.hlsl) with ONE compute dispatch that cascades all five
// bloom mips through groupshared memory — the FidelityFX SPD idea adapted
// to this engine's separated-images bloom chain (MipBuilder::BuildSeparated
// gives each mip its own physical image, so the cascade writes five
// storage images instead of five slices of one image).
//
// Tile geometry: 16x16 threads cover a 16x16 tile of bloom mip 0
// (== 32x32 HDR texels). Cascade: 16x16 -> 8x8 -> 4x4 -> 2x2 -> 1x1,
// each level a 2x2 Karis average of its parent held in groupshared
// (256 + 64 + 16 + 4 = 340 float4 = ~5.4 KB LDS). No intermediate global
// memory: parent levels are consumed straight out of LDS behind barriers.
//
// Filtering fidelity vs the raster path it replaces:
//  - Mip 0: 2x2 HDR box (not 13-tap) + the SAME soft-knee Prefilter with
//    the SAME threshold/knee push values — threshold behavior identical,
//    footprint tighter (less light leaking from far taps).
//  - Mips 1..4: 2x2 Karis average per level (firefly suppression is kept
//    through the chain, like the old per-level 13-tap Karis).
//  - NaN/inf defense: every HDR tap is SanitizeHdr'd (see common.hlsli),
//    same poison contract as bloom_down.frag.hlsl.
//  - Odd extents: parent fetches clamp (min(2x+1, parentExtent-1)) so the
//    orphan edge texel folds into the last pixel instead of dropping energy.
//
// Out-of-tile threads participate in the LDS fill + barriers, then skip
// their stores (same discipline as evsm_blur.hlsli).
//
// C++ mirror: SpdPush in Engine/Rendergraph/bloom.h (48 bytes).

#define SPD_PASS
#include "common.hlsli"

// 16x16 threads; cascade depths for a 5-level bloom chain.
#define SPD_BLOCK 16u
#define SPD_LEVELS 5

// LDS regions (flat float4 array, row-major per level).
#define SPD_GS0 0
#define SPD_GS1 (16 * 16)
#define SPD_GS2 (16 * 16 + 8 * 8)
#define SPD_GS3 (16 * 16 + 8 * 8 + 4 * 4)
#define SPD_GS_SIZE (16 * 16 + 8 * 8 + 4 * 4 + 2 * 2)

// Must match SpdPush in Engine/Rendergraph/bloom.h (48 bytes, DX layout).
struct SpdPush
{
    uint srcIndex;      // bindless textures[] index of the HDR source
    uint dstIndex0;     // bindless storage-image indices of the 5 bloom mips
    uint dstIndex1;
    uint dstIndex2;
    uint dstIndex3;
    uint dstIndex4;
    uint baseWidth;     // bloom mip 0 extent (dispatch tiling + bounds)
    uint baseHeight;
    uint hdrWidth;      // HDR source extent (tap clamp)
    uint hdrHeight;
    float threshold;    // soft-knee prefilter (mip 0 only, like the raster path)
    float knee;
};

[[vk::push_constant]]
ConstantBuffer<SpdPush> spd;

// Bindless storage-image array (set 0, binding 2) — the same descriptor
// array evsmStorage[] addresses (one unified bindless index space, so the
// bloom mip bindless indices work here directly). float4 matches the
// RGBA16F bloom images (format/typed-format pairing must agree, see
// evsm_blur.hlsli).
[[vk::binding(2, 0)]]
RWTexture2D<float4> spdImages[];

groupshared float4 gsTile[SPD_GS_SIZE];

float SpdLuma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Soft-knee prefilter, verbatim math from bloom_down.frag.hlsl Prefilter.
float3 SpdPrefilter(float3 c, float threshold, float knee)
{
    float luma = SpdLuma(c);
    float soft = clamp(luma - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * max(knee, 1e-4f));
    float contribution = max(soft, luma - threshold) / max(luma, 1e-4f);
    return c * contribution;
}

// 2x2 Karis average (firefly suppression): weight 1/(1+luma), renormalized.
float3 SpdKaris4(float3 s0, float3 s1, float3 s2, float3 s3)
{
    float w0 = 1.0f / (1.0f + SpdLuma(s0));
    float w1 = 1.0f / (1.0f + SpdLuma(s1));
    float w2 = 1.0f / (1.0f + SpdLuma(s2));
    float w3 = 1.0f / (1.0f + SpdLuma(s3));
    float3 sum = s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3;
    return sum / max(w0 + w1 + w2 + w3, 1e-5f);
}

[numthreads(SPD_BLOCK, SPD_BLOCK, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    Texture2D srcTex = textures[spd.srcIndex];

    const uint2 tile0 = gid.xy * SPD_BLOCK; // mip 0 tile origin
    const uint2 px0 = tile0 + gtid.xy;      // this lane's mip 0 pixel

    // ---- Level 0: 2x2 HDR box + prefilter --------------------------------
    // Mip 0 pixel (x,y) covers HDR texels (2x,2y)..(2x+1,2y+1), clamped.
    const int2 hdrMax = int2(spd.hdrWidth, spd.hdrHeight) - 1;
    const int2 hdrBase = int2(px0 * 2u);
    float3 h0 = SanitizeHdr(srcTex.Load(int3(clamp(hdrBase, int2(0, 0), hdrMax), 0)).rgb);
    float3 h1 = SanitizeHdr(srcTex.Load(int3(clamp(hdrBase + int2(1, 0), int2(0, 0), hdrMax), 0)).rgb);
    float3 h2 = SanitizeHdr(srcTex.Load(int3(clamp(hdrBase + int2(0, 1), int2(0, 0), hdrMax), 0)).rgb);
    float3 h3 = SanitizeHdr(srcTex.Load(int3(clamp(hdrBase + int2(1, 1), int2(0, 0), hdrMax), 0)).rgb);
    float3 v0 = SpdPrefilter(SpdKaris4(h0, h1, h2, h3), spd.threshold, spd.knee);
    gsTile[SPD_GS0 + gtid.y * SPD_BLOCK + gtid.x] = float4(v0, 1.0f);
    if (px0.x < spd.baseWidth && px0.y < spd.baseHeight)
        spdImages[spd.dstIndex0][px0] = float4(v0, 1.0f);
    GroupMemoryBarrierWithGroupSync();

    // ---- Level 1: 8x8 from the 16x16 --------------------------------------
    if (gtid.x < 8 && gtid.y < 8)
    {
        uint2 p = uint2(gtid.x * 2u, gtid.y * 2u);
        float3 s0 = gsTile[SPD_GS0 + p.y * 16 + p.x].rgb;
        float3 s1 = gsTile[SPD_GS0 + p.y * 16 + min(p.x + 1u, 15u)].rgb;
        float3 s2 = gsTile[SPD_GS0 + min(p.y + 1u, 15u) * 16 + p.x].rgb;
        float3 s3 = gsTile[SPD_GS0 + min(p.y + 1u, 15u) * 16 + min(p.x + 1u, 15u)].rgb;
        float3 v1 = SpdKaris4(s0, s1, s2, s3);
        gsTile[SPD_GS1 + gtid.y * 8 + gtid.x] = float4(v1, 1.0f);
        uint2 px1 = gid.xy * 8u + gtid.xy;
        uint ew1 = max(spd.baseWidth >> 1u, 1u), eh1 = max(spd.baseHeight >> 1u, 1u);
        if (px1.x < ew1 && px1.y < eh1)
            spdImages[spd.dstIndex1][px1] = float4(v1, 1.0f);
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Level 2: 4x4 from the 8x8 ----------------------------------------
    if (gtid.x < 4 && gtid.y < 4)
    {
        uint2 p = uint2(gtid.x * 2u, gtid.y * 2u);
        float3 s0 = gsTile[SPD_GS1 + p.y * 8 + p.x].rgb;
        float3 s1 = gsTile[SPD_GS1 + p.y * 8 + min(p.x + 1u, 7u)].rgb;
        float3 s2 = gsTile[SPD_GS1 + min(p.y + 1u, 7u) * 8 + p.x].rgb;
        float3 s3 = gsTile[SPD_GS1 + min(p.y + 1u, 7u) * 8 + min(p.x + 1u, 7u)].rgb;
        float3 v2 = SpdKaris4(s0, s1, s2, s3);
        gsTile[SPD_GS2 + gtid.y * 4 + gtid.x] = float4(v2, 1.0f);
        uint2 px2 = gid.xy * 4u + gtid.xy;
        uint ew2 = max(spd.baseWidth >> 2u, 1u), eh2 = max(spd.baseHeight >> 2u, 1u);
        if (px2.x < ew2 && px2.y < eh2)
            spdImages[spd.dstIndex2][px2] = float4(v2, 1.0f);
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Level 3: 2x2 from the 4x4 ----------------------------------------
    if (gtid.x < 2 && gtid.y < 2)
    {
        uint2 p = uint2(gtid.x * 2u, gtid.y * 2u);
        float3 s0 = gsTile[SPD_GS2 + p.y * 4 + p.x].rgb;
        float3 s1 = gsTile[SPD_GS2 + p.y * 4 + min(p.x + 1u, 3u)].rgb;
        float3 s2 = gsTile[SPD_GS2 + min(p.y + 1u, 3u) * 4 + p.x].rgb;
        float3 s3 = gsTile[SPD_GS2 + min(p.y + 1u, 3u) * 4 + min(p.x + 1u, 3u)].rgb;
        float3 v3 = SpdKaris4(s0, s1, s2, s3);
        gsTile[SPD_GS3 + gtid.y * 2 + gtid.x] = float4(v3, 1.0f);
        uint2 px3 = gid.xy * 2u + gtid.xy;
        uint ew3 = max(spd.baseWidth >> 3u, 1u), eh3 = max(spd.baseHeight >> 3u, 1u);
        if (px3.x < ew3 && px3.y < eh3)
            spdImages[spd.dstIndex3][px3] = float4(v3, 1.0f);
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Level 4: 1x1 from the 2x2 ----------------------------------------
    if (gtid.x == 0 && gtid.y == 0)
    {
        float3 s0 = gsTile[SPD_GS3].rgb;
        float3 s1 = gsTile[SPD_GS3 + 1].rgb;
        float3 s2 = gsTile[SPD_GS3 + 2].rgb;
        float3 s3 = gsTile[SPD_GS3 + 3].rgb;
        float3 v4 = SpdKaris4(s0, s1, s2, s3);
        uint2 px4 = gid.xy;
        uint ew4 = max(spd.baseWidth >> 4u, 1u), eh4 = max(spd.baseHeight >> 4u, 1u);
        if (px4.x < ew4 && px4.y < eh4)
            spdImages[spd.dstIndex4][px4] = float4(v4, 1.0f);
    }
}
