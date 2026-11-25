// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"
#include "double_float.hlsli"

// Motion vector VS: outputs the current clip position (SV_Position) and the
// previous-frame clip position as a separate interpolated attribute.
// The FS computes the UV delta from both; computing it per-vertex would be
// screen-space linear interpolation and wrong under perspective (the
// rasterizer perspective-corrects each attribute independently).
// prevViewProj comes from FrameUniforms, prevModel from push constants.

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 prevClip : PREVCLIP;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();

    // Current clip via the SAME double-float camera-relative transform the
    // GPU-scene prepass used, so the motion leg's depth matches bit-for-bit
    // (the bare GreaterEqual test otherwise drops ~half the fragments when
    // the prepass rasterised meshlets + double-float while this leg used
    // float32 absolute globalTransform). The model slot carries basis X/Y/Z
    // + worldPosition high/low (see RecordMotion).
    float3x3 basis = float3x3(pc.model[0].xyz, pc.model[1].xyz, pc.model[2].xyz);
    DoubleFloat3 worldPos;
    worldPos.high = float4(pc.model[0].w, pc.model[1].w, pc.model[2].w, 0.0f);
    worldPos.low = pc.model[3];
    DoubleFloat3 origin;
    origin.high = frame.cameraOriginHigh;
    origin.low = frame.cameraOriginLow;
    const float3 relative = DoubleFloatTransformPoint(IN.position, basis, worldPos, origin);
    OUT.pos = mul(frame.relativeViewProj, float4(relative, 1.0f));

    // prevModel is 3 float4 ROWS of the affine transform (row-major); apply
    // it as row dot-products instead of a matrix mul (a float4[3] carries no
    // matrix-stride decoration, which keeps the push constant spirv-val-clean).
    float4 localPos = float4(IN.position, 1.0);
    float4 prevWorld = float4(
        dot(pc.prevModel[0], localPos),
        dot(pc.prevModel[1], localPos),
        dot(pc.prevModel[2], localPos),
        1.0);
    OUT.prevClip = mul(frame.prevViewProj, prevWorld);

    // uv + color pass through for the FS coverage keep-test (MASK cutout /
    // dithered Blend): vertex color.a is part of coverage alpha (seam
    // ribbons) and layered materials use the RGBA as layer weights.
    OUT.uv = IN.uv;
    OUT.color = IN.color;
    return OUT;
}
