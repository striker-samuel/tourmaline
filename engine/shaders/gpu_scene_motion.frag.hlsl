// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"

// GPU-scene batched motion-vector FS: per-pixel UV delta (prevUV - currUV)
// into an RG16F target. The per-instance material id arrives as a flat
// varying from gpu_scene_motion.vert.hlsl (location 2), so the MASK
// alpha-tested cutout reuses the same alpha test as motionvector.frag.hlsl
// but without a per-draw push-constant material id.

struct PSIn {
    float4 pos : SV_Position;
    float4 prevClip : PREVCLIP;
    float2 uv : TEXCOORD0;
    nointerpolation uint materialId : TEXCOORD1;
};

struct PSOut {
    float2 motion : SV_Target0;
    // NOTE: no SV_Depth output — the motion pass tests read-only against
    // prepass depth (writeEnable=false), so a DepthReplacing flag would
    // buy nothing and risks the post-shader depth path on some drivers.
    // Drivers already early-test demote-discard.
};

PSOut mainPS(PSIn IN) {
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[IN.materialId];
    if (mat.params.w == 2.0f) // ShadingModel::Masked
    {
        float alphaCutoff = mat.params.x;
        float baseAlpha = mat.diffuse.a;
        int baseColorIdx = mat.tex.x;
        if (baseColorIdx >= 0) {
            uint slot = (uint)baseColorIdx;
            Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
            float4 s = baseTex.Sample(samplers[0], IN.uv);
            baseAlpha *= s.a;
        }
        if (baseAlpha < alphaCutoff)
            discard;
    }
    // Dithered Blend: same screen-door pattern as the main view so motion
    // vectors exist exactly for the pixels the color pass kept.
    if (mat.params2.w == 2.0f && pc.pad_pc == 0) {
        float baseAlpha = mat.diffuse.a;
        int baseColorIdx = mat.tex.x;
        if (baseColorIdx >= 0) {
            uint slot = (uint)baseColorIdx;
            Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
            float4 s = baseTex.Sample(samplers[0], IN.uv);
            baseAlpha *= s.a;
        }
        if (!DitherKeep(baseAlpha, IN.pos.xy))
            discard;
    }

    FrameUniforms frame = pc.frameUniforms.Get();

    // SV_Position is post-divide pixel coords; same NDC->UV convention as
    // the Y-flipped proj both frames share.
    const float2 currUV = IN.pos.xy * frame.invViewportSize;
    float2 motion = float2(0.0f, 0.0f);
    // A previous clip position is a VALID reprojection only when its W is
    // positive and finite (in front of the previous camera). Behind-camera or
    // non-finite history reads as "no history" (zero motion) — never a huge
    // spurious delta, and never a NaN/Inf reaching a temporal consumer. This
    // replaces the old magnitude heuristic, which both discarded legitimate
    // large motion and failed to reject invalid W before the divide.
    if (isfinite(IN.prevClip.w) && IN.prevClip.w > 0.0f) {
        const float2 prevUV = (IN.prevClip.xy / IN.prevClip.w) * 0.5f + 0.5f;
        const float2 m = prevUV - currUV;
        if (isfinite(m.x) && isfinite(m.y))
            motion = m;
    }

    OUT.motion = motion;
    return OUT;
}
