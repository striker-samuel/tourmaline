// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"

// SSR temporal depth history: a 1:1 R32F mirror of the final
// renderDepth, copied once per TEMPORAL record (emitted by
// AddSsrTemporalPasses after the composite — this record's temporal
// pass read the PREVIOUS copy). The NEXT record's disocclusion test
// samples it at the reprojected coordinate: that test needs the depth
// of the surface the HISTORY reflected — last frame's depth at a
// last-frame coordinate. The CURRENT buffer at that coordinate is a
// different camera's geometry under motion (see ssr_temporal.frag.hlsl
// disocclusion note). SV_Position -> exact texel Load: 1:1, no
// sampler, no derivatives; the depth value rides .r unchanged
// (reversed-Z, the same domain the temporal test linearizes).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outDepth : SV_Target0; // R32F mirror (r = reversed-Z depth)
};

// Must match Engine/Rendergraph/ssr.h SsrDepthHistoryPush (8 bytes).
struct SsrDepthHistoryPush
{
    uint depthTextureIndex; // full-res final depth (bindless, Load'ed)
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsrDepthHistoryPush> dp;

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    Texture2D depthTex = textures[NonUniformResourceIndex(dp.depthTextureIndex)];
    OUT.outDepth = float4(depthTex.Load(int3(int2(pos.xy), 0)).r,
        0.0f, 0.0f, 1.0f);
    return OUT;
}
