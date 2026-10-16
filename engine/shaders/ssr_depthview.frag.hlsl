// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"

// Ground-truth depth view (TOURMALINE_SSR_DUMP): samples the depth image
// EXACTLY the way the SSR trace does — same bindless textures[] slot,
// same .Load — and writes it to a dumpable target. This bypasses the
// copy-based readback entirely: if this output is clean [0,1] reverse-Z,
// the depth-as-seen-by-SSR is fine and any dotted corruption lives in the
// readback path, not the renderer.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// All scalars (no float2 members — see the SsrBlurPush layout bug).
// Must stay 16 bytes.
struct SsrDepthViewPush
{
    uint depthTextureIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::push_constant]]
ConstantBuffer<SsrDepthViewPush> dp;

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    // The trace's receiver tap: half-res pixel -> full-res 2x2 block's
    // origin — the exact coordinates ssr.frag.hlsl loads first.
    int2 halfPx = int2(pos.xy);
    int2 fullBase = halfPx * 2;
    float d = textures[NonUniformResourceIndex(dp.depthTextureIndex)]
        .Load(int3(fullBase, 0)).r;
    OUT.outColor = float4(d, d, d, 1.0);
    return OUT;
}
