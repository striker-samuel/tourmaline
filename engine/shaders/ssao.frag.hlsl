#define SSAO_PASS
#include "common.hlsli"
#include "ao.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // R16G16B16A16_UNorm: (ao, encRepDist, octN.xy)
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
    // Strict block classification: a block is a valid AO receiver only
    // when ALL four taps are geometry. Any background tap (sky, clear, or
    // an out-of-bounds Load returning 0 at odd-extent borders) marks the
    // whole block unoccluded — averaging "valid taps only" across a
    // foreground/background discontinuity fabricates a center that smears
    // occlusion toward the sky (streaks). The geometry side keeps its AO
    // through the pure-geo neighbor blocks + the consumer's depth-guided
    // upsample (see ao.hlsli).
    // Guarded blocks (any-sky / silhouette-edge / too-far) write the
    // reserved sentinel triple (ao.hlsli kAoSkyBlock): unoccluded, no
    // distance data, neutral normal — every later edge weight in the
    // chain rejects them at silhouettes through the sentinel decode.
    if (AoIsBackground(d00) || AoIsBackground(d10) ||
        AoIsBackground(d01) || AoIsBackground(d11))
    {
        OUT.outColor = kAoSkyBlock;
        return OUT;
    }
    float dCenter = (d00 + d10 + d01 + d11) * 0.25f;

    float centerDist = AoViewDist(dCenter, nearPlane);
    if (centerDist > sp.maxDistance)
    {
        OUT.outColor = kAoSkyBlock;
        return OUT;
    }

    // Pixel-center NDC (framebuffer y-down, matches forwardplus.cs.hlsl).
    float2 fullPx = float2(fullBase) + 0.5f;
    float2 ndcXY = float2(fullPx.x * sp.invDepthW * 2.0f - 1.0f,
                          fullPx.y * sp.invDepthH * 2.0f - 1.0f);
    float3 center = SsaoUnprojectView(ndcXY, dCenter, frame.invProj);
    float centerZ = -center.z; // positive view distance

    // Depth-derived normal from full-res neighbors (one half-res step over).
    // A background neighbor means this block sits on a silhouette: the old
    // dCenter substitution fabricated a planar normal across the edge and
    // the rotated kernel smeared occlusion into the sky (streaks). Stay
    // unoccluded instead — the consumer's depth-guided upsample keeps the
    // pure-geo interior fully shaded.
    float dPx = depthTex.Load(int3(fullBase + int2(2, 0), 0)).r;
    float dPy = depthTex.Load(int3(fullBase + int2(0, 2), 0)).r;
    if (AoIsBackground(dPx) || AoIsBackground(dPy))
    {
        OUT.outColor = kAoSkyBlock;
        return OUT;
    }
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
        if (AoIsBackground(sampD))
            continue; // sky behind the kernel: no occluder
        float sampDist = AoViewDist(sampD, nearPlane);
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
    // SSGI chain export (see ao.hlsli): the block's self-consistent
    // representative triple — ao + the encoded block-average view
    // distance + the octahedral block normal this very pass derived
    // for its horizon test (N above). The blur/compose stages read and
    // re-emit the same representation; nobody re-derives a block's
    // depth or normal from a corner full-res texel again.
    OUT.outColor = float4(
        ao,
        AoEncodeRepDist(centerDist, sp.maxDistance),
        AoOctEncode(N));
    return OUT;
}
