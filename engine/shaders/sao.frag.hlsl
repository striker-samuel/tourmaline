// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SAO_PASS
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

// Must match Engine/Rendergraph/sao.h SaoPush (48 bytes).
struct SaoPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint depthTextureIndex;
    uint sampleCount; // 1..32 spiral taps
    float radius; // view-space spiral extent (m)
    float bias; // surface-offset bias (view units)
    float intensity; // reserved (composite lives in the opaque ambient)
    float maxDistance; // AO fade-out distance (m)
    float invDepthW; // 1 / full-res depth width
    float invDepthH; // 1 / full-res depth height
    uint spiralTurns; // 1..16 extra spiral twist
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SaoPush> sp;

// Unproject NDC -> view space (exact, flip/reversed-Z agnostic).
float3 SaoUnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

// Interleaved gradient noise: per-pixel spiral rotation, no texture.
float SaoIGN(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
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
    // Strict block classification, same invariant as the SSAO/GTAO raw
    // passes: a block is a valid AO receiver only when ALL four taps are
    // geometry. Any background tap (sky, clear, or an out-of-bounds Load
    // returning 0 at odd-extent borders) marks the whole block unoccluded.
    // Guarded blocks (any-sky / silhouette-edge / too-far) write the
    // reserved sentinel triple (ao.hlsli kAoSkyBlock) — see ssao.frag.
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
    float3 center = SaoUnprojectView(ndcXY, dCenter, frame.invProj);

    // Depth-derived normal from full-res neighbors (one half-res step over).
    // Same silhouette guard as SSAO/GTAO: a background neighbor means this
    // block sits on an edge — stay unoccluded instead of fabricating a normal.
    float dPx = depthTex.Load(int3(fullBase + int2(2, 0), 0)).r;
    float dPy = depthTex.Load(int3(fullBase + int2(0, 2), 0)).r;
    if (AoIsBackground(dPx) || AoIsBackground(dPy))
    {
        OUT.outColor = kAoSkyBlock;
        return OUT;
    }
    float2 ndcPx = ndcXY + float2(2.0f * sp.invDepthW * 2.0f, 0.0f);
    float2 ndcPy = ndcXY + float2(0.0f, 2.0f * sp.invDepthH * 2.0f);
    float3 posX = SaoUnprojectView(ndcPx, dPx, frame.invProj);
    float3 posY = SaoUnprojectView(ndcPy, dPy, frame.invProj);
    float3 N = normalize(cross(posX - center, posY - center) + 1e-9f);
    float3 V = -center / max(centerDist, 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;

    uint n = clamp(sp.sampleCount, 1, 32);
    uint turns = clamp(sp.spiralTurns, 1, 16);
    float radius = max(sp.radius, 1e-4f);
    float radius2 = radius * radius;

    // Screen-space march radius: view radius -> pixels at the center depth.
    // Project a view-space offset of `radius` to NDC with the projection
    // matrix scale factors (proj[0][0], proj[1][1] are focal lengths), then
    // NDC -> pixels (ndc spans 2 units over the full width: px = ndc * W/2 —
    // the missing *0.5 here made the spiral 2x too wide, so the world-space
    // radius reject killed the outer half of the taps and SAO read ~dead).
    float2 focal = float2(abs(frame.proj[0][0]), abs(frame.proj[1][1]));
    focal = max(focal, float2(1e-4f, 1e-4f));
    float2 fullDepth = float2(1.0f / sp.invDepthW, 1.0f / sp.invDepthH);
    float2 radiusPx = radius * focal * fullDepth * 0.5f
        / max(centerDist, 1e-3f);
    // Clamp the spiral so far-field pixels don't gather across the screen.
    radiusPx = min(radiusPx, fullDepth * 0.25f);

    // Scalable Ambient Obscurance: golden-angle spiral taps with linearly
    // growing world radius (McGuire et al.). Each tap reconstructs its view
    // position; taps above the surface plane (along +N, past the bias)
    // occlude with a spherical radius-shell falloff, so nearby lids darken
    // corners while distant walls and below-surface taps do not.
    float angle0 = SaoIGN(float2(halfPx)) * 6.2831853f;
    float occ = 0.0f;
    // Denominator semantics mirror SSAO's wSum: normalize by the taps
    // that could VOTE (passed the in-quad / non-sky / radius checks,
    // weighted by their shell falloff) instead of by all n — the bare
    // /n diluted every partially-occluded pixel by the rejected taps
    // and SAO read near-dead next to SSAO.
    float wSum = 0.0f;
    for (uint i = 0; i < 32; ++i)
    {
        if (i >= n)
            break;
        // Linearly growing radius fraction + golden-angle direction with an
        // extra spiral twist (denser near the center, even coverage).
        float t = (float(i) + 0.5f) / float(n);
        float phi = angle0 + float(i) * 2.39996323f
            + float(turns) * t * 6.2831853f;
        float2 spiralDir = float2(cos(phi), sin(phi));
        float2 tapFullF = fullPx + spiralDir * (t * radiusPx);
        if (tapFullF.x < 0.0f || tapFullF.y < 0.0f ||
            tapFullF.x > fullDepth.x - 1.0f || tapFullF.y > fullDepth.y - 1.0f)
            continue;
        int2 tapPx = int2(tapFullF);
        float tapD = depthTex.Load(int3(tapPx, 0)).r;
        if (AoIsBackground(tapD))
            continue; // sky behind the spiral: no occluder, no vote
        float2 tapNdc = float2(
            (float(tapPx.x) + 0.5f) * sp.invDepthW * 2.0f - 1.0f,
            (float(tapPx.y) + 0.5f) * sp.invDepthH * 2.0f - 1.0f);
        float3 tapPos = SaoUnprojectView(tapNdc, tapD, frame.invProj);
        float3 dv = tapPos - center;
        float vv = dot(dv, dv);
        if (vv >= radius2)
            continue;
        float dist = sqrt(max(vv, 1e-12f));
        // Spherical shell falloff: 1 at the center, 0 at the radius edge.
        float falloff = max(1.0f - vv / max(radius2, 1e-12f), 0.0f);
        falloff *= falloff;
        wSum += falloff;
        // Normal-weighted obscurance: how far the tap sits above the
        // surface plane (bias eats self-occlusion), as a cosine so flat
        // walls beside the pixel barely vote.
        float above = dot(dv, N) - sp.bias;
        if (above <= 0.0f)
            continue;
        float cosine = above / max(dist, 1e-6f);
        occ += falloff * cosine;
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
    // for its obscurance test (N above). The blur/compose stages read
    // and re-emit the same representation.
    OUT.outColor = float4(
        ao,
        AoEncodeRepDist(centerDist, sp.maxDistance),
        AoOctEncode(N));
    return OUT;
}
