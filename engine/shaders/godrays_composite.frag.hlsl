// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define GODRAYS_PASS
#include "common.hlsli"
#include "dof.hlsli"

// Screen-space god rays, stage 4 - the HDR COMPOSITE (full res): the
// filtered screen-space rays added ADDITIVELY over the scene HDR,
// modulated by the FOG IN FRONT of the visible scene depth -
//
//   fogFront  = 1 - exp(-fogDensity * viewDist(sceneDepth))
//   contrib   = rays * sunHdr * intensity * fogFront
//   result    = base + contrib
//
// The fogFront is the plan's foreground-shortening: a near surface
// leaves little fog between it and the camera, so its screen-space ray
// contribution fades even where the upsampled rays field is bright;
// sky/far pixels take the full term (viewDist clamped at the fog max
// distance). fogDensity is the AUTHORED base-zone density (the env
// table owns the fog look - the retired panel slider's lesson).
//
// ADDITIVE, NOT a CSM multiplier: the CSM leg already occludes the
// march's sun term (SampleFogSunCSM), and this is a separate,
// view-dependent scattering approximation - multiplying the two would
// darken the fog twice (the plan's warning). The two are independently
// toggleable at the app (CSM fog shadows vs screen-space rays) for the
// A/B; under EVSM the CSM leg gates off (stale raw atlas) and this
// chain is the fog sun-occlusion fallback.
//
// Debug modes (the push's debugMode - TOURMALINE_GODRAYS_DEBUG):
//   1 = the raw sun occlusion mask (upsampled)
//   2 = the unfiltered radial rays
//   3 = the filtered fog-shadow field
//   4 = the final screen-space contribution term
// The 0..1 views carry the dbgGain pre-scale (1/exposure - Main's
// flat-debug-view trick: the physical-units exposure crushes a bare
// 0..1 readout to black); mode 4 is HDR-scale like the base and rides
// the scene exposure directly. Each REPLACES the composite (the view
// IS the diagnostic - the FOG_GI_DEBUG discipline).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: the ray-composited HDR
};

// Must match Engine/Rendergraph/godrays.h GodraysCompositePush
// (64 bytes). uint64 BDA leads; scalars after.
struct GodraysCompositePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;     // the scene HDR to composite over
    uint depthTextureIndex;   // full-res scene depth (fogFront)
    uint raysTextureIndex;    // the FILTERED 1/16 rays
    uint raysRawTextureIndex; // the unfiltered radial output (debug 2)
    uint maskTextureIndex;    // the quarter-res mask (debug 1)
    uint samplerIndex;        // linear clamp (HDR + rays taps)
    float invDepthW;          // 1 / full-res depth extent
    float invDepthH;
    float intensity;          // the artistic strength (subtle default)
    float fogDensity;         // the AUTHORED base-zone extinction
    float maxDistance;        // the fog distance clamp (sky pixels)
    uint debugMode;           // 0 off, 1 mask, 2 raw, 3 filtered, 4 term
    uint useSun;              // the volumetric sun master (0 = no leg)
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<GodraysCompositePush> cp;

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(cp.hdrTextureIndex)];
    Texture2D raysTex = textures[NonUniformResourceIndex(cp.raysTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(cp.samplerIndex)];

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    // Debug views 1-3: the 0..1 intermediates, dbgGain-scaled.
    if (cp.debugMode >= 1u && cp.debugMode <= 3u)
    {
        float dbg = 0.0;
        if (cp.debugMode == 1u)
        {
            Texture2D maskTex =
                textures[NonUniformResourceIndex(cp.maskTextureIndex)];
            dbg = maskTex.SampleLevel(smp, IN.uv, 0.0).r;
        }
        else if (cp.debugMode == 2u)
        {
            Texture2D rawTex =
                textures[NonUniformResourceIndex(cp.raysRawTextureIndex)];
            dbg = rawTex.SampleLevel(smp, IN.uv, 0.0).r;
        }
        else
        {
            dbg = raysTex.SampleLevel(smp, IN.uv, 0.0).r;
        }
        const float dbgGain = 1.0 / max(frame.exposure, 1e-6);
        OUT.outColor = float4((dbg * dbgGain).xxx, 1.0);
        return OUT;
    }

    // The filtered rays, bilinearly upsampled to full res.
    const float rays = raysTex.SampleLevel(smp, IN.uv, 0.0).r;

    // fogFront: the fog between the camera and the VISIBLE surface.
    Texture2D depthTex = textures[NonUniformResourceIndex(cp.depthTextureIndex)];
    float2 depthSize = float2(1.0 / max(cp.invDepthW, 1e-9),
                              1.0 / max(cp.invDepthH, 1e-9));
    int2 depthPx = int2(clamp(IN.uv * depthSize - 0.5,
        float2(0.0, 0.0), depthSize - 1.0));
    const float ndcDepth = depthTex.Load(int3(depthPx, 0)).r;
    float viewDist;
    if (DofIsBackgroundOrGarbage(ndcDepth))
    {
        // Sky: the full fog interval (clamped at the march range).
        viewDist = max(cp.maxDistance, 1.0);
    }
    else
    {
        float2 ndc = IN.uv * 2.0 - 1.0;
        viewDist = DofViewDist(ndcDepth, ndc, frame.invProj);
        viewDist = min(viewDist, max(cp.maxDistance, 1.0));
    }
    const float fogFront =
        1.0 - exp(-max(cp.fogDensity, 0.0) * viewDist);

    // The sun term: the scene env's sun color/intensity (the same
    // sunHdr the volumetric march uses), gated by the shared sun
    // master.
    float3 sunHdr = float3(0.0, 0.0, 0.0);
    if (cp.useSun != 0u)
        sunHdr = frame.env.sunColor.rgb * max(frame.env.sunColor.w, 0.0);

    const float3 contrib = rays * sunHdr
        * max(cp.intensity, 0.0) * fogFront;

    // Mode 4: the contribution term alone (HDR scale - rides the
    // scene exposure, no gain).
    if (cp.debugMode == 4u)
    {
        OUT.outColor = float4(SanitizeHdr(contrib), 1.0);
        return OUT;
    }

    const float3 outColor = max(base + contrib, float3(0.0, 0.0, 0.0));
    OUT.outColor = float4(SanitizeHdr(outColor), 1.0);
    return OUT;
}
