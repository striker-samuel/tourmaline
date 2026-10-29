// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define VOLUMETRIC_COMPOSITE_PASS
#include "common.hlsli"
#include "dof.hlsli"

// Volumetric fog COMPOSITE (the full-resolution half of the perf
// split): the depth-aware bilateral upsample of the half-res
// (scatter, throughput) fog + the scene HDR reconstruction -
//
//   fog = sum_i  w_i * fogTex_i / sum_i w_i
//   w_i = bilinear_i * exp(-|z_i - z_full| / max(0.1, 0.02 * z_full))
//   out = hdr * fog.a + fog.rgb
//
// z_i = the half-res sample's representative VIEW DISTANCE (the
// integrate pass's R32F target - LINEAR distance, never raw
// reversed-Z, which is non-linear and would misweight the band);
// z_full = this pixel's view distance (background clamps to the fog
// max distance, matching the sky-side reps). At a silhouette, if the
// bilateral weights collapse (the neighborhood spans surfaces at very
// different depths), the NEAREST-DEPTH tap takes over (the plan's
// fallback - never a normalized average of unrelated fog).
//
// The output extent is the SAME full-res HDR target the old combined
// pass wrote (the old half-res fog mode broke exposure/DLSS/tonemap
// by shrinking the HDR OUTPUT - this split keeps the output 1:1 and
// shrinks only the march). The debug views ride the integrate pass's
// (dbg, 0) outputs: transmittance 0 kills the base here, the pure
// upsampled diagnostic shows through.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: hdr * throughput + scatter
};

// Must match Engine/Rendergraph/volumetric.h VolumetricCompositePush
// (48 bytes). uint64 BDA leads; scalars after.
struct VolumetricCompositePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms; // invProj (view dist)
    uint hdrTextureIndex;      // the FROZEN scene HDR (pre-fog)
    uint fogTextureIndex;      // half-res (scatter.rgb, throughput.a)
    uint fogDepthTextureIndex; // half-res rep view distance (R32F)
    uint depthTextureIndex;    // full-res scene depth
    uint samplerIndex;         // linear clamp (the HDR tap)
    float invDepthW;           // 1 / full-res depth extent
    float invDepthH;
    float maxDistance;         // the sky clamp (matches the integrate)
    float depthFalloff;        // the bilateral band scale (0.02: the
                               // falloff grows with distance)
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<VolumetricCompositePush> cp;

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(cp.hdrTextureIndex)];
    Texture2D fogTex = textures[NonUniformResourceIndex(cp.fogTextureIndex)];
    Texture2D fogDepthTex =
        textures[NonUniformResourceIndex(cp.fogDepthTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(cp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(cp.samplerIndex)];

    const float3 hdr = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    // This pixel's view distance (the bilateral key): background
    // clamps to the fog max distance - the same value the sky-side
    // half-res reps carry, so sky neighborhoods weight coherently.
    float2 depthSize = float2(1.0 / max(cp.invDepthW, 1e-9),
                              1.0 / max(cp.invDepthH, 1e-9));
    int2 fullPx = int2(pos.xy);
    const float ndcDepth = depthTex.Load(int3(fullPx, 0)).r;
    float zFull;
    if (DofIsBackgroundOrGarbage(ndcDepth))
    {
        zFull = max(cp.maxDistance, 1.0);
    }
    else
    {
        float2 ndc = IN.uv * 2.0 - 1.0;
        zFull = min(DofViewDist(ndcDepth, ndc, frame.invProj),
            max(cp.maxDistance, 1.0));
    }

    // The four neighboring half-res texels (the 2x2 tent addressing of
    // the SSR/AO upsamples): half-res grid position of this pixel.
    float2 halfSize = depthSize * 0.5;
    float2 prevPx = IN.uv * halfSize - 0.5;
    int2 p0 = int2(floor(prevPx));
    float2 f = frac(prevPx);
    const int2 halfMax = int2(halfSize) - 1;

    // The bilateral band: the falloff grows with distance (2% of z),
    // floored at 0.1 m so near-surface edges stay tight.
    const float band = max(0.1, cp.depthFalloff * max(zFull, 1e-3));

    float4 fogAcc = float4(0.0, 0.0, 0.0, 0.0);
    float wSum = 0.0;
    float bestDz = 1e30;
    float4 bestFog = float4(0.0, 0.0, 0.0, 1.0);
    [unroll]
    for (int ty = 0; ty < 2; ++ty)
    {
        [unroll]
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = p0 + int2(tx, ty);
            if (hp.x < 0 || hp.y < 0 || hp.x > halfMax.x || hp.y > halfMax.y)
                continue; // border candidate: skip, the normalization absorbs
            const float2 tapUv = (float2(hp) + 0.5) / halfSize;
            // Point taps (the four-texel tent is the filter - a linear
            // sample would double-blur across the same neighborhood).
            const float4 tapFog = fogTex.Load(int3(hp, 0));
            const float zTap = fogDepthTex.Load(int3(hp, 0)).r;

            const float dz = abs(zTap - zFull);
            // The silhouette fallback tracker: the nearest-depth tap.
            if (dz < bestDz)
            {
                bestDz = dz;
                bestFog = tapFog;
            }

            const float wBilinear = (tx ? f.x : 1.0 - f.x)
                * (ty ? f.y : 1.0 - f.y);
            const float wDepth = exp(-dz / band);
            const float w = wBilinear * wDepth;
            fogAcc += tapFog * w;
            wSum += w;
        }
    }

    // Collapse (a silhouette: every tap's depth is far from this
    // pixel's): the NEAREST-DEPTH tap stands in - never a normalized
    // mix of unrelated surfaces' fog.
    const float4 fog = (wSum > 1e-4) ? fogAcc / wSum : bestFog;

    const float3 outColor = max(
        hdr * saturate(fog.a) + fog.rgb,
        float3(0.0, 0.0, 0.0));
    OUT.outColor = float4(SanitizeHdr(outColor), 1.0);
    return OUT;
}
