// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "ssr_compute.hlsli"
#include "gpu_platform.hlsli"

// SSR copy: blits the compute-chain radiance buffer (float4 per
// half-res pixel: rgb = reflected HDR x Fresnel x fade x intensity,
// a = hit confidence) into the half-res raw reflection target the
// bilateral blur + temporal + composite consume. Trivial FS (one BDA
// load + one store); the blur's bilinear reads stay on the image.

// Must match Engine/Rendergraph/ssr.h SsrCopyPush (32 bytes).
struct SsrCopyPush
{
    GPU_PTR_READ(SsrRadiance) radiance;
    uint halfW;
    uint halfH;
    uint pad0;
    uint pad1;
};

GPU_PUSH(SsrCopyPush, sp);

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F reflection layer
};

PSOut mainPS(float4 pos : SV_Position)
{
    PSOut OUT;
    int2 halfPx = int2(clamp(pos.xy, float2(0.0f, 0.0f),
        float2(sp.halfW, sp.halfH) - 1.0f));
    OUT.outColor = sp.radiance.Get().v[halfPx.y * int(sp.halfW) + halfPx.x];
    return OUT;
}
