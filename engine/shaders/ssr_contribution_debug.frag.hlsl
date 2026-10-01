// SSR contribution debug view (F12): shows the SSR contribution isolated
// (SSR layer only, without the base HDR). The SSR thin G-buffer (oct normal +
// roughness + metal) is written by the Forward+ depth prepass; the trace
// uses real normals (no wireframe artifact) and continuous roughness.
// Pairs with default.vert.hlsl (same VS as the lit path): the VS outputs
// world-space normal and uv; the FS reads the G-buffer (oct normal + roughness
// + metal) and traces the SSR. Output is the SSR contribution only (rgb),
// with a=1 on hit, a=0 on miss. Exposure compensation is applied like the
// other debug views so the SSR contribution survives tonemap intact.
//
// Exposure-compensated like the other debug views (default_unlit et al): the
// SSR contribution travels through HDR + tonemap (hdr * exposure -> ACES) with
// a physical-units exposure (~0.001) that would crush a raw 0..1 color to
// black, so the output is pre-scaled by 1/exposure.

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "environment.hlsli"
#include "hzb.hlsli"
#include "ssr_compute.hlsli"

// Must match Engine/Rendergraph/ssr.h SsrContributionDebugPush (64 bytes).
struct SsrContributionDebugPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint gbufferTextureIndex;
    uint ssrHzbTextureIndex;
    uint halfW;
    uint halfH;
    float invDepthW;
    float invDepthH;
    float maxDistance;
    float thickness;
    float intensity;
    float f0;
    uint frameIndex;
    uint mostDetailedMip;
    uint maxIntersections;
    uint ssrHzbMipCount;
};

[[vk::push_constant]]
ConstantBuffer<SsrContributionDebugPush> sp;

struct PSIn
{
    float4 clipPos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    float2 lightmapUv : TEXCOORD3;
    nointerpolation uint lmChunk : TEXCOORD4;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    if (sp.gbufferTextureIndex == 0xffffffffu)
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }

    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D gbTex = textures[NonUniformResourceIndex(sp.gbufferTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    Texture2D ssrHzbTex = textures[NonUniformResourceIndex(sp.ssrHzbTextureIndex)];

    float2 invDepth = float2(sp.invDepthW, sp.invDepthH);

    int2 halfPx = int2(IN.clipPos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;

    float dCenter = 0.0f;
    int2 fullPxC = fullBase;
    float dBest = 0.0f;
    if (!DofIsBackgroundOrGarbage(d00) && d00 > dBest) { dBest = d00; fullPxC = fullBase; }
    if (!DofIsBackgroundOrGarbage(d10) && d10 > dBest) { dBest = d10; fullPxC = fullBase + int2(1, 0); }
    if (!DofIsBackgroundOrGarbage(d01) && d01 > dBest) { dBest = d01; fullPxC = fullBase + int2(0, 1); }
    if (!DofIsBackgroundOrGarbage(d11) && d11 > dBest) { dBest = d11; fullPxC = fullBase + int2(1, 1); }
    if (DofIsBackgroundOrGarbage(dBest))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }

    float2 ndcC = SsrFullNdc(fullPxC, invDepth);
    float3 viewPos = SsrUnprojectView(ndcC, dBest, frame.invProj);

    float4 gb = gbTex.Load(int3(fullPxC, 0));
    float3 N = SsrOctDecode(gb.xy);
    float roughness = gb.z;
    float metal = (gb.w >= 0.5f) ? 1.0f : 0.0f;

    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;

    float3 F0 = lerp(sp.f0, 1.0f, metal);
    float ndv = saturate(dot(N, V));
    float fres = F0 + (1.0f - F0) * pow(1.0f - ndv, 5.0f);

    float3 R = reflect(-V, N);

    float cone = roughness * roughness;
    if (cone > 0.0f)
    {
        float2 j = float2(
            SsrIGN(float2(halfPx.x + 11.7f, halfPx.y + 5.3f), sp.frameIndex),
            SsrIGN(float2(halfPx.x + 7.9f, halfPx.y + 19.1f), sp.frameIndex)
        ) * 2.0f - 1.0f;
        float3 t = normalize(cross(abs(R.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f), R));
        float3 b = cross(R, t);
        R = normalize(R + (t * j.x + b * j.y) * cone * 1.5f);
    }

    float gloss = 1.0f - roughness;
    float glossFade = gloss * gloss;

    // Simplified ray march for debug view - trace the reflection
    float2 halfExtent = float2(sp.halfW, sp.halfH);
    float2 fullExtent = float2(1.0f / max(sp.invDepthW, 1e-9f),
                               1.0f / max(sp.invDepthH, 1e-9f));
    float2 uv = (float2(halfPx) + 0.5f) / max(halfExtent, float2(1.0f, 1.0f));
    float3 screenOrigin = float3(uv, dBest);
    float3 screenDir = SsrProjectDirection(viewPos, R, screenOrigin, frame.proj);

    if (length(screenDir.xy) <= 1e-9f)
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }

    // Simplified: just sample HDR along reflection vector (no Hi-Z for debug)
    float3 reflected = float3(0.0f, 0.0f, 0.0f);
    float confidence = 0.0f;

    // Simple ray march (abbreviated for debug view)
    for (int i = 0; i < 64; ++i)
    {
        float t = float(i) * sp.maxDistance / 64.0f;
        float3 samplePos = viewPos + R * t;
        float4 proj = mul(frame.proj, float4(samplePos, 1.0f));
        float2 sampleUv = proj.xy / proj.w * 0.5f + 0.5f;

        if (sampleUv.x < 0.0f || sampleUv.x > 1.0f || sampleUv.y < 0.0f || sampleUv.y > 1.0f)
            break;

        float sampleDepth = depthTex.Sample(smp, sampleUv).r;
        float3 sampleViewPos = SsrUnprojectView(sampleUv * 2.0f - 1.0f, sampleDepth, frame.invProj);

        if (length(sampleViewPos - samplePos) < sp.thickness)
        {
            reflected = hdrTex.Sample(smp, sampleUv).rgb;
            confidence = 1.0f;
            break;
        }
    }

    if (confidence > 0.0f)
    {
        reflected = min(max(reflected, float3(0.0f, 0.0f, 0.0f)), float3(65504.0f, 65504.0f, 65504.0f));
        float gloss = 1.0f - roughness;
        reflected = min(reflected * (fres * (gloss * gloss) * sp.intensity),
            float3(8192.0f, 8192.0f, 8192.0f));
    }

    OUT.outColor = float4(reflected, confidence);
    return OUT;
}