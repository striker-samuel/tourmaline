#define GTAO_PASS
#include "common.hlsli"
#include "ao.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // R8: only .r survives
};

// Must match Engine/Rendergraph/gtao.h GtaoPush (56 bytes).
struct GtaoPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint depthTextureIndex;
    uint sliceCount; // 1..8 slice directions
    uint stepsPerSlice; // 1..8 taps per side march
    uint padA;
    float radius; // view-space slice length (m)
    float bias; // horizon bias (view units)
    float thickness; // thin-occluder falloff (m); 0 = off
    float maxDistance; // AO fade-out distance (m)
    float invDepthW; // 1 / full-res depth width
    float invDepthH; // 1 / full-res depth height
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<GtaoPush> gp;

// Unproject NDC -> view space (exact, flip/reversed-Z agnostic).
float3 GtaoUnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

// Interleaved gradient noise: per-pixel slice rotation, no texture.
float GtaoIGN(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = gp.frameUniforms.Get();
    Texture2D depthTex = textures[NonUniformResourceIndex(gp.depthTextureIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    // Half-res pixel -> full-res 2x2 block. Depth .Load needs no sampler
    // (D32 has no linear-filtering support on most drivers).
    int2 halfPx = int2(IN.pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;
    // Strict block classification, same invariant as the SSAO raw pass: a
    // block is a valid AO receiver only when ALL four taps are geometry.
    // Any background tap (sky, clear, or an out-of-bounds Load returning 0
    // at odd-extent borders) marks the whole block unoccluded.
    if (AoIsBackground(d00) || AoIsBackground(d10) ||
        AoIsBackground(d01) || AoIsBackground(d11))
    {
        OUT.outColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return OUT;
    }
    float dCenter = (d00 + d10 + d01 + d11) * 0.25f;

    float centerDist = AoViewDist(dCenter, nearPlane);
    if (centerDist > gp.maxDistance)
    {
        OUT.outColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return OUT;
    }

    // Pixel-center NDC (framebuffer y-down, matches forwardplus.cs.hlsl).
    float2 fullPx = float2(fullBase) + 0.5f;
    float2 ndcXY = float2(fullPx.x * gp.invDepthW * 2.0f - 1.0f,
                          fullPx.y * gp.invDepthH * 2.0f - 1.0f);
    float3 center = GtaoUnprojectView(ndcXY, dCenter, frame.invProj);
    float centerZ = -center.z; // positive view distance

    // Depth-derived normal from full-res neighbors (one half-res step over).
    // Same silhouette guard as SSAO: a background neighbor means this block
    // sits on an edge — stay unoccluded instead of fabricating a normal.
    float dPx = depthTex.Load(int3(fullBase + int2(2, 0), 0)).r;
    float dPy = depthTex.Load(int3(fullBase + int2(0, 2), 0)).r;
    if (AoIsBackground(dPx) || AoIsBackground(dPy))
    {
        OUT.outColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return OUT;
    }
    float2 ndcPx = ndcXY + float2(2.0f * gp.invDepthW * 2.0f, 0.0f);
    float2 ndcPy = ndcXY + float2(0.0f, 2.0f * gp.invDepthH * 2.0f);
    float3 posX = GtaoUnprojectView(ndcPx, dPx, frame.invProj);
    float3 posY = GtaoUnprojectView(ndcPy, dPy, frame.invProj);
    float3 N = normalize(cross(posX - center, posY - center) + 1e-9f);
    float3 V = -center / max(centerDist, 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;

    // View-space TBN for slice rotation (IGN-rotated so slices don't band).
    float angle0 = GtaoIGN(float2(halfPx)) * 6.2831853f;
    float3 randV = float3(cos(angle0), sin(angle0), 0.35f);
    float3 tangent = normalize(randV - N * dot(randV, N));
    float3 bitangent = cross(N, tangent);

    uint slices = clamp(gp.sliceCount, 1, 8);
    uint steps = clamp(gp.stepsPerSlice, 1, 8);
    float radius = max(gp.radius, 1e-4f);

    // Screen-space march radius: view radius -> pixels at the center depth.
    // Project a view-space offset of `radius` to NDC with the projection
    // matrix scale factors (proj[0][0], proj[1][1] are focal lengths).
    float2 focal = float2(abs(frame.proj[0][0]), abs(frame.proj[1][1]));
    focal = max(focal, float2(1e-4f, 1e-4f));
    float2 fullDepth = float2(1.0f / gp.invDepthW, 1.0f / gp.invDepthH);
    float2 radiusPx = radius * focal * fullDepth / max(centerZ, 1e-3f);
    // Clamp the march so far-field pixels don't gather across the screen.
    radiusPx = min(radiusPx, fullDepth * 0.25f);

    float visSum = 0.0f;
    for (uint s = 0; s < 8; ++s)
    {
        if (s >= slices)
            break;
        // Slice direction: evenly spaced over PI (opposite dirs share one
        // slice), rotated per-pixel by the IGN angle.
        float phi = angle0 + float(s) * 3.14159265f / float(slices);
        float2 sliceUvDir = float2(cos(phi), sin(phi));
        // View-space slice axis: TBN-projected screen dir, re-orthogonalized
        // against N so the slice plane always contains the normal.
        float3 sliceDir = normalize(
            tangent * sliceUvDir.x + bitangent * sliceUvDir.y + 1e-9f);
        sliceDir = normalize(sliceDir - N * dot(sliceDir, N) + 1e-9f);

        // Horizon elevation (sin) per side; init at -1 = fully open sky.
        float sinHFront = -1.0f;
        float sinHBack = -1.0f;
        for (uint i = 1; i <= 8; ++i)
        {
            if (i > steps)
                break;
            // Quadratic spacing: denser near the center.
            float t = float(i) / float(steps);
            float r = radius * t * t;
            float2 offPxF = sliceUvDir * (r / max(radius, 1e-6f)) * radiusPx;
            for (uint side = 0; side < 2; ++side)
            {
                float2 tapFullF = fullPx + (side == 0 ? offPxF : -offPxF);
                if (tapFullF.x < 0.0f || tapFullF.y < 0.0f ||
                    tapFullF.x > fullDepth.x - 1.0f || tapFullF.y > fullDepth.y - 1.0f)
                    continue;
                int2 tapPx = int2(tapFullF);
                float tapD = depthTex.Load(int3(tapPx, 0)).r;
                if (AoIsBackground(tapD))
                    continue; // sky behind the slice: no occluder
                float tapDist = AoViewDist(tapD, nearPlane);
                float2 tapNdc = float2(
                    (float(tapPx.x) + 0.5f) * gp.invDepthW * 2.0f - 1.0f,
                    (float(tapPx.y) + 0.5f) * gp.invDepthH * 2.0f - 1.0f);
                float3 tapPos = GtaoUnprojectView(tapNdc, tapD, frame.invProj);
                float3 dv = tapPos - center;
                float dvLen = max(length(dv), 1e-6f);
                // Thin-occluder falloff: taps far behind the surface plane
                // fade out instead of fully occluding (XeGTAO thickness
                // heuristic). Disabled when thickness <= 0.
                float falloff = 1.0f;
                if (gp.thickness > 1e-6f)
                {
                    float behind = max(tapDist - centerZ - gp.bias, 0.0f);
                    falloff = saturate(1.0f - behind / max(gp.thickness, 1e-6f));
                    // Range check: taps outside the radius shell fade out.
                    falloff *= saturate(radius / max(dvLen, 1e-6f));
                }
                else
                {
                    float rangeCheck = smoothstep(0.0f, 1.0f,
                        radius / max(abs(centerZ - tapDist), 1e-6f));
                    falloff = rangeCheck;
                }
                if (falloff <= 1e-4f)
                    continue;
                // Horizon elevation of this tap within the slice plane:
                // height above the tangent plane over the along-slice run.
                float run = dot(dv, (side == 0 ? sliceDir : -sliceDir));
                float rise = dot(dv, N) - gp.bias;
                // Taps behind the slice origin vote only through falloff;
                // require a forward run so back-facing geometry can't invert
                // the horizon.
                if (run <= 1e-6f)
                    continue;
                float sinH = rise / max(dvLen, 1e-6f);
                // Weight near taps higher: scale the horizon vote by falloff
                // toward the open-sky default (-1 = no occlusion).
                sinH = lerp(-1.0f, sinH, falloff);
                if (side == 0)
                    sinHFront = max(sinHFront, sinH);
                else
                    sinHBack = max(sinHBack, sinH);
            }
        }
        // Integrate the visibility arc between the two horizons: each side
        // contributes (sin(h) + 1) / 2 (0 = hemisphere blocked, 1 = open),
        // the slice visibility is the mean of both sides.
        float visFront = saturate((sinHFront + 1.0f) * 0.5f);
        float visBack = saturate((sinHBack + 1.0f) * 0.5f);
        visSum += (visFront + visBack) * 0.5f;
    }

    float ao = visSum / float(max(slices, 1));
    // Distance fade: full strength nearby, none past maxDistance.
    float fade = 1.0f - smoothstep(gp.maxDistance * 0.5f, gp.maxDistance, centerDist);
    ao = lerp(1.0f, ao, fade);
    OUT.outColor = float4(ao, ao, ao, 1.0f);
    return OUT;
}
