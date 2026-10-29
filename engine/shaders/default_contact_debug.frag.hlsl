// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "light.hlsli"
#include "common.hlsli"

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

// Sun contact-shadow CONTRIBUTION debug view (combo/env only - see
// ViewMode::ContactShadows): the two sun-visibility estimates side by
// side in one glance, channel-coded so their INTERACTION reads without
// toggling:
//   R = the contact raymarch's visibility (contact_shadow.cs.hlsl's
//       field - 1 lit, 0 occluded)
//   G = the CSM/EVSM shadow-map factor (the lit body's exact branch:
//       EVSM moments under EVSM, the PCF depth atlas otherwise)
//   B = -
// so: YELLOW (R+G) = sun-lit, untouched by either; GREEN (G only) = the
// contact march found an occluder the cascade missed - THE EFFECT'S
// CONTRIBUTION (what this view exists to show); RED (R only) = the
// cascade shadows but the march missed it - the thin-occluder/resolution
// tuning signal (try _RES=1 or a bigger _THICKNESS); BLACK = both dark.
// Dim blue marks receivers whose NdotL <= 0 (the sun term is zero
// there - the contact value is inert and would read misleadingly).
// MAGENTA = the visibility texture was not published this frame (the
// pass gated off: the effect disabled, or the resources not yet valid -
// never a silent white).
//
// The contact pass is EXEMPT from its own debug-view gate
// (ContactShadowsActive keeps it running under THIS view - the
// Ssgi-views-keep-their-inputs pattern): the readout samples the REAL
// march output (frame.contactShadowIndex), it does not re-trace.
//
// Pairs with default.vert.hlsl (same PSIn as default_lit - the family
// contract; see AppResources::debugPrograms). Mask cutout + Blend
// dither + the closeup dissolve match the lit path so silhouettes
// agree. Exposure-compensated like every debug view (the 0..1 readout
// travels through HDR + tonemap); NO fog (flat readouts stay
// unfogged by discipline).

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    // ALL inputs carry EXPLICIT locations (DXC rejects partial explicit
    // assignment; see default_unlit.frag.hlsl's note).
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // Match the lit path's closeup dissolve (default_unlit's block): a
    // flagged subtree screen-doors away here too.
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameEarly = pc.frameUniforms.Get();
        FramePSHot hotEarly = frameEarly.frameHot.Get();
        if (!CloseupDitherKeep(IN.worldPos, hotEarly.cameraPos, clipPos.xy))
            discard;
    }

    // Masked cutout + dithered Blend (matches lit/cascade debug): the
    // silhouette must agree or the readout lies at every leaf edge.
    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float baseAlpha = mat.diffuse.a;
    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseAlpha *= baseTex.Sample(samplers[0], IN.uv).a;
    }
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;
    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, clipPos.xy))
        discard;

    FrameUniforms frame = pc.frameUniforms.Get();
    FramePSHot hot = frame.frameHot.Get();
    Light light = pc.bLights.Get().lights[0];

    // No visibility published this frame: LOUD, never a silent white
    // (the bring-up lesson - a no-op readout looks like "all lit").
    if (frame.contactShadowIndex == 0xffffffffu)
    {
        float3 color = float3(1.0f, 0.05f, 1.0f);
        color /= max(frame.exposure, 1e-6f);
        OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
        return OUT;
    }

    // The contact march's field (the lit body's exact tap: screen uv,
    // linear sampler 0 - half-res bilinearly upsamples).
    float2 csUv = clipPos.xy * frame.invViewportSize;
    float contact = textures[NonUniformResourceIndex(frame.contactShadowIndex)]
        .Sample(samplers[0], csUv).r;

    // The shadow-map factor: the lit body's exact branch (EVSM moments
    // vs the PCF depth atlas). No cascades at all -> 1.0 (nothing
    // shadow-mapped; the R channel then reads the contact field alone).
    float csm = 1.0f;
    if (light.shadow_map_index >= 0 && pc.numCascades > 0)
    {
        float3 faceN = normalize(IN.normal);
        if (!IN.isFrontFace)
            faceN = -faceN;
        if (hot.evsmMode != 0u)
            csm = GetShadowFactorEvsmCascadedFromPC(0, IN.worldPos, faceN,
                light, (uint) light.shadow_map_index, EVSM_EXPONENT, clipPos.xy);
        else
            csm = GetShadowFactorCascadedFromPC(0, IN.worldPos, faceN, light);
    }

    float3 L = normalize(-light.direction);
    float3 faceN = normalize(IN.normal);
    if (!IN.isFrontFace)
        faceN = -faceN;
    float NdotL = saturate(dot(faceN, L));

    float3 color;
    if (NdotL <= 0.0f)
        // Sun term zero: the contact value is inert here - a dim flat
        // blue so those surfaces never read as "lit/occluded" data.
        color = float3(0.05f, 0.05f, 0.35f);
    else
        color = float3(saturate(contact), saturate(csm), 0.0f);

    // Exposure compensation (readout -> HDR): pre-scale by 1/exposure so
    // the 0..1 false color survives the hdr * exposure -> ACES path.
    color /= max(frame.exposure, 1e-6f);

    OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
