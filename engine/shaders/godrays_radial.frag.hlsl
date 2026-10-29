// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define GODRAYS_PASS
#include "common.hlsli"
#include "godrays_common.hlsli"

// Screen-space god rays, stage 2 - the RADIAL SCATTER (1/16 res): per
// destination pixel, march TOWARD the projected sun gathering the
// occlusion mask with the classic decayed radial kernel -
//
//   delta = (sunUv - uv) * reach / N      (the march step)
//   rays = sum_i mask(uv + delta*(i+noise)) * decay^i  * weight / N
//
// Samples that pass through LIT mask regions (visible sky in the sun's
// glow) add light; samples through occluded regions (geometry in front
// of the sun) add nothing - the accumulated pattern IS the shaft
// structure. The per-pixel IGN start dither (GodraysIgn) breaks the
// concentric banding the discrete 32-sample kernel would otherwise
// ring around the sun; decay (0.97) weights near samples over far.
//
// A FRAGMENT fullscreen pass at the 1/16 extent (not compute): the
// engine's post chain is fullscreen-triangle fragment passes
// (tonemap.vert + PS at SetExtent), the destination is an ordinary
// color target the later stages sample bilinearly, and there is no
// workgroup-shared structure a compute form would exploit - the house
// shape is the simpler, proven one.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: r = the gathered rays
};

// Must match Engine/Rendergraph/godrays.h GodraysRadialPush
// (40 bytes). All-scalar layout on purpose (the spirv-val
// straddling-vector hazard).
struct GodraysRadialPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms; // the sun Uv
    uint maskTextureIndex; // the quarter-res occlusion mask
    uint samplerIndex;     // linear clamp (the march taps)
    uint sampleCount;      // the kernel length, 4..64
    float decay;           // per-sample illumination decay (0.97)
    float reach;           // march length as a fraction of the uv
                           // distance to the sun (1.0 = all the way)
    float weight;          // the normalization pre-scale
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<GodraysRadialPush> rp;

static const uint kGodraysMaxSamples = 64;

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;

    FrameUniforms frame = rp.frameUniforms.Get();
    float2 sunUv;
    bool sunValid;
    GodraysSunUv(frame, sunUv, sunValid);
    if (!sunValid)
    {
        OUT.outColor = float4(0.0, 0.0, 0.0, 1.0);
        return OUT;
    }

    Texture2D maskTex = textures[NonUniformResourceIndex(rp.maskTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(rp.samplerIndex)];

    const uint n = clamp(rp.sampleCount, 4u, kGodraysMaxSamples);
    const float invN = 1.0 / float(n);
    const float2 delta = (sunUv - IN.uv) * saturate(rp.reach) * invN;

    // The IGN start dither: sample i lands at uv + delta*(i + noise)
    // instead of the integer lattice - the ring breaker.
    const float noise = GodraysIgn(pos.xy);
    float2 sampleUv = IN.uv + delta * noise;

    float illumination = 1.0;
    float rays = 0.0;
    [loop]
    for (uint i = 0; i < kGodraysMaxSamples; ++i)
    {
        if (i >= n)
            break;
        // Out-of-range march taps: skip (the linear-clamp sampler would
        // smear border duplicates into the accumulation - the same
        // border contract the SSR filters use).
        if (all(sampleUv >= float2(0.0, 0.0)) && all(sampleUv <= float2(1.0, 1.0)))
            rays += maskTex.SampleLevel(smp, sampleUv, 0.0).r * illumination;
        illumination *= saturate(rp.decay);
        sampleUv += delta;
    }
    rays *= max(rp.weight, 0.0) * invN;

    OUT.outColor = float4(rays, 0.0, 0.0, 1.0);
    return OUT;
}
