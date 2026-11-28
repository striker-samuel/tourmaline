// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSAO_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

// CORTAO compose: applies the full-res CORTAO occlusion to the scene HDR via
// Main's pre-AO ambient export (aoTerm), the same reconstruction the SSGI
// compose uses (hdr - aoTerm * (1 - aoEff)). No depth/normal joint-bilateral
// here — CORTAO is already full res and temporally smooth after its filter.

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut {
    float4 outColor : SV_Target0;
};

struct CortaoComposePush {
    uint occlusionIndex; // full-res R32 occlusion (bindless sampled)
    uint aoTermIndex;    // Main MRT export (pre-AO ambient, bindless sampled)
    uint hdrIndex;       // full-res HDR (bindless sampled; rewritten as attachment)
    float intensity;     // aoEff = lerp(1, occlusion, intensity)
};
GPU_PUSH(CortaoComposePush, cp);

PSOut mainPS(PSIn IN) {
    PSOut OUT;
    const int2 px = int2(IN.pos.xy);
    const float occlusion =
        textures[NonUniformResourceIndex(cp.occlusionIndex)].Load(int3(px, 0)).r;
    const float aoEff = lerp(1.0f, saturate(occlusion), saturate(cp.intensity));
    const float3 aoTerm =
        textures[NonUniformResourceIndex(cp.aoTermIndex)].Load(int3(px, 0)).rgb;
    const float3 hdr = textures[NonUniformResourceIndex(cp.hdrIndex)].Load(int3(px, 0)).rgb;
    // Sky: aoTerm holds Main's black clear (the lit FS never ran there), so
    // the reconstruction is identity (hdr - 0) regardless of the occlusion.
    OUT.outColor = float4(SanitizeHdr(hdr - aoTerm * (1.0f - aoEff)), 1.0f);
    return OUT;
}
