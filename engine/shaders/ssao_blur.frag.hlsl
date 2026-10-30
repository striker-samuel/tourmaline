// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSAO_PASS
#include "common.hlsli"
#include "ao.hlsli"
#include "gpu_platform.hlsli"

// SSGI AO blur: 5x5 bilateral of the half-res raw AO over the SELF-CARRIED
// (ao, repDist, repNormal) triple — ZERO depth loads (the old pass did 29
// full-res depth .Loads per half-res pixel re-deriving what the raw pass
// already knew). Edge weights decode the .g channel; the representative
// normal rides the same weights (decode-oct -> weighted vector sum ->
// normalize -> re-encode: never average ENCODED pairs across the
// octahedral fold — the fold is discontinuous at the hemisphere seam).

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // R16G16B16A16_UNorm: (ao, encRepDist, octN.xy)
};

// Must match Engine/Rendergraph/ssgi.h SsgiAoBlurPush (32 bytes).
// All-scalar layout on purpose (see SsrBlurPush's note: float2 members
// would straddle 8-byte alignment and spirv-val rejects the module).
struct SsgiAoBlurPush
{
    uint aoTextureIndex; // raw half-res (ao, encDist, octN)
    uint samplerIndex; // linear clamp (taps)
    uint pad0;
    uint pad1;
    float sharpness; // depth edge weight (higher = tighter)
    float maxDistance; // rep-dist decode (matches the raw pass)
    float texelX; // 1 / half-res width
    float texelY; // 1 / half-res height
};

GPU_PUSH(SsgiAoBlurPush, bp);

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D aoTex = textures[NonUniformResourceIndex(bp.aoTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];

    // Center texel (exact .Load; the filtered 5x5 taps gather around it).
    // Raw's strict guards wrote the RESERVED sentinel code here for
    // any-sky / silhouette-edge / too-far blocks — passthrough, same
    // discipline as the old early-out (a guarded block can never receive
    // or spread AO).
    float4 selfTap = aoTex.Load(int3(int2(IN.pos.xy), 0));
    if (selfTap.g >= 1.0f)
    {
        OUT.outColor = kAoSkyBlock;
        return OUT;
    }
    float centerDist = AoDecodeRepDist(selfTap.g, bp.maxDistance);
    float3 centerN = AoOctDecode(selfTap.zw);

    // 5x5 gaussian-ish blur, edge-rejected across the carried depths.
    float aoAcc = 0.0f;
    float distAcc = 0.0f;
    float3 nAcc = float3(0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float2 tapUv = IN.uv + float2(float(x) * bp.texelX,
                                          float(y) * bp.texelY);
            float4 tap = aoTex.Sample(smp, tapUv);
            float tapDist = AoDecodeRepDist(tap.g, bp.maxDistance);

            float wx = (x == 0) ? 6.0f : ((abs(x) == 1) ? 4.0f : 1.0f);
            float wy = (y == 0) ? 6.0f : ((abs(y) == 1) ? 4.0f : 1.0f);
            float w = wx * wy * AoEdgeWeight(tapDist, centerDist, bp.sharpness);
            aoAcc += tap.r * w;
            distAcc += tapDist * w;
            nAcc += AoOctDecode(tap.zw) * w;
            wSum += w;
        }
    }
    if (wSum <= 1e-5f)
    {
        OUT.outColor = kAoSkyBlock;
        return OUT;
    }
    float ao = aoAcc / wSum;
    // Representative distance: weighted scalar average of the ACCEPTED
    // samples — sentinel taps carried the kAoSkyDist sentinel and ~0
    // weight (exp underflow), so the average stays <= maxDistance and the
    // re-encode below can never reach the reserved code from them.
    float repDist = distAcc / wSum;
    // Representative normal: weighted vector average on the sphere
    // (fold-safe); a fully-cancelled sum (opposing normals across an
    // edge, all-tap rejection) keeps the center's own orientation.
    float nLen = length(nAcc);
    float3 repN = (nLen > 1e-4f) ? nAcc / nLen : centerN;
    OUT.outColor = float4(
        ao,
        AoEncodeRepDist(repDist, bp.maxDistance),
        AoOctEncode(repN));
    return OUT;
}
