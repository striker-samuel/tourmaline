// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "gpu_platform.hlsli"

// EVSM separable Gaussian prefilter — shared H/V kernel body.
//
// One dispatch per tile per leg (caller loops tiles; the rendergraph gets
// one AsyncCompute pass per leg — H: moments(resolved MSAA) -> temp,
// V: temp -> blurred, which light then samples). The two legs are separate
// entry points (evsm_blur_h.cs.hlsl / evsm_blur_v.cs.hlsl) that include
// this header and pick the axis with EVSM_BLUR_AXIS, so the direction
// never exists as a runtime branch and each leg's address math compiles
// straight-line.
//
// Cost budget per OUTPUT TEXEL (the old single-shader two-leg form did:
// 18 filtered RGBA32F samples + 18 exp() + 18 UV clamps + 18 sanitizing
// clamps + 2 weight normalizations):
//  - Weights: the Gaussian is dispatch-uniform (one blurRadius -> one
//    sigma for the whole dispatch, and sigma = clamp(radius/2, 1, 3)
//    only takes a handful of values), so the CPU bakes the FIVE
//    normalized symmetric weights (w0..w4, sum = 1) into the push:
//    no shader exp(), no wSum accumulation, no final division.
//  - Taps: every tap sits on an exact texel center, so the kernel
//    Load()s (OpImageFetch) through the bindless textures[] array — no
//    UV math, no sampler, no filtering. srcIndex/dstIndex are
//    dispatch-uniform too, so no NonUniformResourceIndex.
//  - Halo: a 16x16 workgroup used to issue 256 x 9 = 2304 global
//    fetches per leg. Instead the group loads its 24x16 (H) / 16x24 (V)
//    source patch ONCE into groupshared (384 texels for 256 outputs —
//    1.5 global fetches per output instead of 9) and every tap
//    convolves out of LDS. 6 KB per workgroup.
//  - Edge clamp: the load clamps every patch texel into the tile's
//    TEXEL bounds [tileOffset, tileOffset + tileExtent - 1] —
//    bit-identical to the old half-texel-inset UV clamp (uvMin was the
//    CENTER of the first texel, uvMax the CENTER of the last), so
//    sibling tiles still never bleed. The clamp also bakes the edge
//    behavior into the patch, so the convolve needs NO clamping at
//    all: tap (HALO + local +- k) is always inside [0, PATCH) by
//    construction (local < BLOCK, k <= HALO).
//  - Sanitize: only the H leg sanitizes, ONCE PER SOURCE TEXEL (not per
//    tap). The V leg trusts its input: its source is the H output, and
//    a convex combination of finite clamped values is finite and
//    clamped, so re-clamping 9 V taps is pure waste. (The producer side
//    is clean by construction anyway: every moment pass clears its
//    tile to the far-plane warp and both FS legs write
//    saturate(depth)-warped moments. The H-load clamp stays as cheap
//    defense-in-depth against one garbage tap poisoning a whole blur
//    window.)
//
// Tile discipline: out-of-tile threads exit AFTER the cooperative patch
// load + barrier — a thread that returned early would skip its share of
// the loads and strand the group. The destination tile stays fully
// covered.
//
// C++ mirror: smvk::EvsmBlurPush in Engine/Rendergraph/evsm.h (48 bytes).

// Workgroup + halo geometry (kEvsmBlurBlock on the C++ side drives the
// dispatch tiling; EVSM_BLUR_HALO is the 9-tap kernel support: taps -4..+4).
#define EVSM_BLUR_BLOCK 16
#define EVSM_BLUR_HALO 4
#define EVSM_BLUR_PATCH (EVSM_BLUR_BLOCK + 2 * EVSM_BLUR_HALO)

// Patch layout: 24x16 for the H leg (halo on x), 16x24 for the V leg
// (halo on y). Stored row-major, so the row stride is the patch WIDTH —
// the axis branch collapses into which coordinate carries the halo.
#if EVSM_BLUR_AXIS == 0
#define EVSM_BLUR_PATCH_W EVSM_BLUR_PATCH
#define EVSM_BLUR_PATCH_H EVSM_BLUR_BLOCK
#else
#define EVSM_BLUR_PATCH_W EVSM_BLUR_BLOCK
#define EVSM_BLUR_PATCH_H EVSM_BLUR_PATCH
#endif

struct EvsmBlurPush
{
    uint srcIndex;    // bindless textures[] index this leg Load()s from
    uint dstIndex;    // bindless storage-image index this leg writes to
    uint tileOffsetX; // tile origin in atlas texels (edge clamp min)
    uint tileOffsetY;
    uint tileWidth;   // tile extent in texels (early-out + edge clamp max)
    uint tileHeight;
    float w0;         // CPU-baked normalized symmetric Gaussian weights:
    float w1;         // sigma = clamp(blurRadius/2, 1, 3),
    float w2;         // w_k = exp(-k^2 / (2 sigma^2)) / wSum over taps
    float w3;         // -4..+4. The whole kernel rides in the push —
    float w4;         // the shader never evaluates exp().
    uint pad0;
};

GPU_PUSH(EvsmBlurPush, pcBlur);

// Bindless storage image array (set 0, binding 2). Declared here — the
// only TUs that write storage images are the two blur legs — so no other
// pass pays the declaration. float4 MUST match the RGBA32F moment atlas:
// an OpTypeImage Format operand mismatched against the view's VkFormat is
// undefined for the whole image (GPU-assisted validation flags it;
// "undefined values" poison the loads/stores driver-dependently).
[[vk::binding(2, 0)]]
RWTexture2D<float4> evsmStorage[];

// The source patch shared by the group: 384 float4 = 6 KB.
groupshared float4 gsPatch[EVSM_BLUR_PATCH_W * EVSM_BLUR_PATCH_H];

void EvsmBlurTile(uint3 gtid, uint3 gid)
{
    const int2 tileMin = int2(pcBlur.tileOffsetX, pcBlur.tileOffsetY);
    const int2 tileMax = tileMin + int2(pcBlur.tileWidth, pcBlur.tileHeight) - 1;

    // Output block origin (atlas texels) and the source patch that feeds
    // it: the block shifted by -HALO along the BLUR AXIS only (the cross
    // axis needs no halo — its taps sit on the output texel itself).
    const int2 blockOrigin = tileMin + int2(gid.xy) * EVSM_BLUR_BLOCK;
#if EVSM_BLUR_AXIS == 0
    const int2 patchOrigin = blockOrigin - int2(EVSM_BLUR_HALO, 0);
#else
    const int2 patchOrigin = blockOrigin - int2(0, EVSM_BLUR_HALO);
#endif

    // Cooperative patch load: 384 texels over 256 lanes = <=2 iterations
    // per lane. Every fetch clamps into the tile — that IS the edge
    // behavior (see header) and the safety bound (clamped coords are
    // always inside the atlas image).
    Texture2D srcTex = textures[pcBlur.srcIndex];
    const uint lane = gtid.y * EVSM_BLUR_BLOCK + gtid.x;
    for (uint i = lane; i < EVSM_BLUR_PATCH_W * EVSM_BLUR_PATCH_H; i += 256u)
    {
        const int2 p = patchOrigin
            + int2(i % EVSM_BLUR_PATCH_W, i / EVSM_BLUR_PATCH_W);
        float4 m = srcTex.Load(int3(clamp(p, tileMin, tileMax), 0));
#if EVSM_BLUR_AXIS == 0
        // Sanitize ONCE PER SOURCE TEXEL, H leg only (see header): a
        // single inf/NaN moment (fp32 overflow under a huge exponent, or
        // an uncleared tile) poisons the whole blur window. Channel layout
        // is (M1+, M2+, M1-, M2-) of the two exponential warps (see
        // evsm.hlsli). Floor care: the NEGATIVE first moment spans e^-c
        // (d=1) .. 1 (d=0) — with c=42 that is 5.7e-19 — and the negative
        // second moment spans e^-2c .. 1 (3.3e-37 at c=42, still a
        // normal fp32). Floors must stay BELOW those leges: clamping M1-
        // up to e.g. 1e-6 would erase all blocker-depth information
        // below d ~ 0.33 and kill the soft-shadow penumbra estimator
        // (EvsmBlockerDepth reads M1-); clamping M2- above e^-2c poisons
        // the negative Cantelli variance.
        m = clamp(m,
            float4(1e-6f, 1e-6f, 1e-20f, 1e-38f),
            float4(3.0e37f, 3.0e37f, 1.0f, 1.0f));
#endif
        gsPatch[i] = m;
    }
    GroupMemoryBarrierWithGroupSync();

    // Out-of-tile threads exit AFTER the patch load + barrier (see header).
    if (gid.x * EVSM_BLUR_BLOCK + gtid.x >= pcBlur.tileWidth
        || gid.y * EVSM_BLUR_BLOCK + gtid.y >= pcBlur.tileHeight)
        return;

    // 9-tap symmetric convolution straight out of LDS — no clamping (the
    // load-phase clamp already encoded the tile edges into the patch, and
    // (HALO + local +- k) is always in [0, PATCH)). The patch is stored
    // ROW-MAJOR (flat = row * patchWidth + col) and which LOCAL axis is
    // the row flips with the leg: H's rows are the cross axis (y), blur
    // runs along the columns; V's rows ARE the blur axis (y again — the
    // halo rides the rows). So the TAP STEP must use the blur-axis stride:
    // +-k columns for H (stride 1), +-k rows for V (stride patchWidth). A
    // plain flat +-k is only correct for H — for V it walks COLUMNS and
    // wraps into the previous row at col < k, so the leg silently blurs
    // the wrong axis with plausible-looking output: the mean stays right
    // (same texel population) while the tile x-gradient rises ~10% above
    // the input's — impossible for a true cross-axis blur, and exactly
    // what the EVSM dump's gradient stat caught (the naked eye did not).
#if EVSM_BLUR_AXIS == 0
    const int row = int(gtid.y);
    const int col = EVSM_BLUR_HALO + int(gtid.x);
    const int centerFlat = row * EVSM_BLUR_PATCH_W + col;
    const int tapStride = 1;
#else
    const int row = EVSM_BLUR_HALO + int(gtid.y);
    const int col = int(gtid.x);
    const int centerFlat = row * EVSM_BLUR_PATCH_W + col;
    const int tapStride = EVSM_BLUR_PATCH_W;
#endif
    const float wk[4] = { pcBlur.w1, pcBlur.w2, pcBlur.w3, pcBlur.w4 };
    float4 acc = gsPatch[centerFlat] * pcBlur.w0;
    [unroll]
    for (int k = 1; k <= EVSM_BLUR_HALO; ++k)
    {
        const float4 pair = gsPatch[centerFlat - k * tapStride] + gsPatch[centerFlat + k * tapStride];
        acc += pair * wk[k - 1];
    }

    // float4 store: the atlas is RGBA32F — all four moments are real.
    // (A float2 store against an RG32F view was the old format-mismatch
    // hazard; the same rule applies with one more channel.)
    evsmStorage[pcBlur.dstIndex][uint2(blockOrigin + int2(gtid.xy))] = acc;
}
