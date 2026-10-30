// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define DOF_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "gpu_platform.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords (depth .Load)
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/dof.h DofBokehPush (56 bytes).
struct DofBokehPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    uint hdrTextureIndex;
    uint cocTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    float2 texelSize;
    float maxRadiusPx;
    float highlightBoost;
    float highlightThreshold;
    uint pad0;
    uint pad1;
    uint pad2;
};

GPU_PUSH(DofBokehPush, bp);

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = bp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(bp.hdrTextureIndex)];
    Texture2D cocTex = textures[NonUniformResourceIndex(bp.cocTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(bp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];

    float centerPacked = cocTex.Sample(smp, IN.uv).r;
    float centerR01 = DofUnpackRadius01(centerPacked);
    float centerRadiusPx = centerR01 * max(bp.maxRadiusPx, 0.0f);

    int2 px = int2(IN.pos.xy);
    float dCenter = depthTex.Load(int3(px, 0)).r;
    float2 invExtent = bp.texelSize;
    float2 ndcXY = float2((float(px.x) + 0.5f) * invExtent.x * 2.0f - 1.0f,
                          (float(px.y) + 0.5f) * invExtent.y * 2.0f - 1.0f);
    float centerDist = DofViewDist(dCenter, ndcXY, frame.invProj);
    bool centerBg = DofIsBackground(dCenter);

    float3 centerTap = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);
    if (centerRadiusPx < 0.5f)
    {
        OUT.outColor = float4(centerTap, 1.0f);
        return OUT;
    }

    // 48-tap hexagonal bokeh: 3 rings x 16 taps on a hex-shaped disc.
    float3 acc = centerTap;
    float wSum = 1.0f;
    for (uint ring = 0; ring < 3; ++ring)
    {
        float ringR = (float(ring) + 1.0f) / 3.0f;
        for (uint i = 0; i < 16; ++i)
        {
            float theta = (float(i) / 16.0f) * 6.2831853f + float(ring) * 0.2617994f;
            float hexR = DofHexRadius(theta);
            float2 dir = float2(cos(theta), sin(theta));
            float2 tapUv = IN.uv + dir * (hexR * ringR * centerRadiusPx) * bp.texelSize;

            float tapPacked = cocTex.Sample(smp, tapUv).r;
            float tapR01 = DofUnpackRadius01(tapPacked);
            float3 tap = SanitizeHdr(hdrTex.Sample(smp, tapUv).rgb);

            // Depth-aware foreground weight: taps clearly IN FRONT of the
            // center are occluders — downweight so foreground discs don't
            // smear over blurred backgrounds. Background taps always pass.
            int2 tapPx = int2(tapUv * float2(1.0f / max(invExtent.x, 1e-9f),
                                             1.0f / max(invExtent.y, 1e-9f)));
            float tapD = depthTex.Load(int3(tapPx, 0)).r;
            bool tapBg = DofIsBackground(tapD);
            float w = 0.5f + 0.5f * tapR01; // blurred taps matter more
            if (!tapBg && !centerBg)
            {
                float2 tapNdc = float2((float(tapPx.x) + 0.5f) * invExtent.x * 2.0f - 1.0f,
                                       (float(tapPx.y) + 0.5f) * invExtent.y * 2.0f - 1.0f);
                float tapDist = DofViewDist(tapD, tapNdc, frame.invProj);
                if (tapDist < centerDist * 0.98f)
                    w *= 0.25f;
            }
            else if (centerBg != tapBg)
            {
                w *= 0.5f;
            }

            // Highlight boost: bright taps (sun, lamps, window speculars)
            // punch disc-shaped bokeh instead of averaging away.
            float lum = dot(tap, float3(0.2126f, 0.7152f, 0.0722f));
            float hi = saturate((lum - bp.highlightThreshold) / max(bp.highlightThreshold, 1e-3f));
            w *= 1.0f + (max(bp.highlightBoost, 1.0f) - 1.0f) * hi;

            acc += tap * w;
            wSum += w;
        }
    }
    OUT.outColor = float4(acc / max(wSum, 1e-5f), 1.0f);
    return OUT;
}
