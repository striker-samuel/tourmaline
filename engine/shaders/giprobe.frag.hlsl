// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GI probe visualization (see Rendergraph/giprobe.h): the fragment
// shader is a PURE PASS-THROUGH — the vertex shader already evaluated
// the probe's baked directional GI (its exact texel's 9 SG lobes against
// the sphere direction); the FS just writes the interpolated color into
// HDR. No scene lighting, no texture sampling, no GI evaluation here by
// design (the whole point: the sphere's surface reads as the baked
// field's directional answer).
#define GIPROBE_PASS
#include "common.hlsli"
// (GIPROBE_PASS gates common.hlsli's scene-pc block — the VS's push is
// the module's own GiProbePush; the FS reads no push at all.)

struct PSIn
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    // Linear HDR straight into the tone chain (bloom/tonemap grade the
    // probe balls exactly like scene radiance — the debug visual reads
    // as final image, the probeball FS convention).
    OUT.outColor = float4(IN.color, 1.0);
    return OUT;
}
