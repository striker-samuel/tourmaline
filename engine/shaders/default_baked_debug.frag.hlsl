// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Baked-irradiance debug view (editor F8): lobe-0 amplitude of the SG
// lightmap atlas at the interpolated atlas uv. Lobe 0 exists in every
// chunk (5/6/9/12 lobes all carry it) and holds the bulk of the baked
// signal, so no per-draw chunk id is needed — which matters because the
// direct-draw path (default.vert) never resolves one (lmChunk stays
// 0xFFFFFFFF there; the CPU skips unbaked draws instead, so everything
// rasterized here is baked). Alpha cutout + dithered Blend discard match
// the lit path so silhouettes agree. Output is Reinhard + gamma mapped
// for the LDR editor swapchain (the App path tonemaps instead).
#include "common.hlsli"

struct PSIn
{
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    float2 lightmapUv : TEXCOORD3;
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

    // Alpha cutout so holes match the lit view (color itself is debug).
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

    // Lobe-0 atlas amplitude (layer 0 rgb; .a is lobe-1 R, ignored) - Lightmaps deprecated.
#if 0
    FrameUniforms frame = pc.frameUniforms.Get();
    float3 irr = textures[NonUniformResourceIndex(frame.lmLayers[0])]
        .Sample(samplers[0], IN.lightmapUv).rgb;

    // Baked values are HDR (lux-scale bounce); compress for LDR display.
    float3 c = irr / (1.0f + max(irr, 0.0f));
    c = pow(max(c, 0.0f), 1.0f / 2.2f);

    OUT.outColor = float4(SanitizeHdr(c), 1.0f);
#else
    OUT.outColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
#endif
    return OUT;
}
