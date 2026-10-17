// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Chrome-ball visualization of the loaded cubemap probes (see
// Rendergraph/probeball.h): a DEDICATED pass whose fragment paints THE
// PROBE'S OWN baked cubemap onto the sphere surface — reflect(-V, N) into
// the bindless TextureCube at LOD 0 (the sharp, non-prefiltered base mip
// of the baked chain: roughness 0 by the cook's roughness = mip/(mips-1)
// convention), scaled by the cubemap panel intensity. No lit-path
// lighting, no fog: the ball reads as the raw capture it visualizes —
// EXCEPT the analytic sun's image: the captures exclude the sun disc by
// design (the lit path's analytic direct owns the sun ON surfaces) and
// this overlay has no lit-path direct term, so without a compensation a
// chrome ball's mirror of the open sky showed a sunless band while the
// direct sky bloomed the disc ("the bloom doesn't match the open sky").
// The FS adds the disc at the reflect direction — the sun's IMAGE in the
// mirror, the exact disc+rim math of EvaluateSkyColor (environment.hlsli)
// read from the frame env through the FrameUniforms BDA, so the ball's
// sky matches the runtime sky by construction.
#define PROBEBALL_PASS
#include "common.hlsli"

// Mirrors smvk::probeball::ProbeBallPush (C++, probeball.h) exactly.
struct ProbeBallPush
{
    float4x4 viewProj; // 0  the rendering view's VP (reversed-Z, Y-flipped)
    float4 eye;         // 64 xyz camera, w unused
    float4 xform;       // 80 xyz ball center, w radius
    float4 probe;       // 96 x cubemap index (float, cast), y mips,
                        //    z intensity, w sampler slot
    vk::BufferPointer<FrameUniforms> frame; // 112 the env sun fields
};

[[vk::push_constant]]
ConstantBuffer<ProbeBallPush> pb;

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
};

float4 mainPS(VSOut IN) : SV_Target0
{
    float3 N = normalize(IN.normal);
    float3 V = normalize(pb.eye.xyz - IN.worldPos);
    float3 R = reflect(-V, N);

    const uint cubemapIdx = uint(pb.probe.x);
    if (cubemapIdx == 0xffffffffu)
        return float4(1.0f, 0.0f, 1.0f, 1.0f); // magenta: no cubemap bound

    TextureCube cube = cubemaps[NonUniformResourceIndex(cubemapIdx)];
    SamplerState smp = samplers[NonUniformResourceIndex(uint(pb.probe.w))];
    // LOD 0: the sharp base capture of the GGX chain (roughness 0).
    const float intensity = max(pb.probe.z, 0.0f);
    float3 rgb = cube.SampleLevel(smp, R, 0.0).rgb * intensity;

    // The analytic sun's image in the mirror (the runtime sky's disc):
    // the exact EvaluateSkyColor disc + rim math over the reflect
    // direction, from the same frame env the sky pass reads — the ball's
    // sky then blooms exactly like the direct sky.
    const EnvironmentFrameData env = pb.frame.Get().env;
    if (env.sunDirection.w > -0.5f) {
        const float cosA = dot(R, normalize(env.sunDirection.xyz));
        if (cosA > env.sunDirection.w) {
            const float3 sunHdr = env.sunColor.rgb
                * max(env.sunColor.w, 0.0f);
            const float rim = smoothstep(env.sunDirection.w,
                min(env.sunDirection.w + 0.0008f, 1.0f), cosA);
            rgb = lerp(rgb, sunHdr, rim) * intensity;
        }
    }
    return float4(rgb, 1.0f);
}
