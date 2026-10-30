// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define GODRAYS_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "godrays_common.hlsli"
#include "gpu_platform.hlsli"

// Screen-space god rays, stage 1 - the SUN OCCLUSION MASK (quarter
// res): where the sun's glow region is VISIBLE SKY. Per pixel:
//   skyVisible = the scene depth is background (reversed-Z, the
//                DofIsBackgroundOrGarbage test the volumetric march
//                uses) - geometry (a roof, a wall in front of the sun)
//                blanks the mask there, and the radial stage gathering
//                through those blanks is what makes the shafts.
//   sunShape   = 1 - smoothstep(inner, outer, |uv - sunUv|) - a
//                TUNABLE GLOW RADIUS, not a tiny physical disc: the
//                radial filter needs illuminated area to gather from
//                (the plan's note).
// mask = skyVisible * sunShape. Off-screen / behind-camera sun: the
// whole mask is 0 (no special casing downstream).
//
// The CSM fog shadowing gates OFF under EVSM (the raw atlas is not
// written in that mode); this chain reads ONLY the scene depth, so it
// is the fog sun-occlusion FALLBACK under EVSM - and the independent
// A/B against CSM in PCF mode.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: r = the mask
};

// Must match Engine/Rendergraph/godrays.h GodraysMaskPush (32 bytes).
struct GodraysMaskPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    uint depthTextureIndex; // full-res scene depth (sky detection)
    float invDepthW;        // 1 / full-res depth extent
    float invDepthH;
    float glowInner;        // uv radius of the full-strength glow
    float glowOuter;        // uv radius where the glow fades to 0
    uint pad0;
};

GPU_PUSH(GodraysMaskPush, mp);

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;

    FrameUniforms frame = mp.frameUniforms.Get();
    float2 sunUv;
    bool sunValid;
    GodraysSunUv(frame, sunUv, sunValid);
    if (!sunValid)
    {
        OUT.outColor = float4(0.0, 0.0, 0.0, 1.0);
        return OUT;
    }

    // The quarter-res pixel maps onto the FULL-res depth grid by uv
    // (the depth tap decides sky vs geometry at this pixel's actual
    // position - a quarter-res mask still resolves silhouettes well
    // past what the 1/16 radial stage can carry).
    Texture2D depthTex = textures[NonUniformResourceIndex(mp.depthTextureIndex)];
    float2 depthSize = float2(1.0 / max(mp.invDepthW, 1e-9),
                              1.0 / max(mp.invDepthH, 1e-9));
    int2 depthPx = int2(clamp(IN.uv * depthSize - 0.5,
        float2(0.0, 0.0), depthSize - 1.0));
    const float sceneDepth = depthTex.Load(int3(depthPx, 0)).r;

    const float skyVisible =
        DofIsBackgroundOrGarbage(sceneDepth) ? 1.0 : 0.0;

    const float sunShape = 1.0 - smoothstep(mp.glowInner, mp.glowOuter,
        distance(IN.uv, sunUv));

    const float mask = skyVisible * sunShape;
    OUT.outColor = float4(mask, 0.0, 0.0, 1.0);
    return OUT;
}
