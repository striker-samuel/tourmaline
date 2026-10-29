// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define GODRAYS_PASS
#include "common.hlsli"

// Screen-space god rays, stage 3 - the FOG-SHADOW SMOOTHING (1/16,
// same extent as the radial stage): a SMALL 5x5 Gaussian ({6,4,1} axis
// weights - the repo's bilateral table, minus the bilateral gates: the
// rays field has no depth structure to protect) over the raw radial
// output. Deliberately tight: the job is knocking the 1/16 march's
// residual ring/dither noise down before the bilinear full-res
// upsample, NOT blurring the shaft structure away (the plan's warning
// - aggressive filtering here erases exactly the thin beams the
// effect exists for).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: r = the smoothed rays
};

// Must match Engine/Rendergraph/godrays.h GodraysFilterPush
// (24 bytes).
struct GodraysFilterPush
{
    uint raysTextureIndex; // the raw 1/16 radial output
    uint samplerIndex;     // linear clamp
    float texelX;          // 1 / sixteenth-res width
    float texelY;          // 1 / sixteenth-res height
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<GodraysFilterPush> fp;

float GodraysAxisWeight(int k)
{
    const int a = abs(k);
    if (a == 0) return 6.0;
    if (a == 1) return 4.0;
    return 1.0;
}

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    Texture2D raysTex = textures[NonUniformResourceIndex(fp.raysTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(fp.samplerIndex)];

    const int radius = 2; // the 5x5: tight on purpose (see the header)
    float acc = 0.0;
    float wSum = 0.0;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            const float w = GodraysAxisWeight(x) * GodraysAxisWeight(y);
            acc += raysTex.SampleLevel(smp,
                IN.uv + float2(float(x) * fp.texelX, float(y) * fp.texelY),
                0.0).r * w;
            wSum += w;
        }
    }
    const float rays = (wSum > 1e-5) ? acc / wSum : 0.0;
    OUT.outColor = float4(rays, 0.0, 0.0, 1.0);
    return OUT;
}
