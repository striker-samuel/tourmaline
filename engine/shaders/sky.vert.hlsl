// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SKY_PASS
#include "common.hlsli"

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    // Unprojected far-plane position (clip z = 1) in world space,
    // still homogeneous. Interpolating this is EXACT across the
    // fullscreen triangle (worldH = invViewProj * clip is linear in the
    // clip coords, and pos.w is 1 for all three vertices so the GPU's
    // perspective correction is the identity), whereas interpolating
    // the divided worldPos/camera-relative ray would bake the 1/w
    // perspective divide in before interpolation and drift — enough to
    // visibly shift the tight sun-disc rim. The pixel shader divides
    // once (or folds the divide into the normalize, see sky.frag.hlsl).
    float4 worldH : TEXCOORD1;
};

VSOut mainVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2(
        (id << 1) & 2,
        id & 2
    );
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = uv;
    // Narrow BDA read: only the inverse view-projection out of
    // FrameUniforms (3 vertices per frame, so the 64-byte matrix load
    // is noise). The per-pixel 4x4 mul + divide used to run here in
    // the fragment shader for every covered pixel.
    float2 ndc = uv * 2.0f - 1.0f;
    o.worldH = mul(pc.frameUniforms.Get().invViewProj, float4(ndc, 1.0f, 1.0f));
    return o;
}
