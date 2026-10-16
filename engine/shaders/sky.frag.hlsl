// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SKY_PASS
#include "common.hlsli"
#include "environment.hlsli"

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 worldH : TEXCOORD1;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    // Narrow BDA reads only — never materializes the 1360+ byte
    // FrameUniforms or the 128-byte EnvironmentFrameData (the old
    // `FrameUniforms frame = pc.frameUniforms.Get()` was the hotspot).
    // Ray from the VS-interpolated homogeneous far-plane position.
    // dir = worldH.xyz/worldH.w - cameraPos, normalized; the /w is
    // folded into the normalize (normalize(x/w - c) == normalize(x - c*w)
    // for w > 0), so this is one MAD + normalize instead of the old
    // 4x4 mul + divide + length/branch per pixel. uv/ndc reconstruction
    // is gone — the VS ran it 3x per frame instead of ~millions.
    float w = max(IN.worldH.w, 1e-6f);
    float3 dir = normalize(IN.worldH.xyz - pc.frameUniforms.Get().cameraPos * w);

    float3 sky = EvaluateSkyColorFromFrame(dir, pc.frameUniforms);

    // Blend the horizon band toward fog so distant geometry fading to fog
    // meets the dome seamlessly. Uses horizon-band weight, not depth.
    float fogMode = pc.frameUniforms.Get().env.fogEndAndFlags.z;
    if (fogMode > 0.5f)
    {
        float4 fogColor = pc.frameUniforms.Get().env.fogColor;
        float3 fogHdr = fogColor.rgb * max(fogColor.w, 0.0f);
        float band = 1.0f - saturate(abs(dir.y) * 4.0f);
        sky = lerp(sky, fogHdr, band * 0.5f);
    }

    OUT.outColor = float4(SanitizeHdr(sky), 1.0f);
    return OUT;
}
