// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define BLOOM_PASS
#include "common.hlsli"

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/bloom.h BloomDownPush (32 bytes).
struct BloomDownPush
{
    uint srcTextureIndex;
    uint samplerIndex;
    float threshold;
    float knee;
    float2 srcTexelSize;
    uint applyThreshold;
    uint pad;
};

[[vk::push_constant]]
ConstantBuffer<BloomDownPush> bp;

float BloomLuma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// A single +inf tap (half-float overflow of sun/specular highlights in the
// RGBA16F HDR target) poisons the Karis average below: w = 1/(1+inf) = 0,
// so the tap contribution inf*0 = NaN (and Prefilter hits inf/inf = NaN).
// NaN then spreads through every downsample level, the tent upsample smears
// it back up into screen-sized blocks, and the tonemap composites
// hdr += NaN with saturate(NaN) -> 0: big black squares that toggle with
// bloom. Clamp taps to finite half range (SanitizeHdr, common.hlsli) so
// the chain only carries finite energy; identical look for all finite
// input.
float3 SanitizeBloomTap(float3 c)
{
    return SanitizeHdr(c);
}

// Soft-knee prefilter (Unreal/Karis): keeps highlights, rolls off below threshold.
float3 Prefilter(float3 c, float threshold, float knee)
{
    float luma = BloomLuma(c);
    float soft = clamp(luma - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * max(knee, 1e-4f));
    float contribution = max(soft, luma - threshold) / max(luma, 1e-4f);
    return c * contribution;
}

// 13-tap downsample with Karis average (firefly suppression).
// Each tap weighted by 1/(1+luma), renormalized – bright outliers don't dominate.
float3 Downsample13(Texture2D src, SamplerState smp, float2 uv, float2 texel)
{
    float3 s0  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2(-2.0f, -2.0f)).rgb);
    float3 s1  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 0.0f, -2.0f)).rgb);
    float3 s2  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 2.0f, -2.0f)).rgb);
    float3 s3  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2(-1.0f, -1.0f)).rgb);
    float3 s4  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 1.0f, -1.0f)).rgb);
    float3 s5  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2(-1.0f,  1.0f)).rgb);
    float3 s6  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 1.0f,  1.0f)).rgb);
    float3 s7  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2(-2.0f,  0.0f)).rgb);
    float3 s8  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 2.0f,  0.0f)).rgb);
    float3 s9  = SanitizeBloomTap(src.Sample(smp, uv + texel * float2(-2.0f,  2.0f)).rgb);
    float3 s10 = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 0.0f,  2.0f)).rgb);
    float3 s11 = SanitizeBloomTap(src.Sample(smp, uv + texel * float2( 2.0f,  2.0f)).rgb);
    float3 s12 = SanitizeBloomTap(src.Sample(smp, uv).rgb);

    float w0 = 1.0f / (1.0f + BloomLuma(s0));
    float w1 = 1.0f / (1.0f + BloomLuma(s1));
    float w2 = 1.0f / (1.0f + BloomLuma(s2));
    float w3 = 1.0f / (1.0f + BloomLuma(s3));
    float w4 = 1.0f / (1.0f + BloomLuma(s4));
    float w5 = 1.0f / (1.0f + BloomLuma(s5));
    float w6 = 1.0f / (1.0f + BloomLuma(s6));
    float w7 = 1.0f / (1.0f + BloomLuma(s7));
    float w8 = 1.0f / (1.0f + BloomLuma(s8));
    float w9 = 1.0f / (1.0f + BloomLuma(s9));
    float w10 = 1.0f / (1.0f + BloomLuma(s10));
    float w11 = 1.0f / (1.0f + BloomLuma(s11));
    float w12 = 1.0f / (1.0f + BloomLuma(s12));

    // Center + axis taps get slightly higher weight (Jimenez pattern).
    float3 sum = s0 * (w0 * 0.5f) + s1 * w1 + s2 * (w2 * 0.5f)
               + s3 * w3 + s4 * w4 + s5 * w5 + s6 * w6
               + s7 * w7 + s8 * w8
               + s9 * (w9 * 0.5f) + s10 * w10 + s11 * (w11 * 0.5f)
               + s12 * (w12 * 2.0f);
    float wsum = w0 * 0.5f + w1 + w2 * 0.5f
               + w3 + w4 + w5 + w6 + w7 + w8
               + w9 * 0.5f + w10 + w11 * 0.5f
               + w12 * 2.0f;
    return sum / max(wsum, 1e-5f);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D src = textures[NonUniformResourceIndex(bp.srcTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];

    float3 c = Downsample13(src, smp, IN.uv, bp.srcTexelSize);
    if (bp.applyThreshold != 0)
        c = Prefilter(c, bp.threshold, bp.knee);

    OUT.outColor = float4(c, 1.0f);
    return OUT;
}
