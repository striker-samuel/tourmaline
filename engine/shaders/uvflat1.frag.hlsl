// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Flat UV (lightmap) fragment: sample lobe 0 of the baked lightmap
// atlas at the unwrapped atlas uv — the "chart layout over baked
// content" view. Same Reinhard + gamma display mapping as the F8 baked
// debug (the editor renders LDR direct). LIGHTMAPS DEPRECATED.
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

#if 0 // Lightmaps deprecated
    FrameUniforms frame = pc.frameUniforms.Get();
    float3 irr = textures[NonUniformResourceIndex(frame.lmLayers[0])]
        .Sample(samplers[0], IN.lightmapUv).rgb;

    // Baked values are HDR; compress for LDR display (F8 parity).
    float3 c = irr / (1.0f + max(irr, 0.0f));
    c = pow(max(c, 0.0f), 1.0f / 2.2f);

    OUT.outColor = float4(SanitizeHdr(c), 1.0);
#else
    OUT.outColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
#endif
    return OUT;
}
