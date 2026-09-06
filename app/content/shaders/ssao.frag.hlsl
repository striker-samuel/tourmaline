#define SSAO_PASS
#include "common.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // R8: only .r survives
};

// Must match Engine/Rendergraph/ssao.h SsaoPush (48 bytes).
struct SsaoPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint depthTextureIndex;
    uint sampleCount; // 1..32 hemisphere taps
    float radius; // view-space sample radius (m)
    float bias; // self-occlusion bias (view units)
    float intensity; // reserved (composite lives in tonemap)
    float maxDistance; // AO fade-out distance (m)
    float invDepthW; // 1 / full-res depth width
    float invDepthH; // 1 / full-res depth height
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<SsaoPush> sp;

// Unproject NDC -> view space (exact, flip/reversed-Z agnostic).
float3 SsaoUnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

// Interleaved gradient noise: per-pixel rotation angle source, no texture.
float SsaoIGN(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

// Radical inverse base 2 for the Hammersley sequence.
float SsaoRadicalInverse(uint i)
{
    uint bits = reversebits(i);
    return float(bits) * 2.3283064365386963e-10;
}

float SsaoLinearDist(float ndcDepth, float nearPlane)
{
    // Infinite reversed-Z: view distance (-z) = near / d.
    return nearPlane / max(ndcDepth, 1e-6f);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    // Half-res pixel -> full-res 2x2 block. Depth .Load needs no sampler
    // (D32 has no linear-filtering support on most drivers).
    int2 halfPx = int2(IN.pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;
    // Out-of-bounds Loads return 0: fall back to valid taps only.
    float dSum = 0.0f;
    float dCount = 0.0f;
    if (d00 > 1e-6f) { dSum += d00; dCount += 1.0f; }
    if (d10 > 1e-6f) { dSum += d10; dCount += 1.0f; }
    if (d01 > 1e-6f) { dSum += d01; dCount += 1.0f; }
    if (d11 > 1e-6f) { dSum += d11; dCount += 1.0f; }
    if (dCount < 0.5f)
    {
        OUT.outColor = float4(1.0f, 1.0f, 1.0f, 1.0f); // sky / clear: unoccluded
        return OUT;
    }
    float dCenter = dSum / dCount;

    float centerDist = SsaoLinearDist(dCenter, nearPlane);
    if (centerDist > sp.maxDistance)
    {
        OUT.outColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return OUT;
    }

    // Pixel-center NDC (framebuffer y-down, matches forwardplus.cs.hlsl).
    float2 fullPx = float2(fullBase) + 0.5f;
    float2 ndcXY = float2(fullPx.x * sp.invDepthW * 2.0f - 1.0f,
                          fullPx.y * sp.invDepthH * 2.0f - 1.0f);
    float3 center = SsaoUnprojectView(ndcXY, dCenter, frame.invProj);
    float centerZ = -center.z; // positive view distance

    // Depth-derived normal from full-res neighbors (one half-res step over).
    float dPx = depthTex.Load(int3(fullBase + int2(2, 0), 0)).r;
    float dPy = depthTex.Load(int3(fullBase + int2(0, 2), 0)).r;
    if (dPx <= 1e-6f) dPx = dCenter;
    if (dPy <= 1e-6f) dPy = dCenter;
    float2 ndcPx = ndcXY + float2(2.0f * sp.invDepthW * 2.0f, 0.0f);
    float2 ndcPy = ndcXY + float2(0.0f, 2.0f * sp.invDepthH * 2.0f);
    float3 posX = SsaoUnprojectView(ndcPx, dPx, frame.invProj);
    float3 posY = SsaoUnprojectView(ndcPy, dPy, frame.invProj);
    float3 N = normalize(cross(posX - center, posY - center) + 1e-9f);
    float3 V = -center / max(centerDist, 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;

    // IGN-rotated TBN so the kernel doesn't band.
    float angle = SsaoIGN(float2(halfPx)) * 6.2831853f;
    float2 rotSc = float2(cos(angle), sin(angle));
    float3 randV = float3(rotSc.x, rotSc.y, 0.35f);
    float3 tangent = normalize(randV - N * dot(randV, N));
    float3 bitangent = cross(N, tangent);

    uint n = clamp(sp.sampleCount, 1, 32);
    float occ = 0.0f;
    float wSum = 0.0f;
    for (uint i = 0; i < 32; ++i)
    {
        if (i >= n)
            break;
        // Hammersley hemisphere, denser near the center.
        float u = (float(i) + 0.5f) / float(n);
        float phi = SsaoRadicalInverse(i) * 6.2831853f + angle;
        float cosT = 1.0f - u;
        float sinT = sqrt(max(1.0f - cosT * cosT, 0.0f));
        float3 hemi = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        float scale = sp.radius * (0.15f + 0.85f * u * u);
        float3 samp = center + (tangent * hemi.x + bitangent * hemi.y + N * hemi.z) * scale;

        // Project to full-res depth UV (NDC y-down -> uv = ndc * 0.5 + 0.5).
        float4 clip = mul(frame.proj, float4(samp, 1.0f));
        float2 sampNdc = clip.xy / max(clip.w, 1e-9f);
        float2 sampUv = sampNdc * 0.5f + 0.5f;
        if (sampUv.x < 0.0f || sampUv.x > 1.0f || sampUv.y < 0.0f || sampUv.y > 1.0f)
            continue;
        int2 sampPx = int2(sampUv * float2(1.0f / sp.invDepthW, 1.0f / sp.invDepthH));
        float sampD = depthTex.Load(int3(sampPx, 0)).r;
        if (sampD <= 1e-6f)
            continue; // sky behind the kernel: no occluder
        float sampDist = SsaoLinearDist(sampD, nearPlane);
        float sampZ = -samp.z;

        // Occluder in front of the kernel point, within the radius shell.
        float rangeCheck = smoothstep(0.0f, 1.0f, sp.radius / max(abs(centerZ - sampDist), 1e-6f));
        float occluded = (sampDist + sp.bias < sampZ) ? 1.0f : 0.0f;
        occ += occluded * rangeCheck;
        wSum += rangeCheck;
    }

    float ao = 1.0f;
    if (wSum > 1e-4f)
        ao = 1.0f - saturate(occ / wSum);
    // Distance fade: full strength nearby, none past maxDistance.
    float fade = 1.0f - smoothstep(sp.maxDistance * 0.5f, sp.maxDistance, centerDist);
    ao = lerp(1.0f, ao, fade);
    OUT.outColor = float4(ao, ao, ao, 1.0f);
    return OUT;
}
