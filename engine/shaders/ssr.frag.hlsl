#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "environment.hlsli"

// SSR trace (half res): full-res depth -> half-res reflection layer.
// Output: rgb = reflected HDR x Fresnel x intensity (unfaded),
//         a   = edge x distance confidence (1 on sky fallback).
// Rays that hit nothing (sky behind the ray, off-screen exit) fall back
// to the analytic sky specular IBL instead of black — standard SSR+IBL
// pairing, and what makes isolated mirrors (probe balls, open atriums)
// read as chrome instead of going dark. The bilateral blur + composite
// passes consume (rgb, a); the FULL-res base HDR is only re-added in
// the composite, so it stays sharp.

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords (depth .Load is full-res)
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrTracePush (56 bytes).
struct SsrTracePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint steps;
    float2 invDepthSize; // 1 / full-res depth extent (NOT the pass extent)
    float maxDistance;
    float thickness;
    float intensity;
    float f0;
    uint refineSteps;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsrTracePush> sp;

// Interleaved gradient noise: ray-origin dither against banding.
float SsrIGN(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

// Full-res pixel-center NDC (framebuffer y-down: top = -1, matching
// ssao.frag.hlsl and forwardplus.cs.hlsl). Correct for the main view
// because proj carries the [1][1] *= -1 Vulkan flip; invProj unprojects
// it exactly, so this stays flip-agnostic like the SSAO raw pass.
float2 SsrFullNdc(int2 fullPx, float2 invDepthSize)
{
    return float2((float(fullPx.x) + 0.5f) * invDepthSize.x * 2.0f - 1.0f,
                  (float(fullPx.y) + 0.5f) * invDepthSize.y * 2.0f - 1.0f);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];

    float2 invDepth = sp.invDepthSize;

    // Half-res pixel -> full-res 2x2 block (same discipline as ssao.frag).
    // Depth .Load needs no sampler (D32 has no linear filtering).
    int2 halfPx = int2(IN.pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;

    // Receiver = nearest valid tap (reverse-Z: max depth = nearest).
    // Never average across a foreground/background discontinuity: that
    // fabricates a center floating in mid-air and smears reflections
    // across silhouettes. All-background (sky/clear/OOB at odd extents)
    // blocks carry no reflector -> miss.
    float dCenter = 0.0f;
    int2 fullPxC = fullBase;
    float dBest = 0.0f;
    if (!DofIsBackground(d00) && d00 > dBest) { dBest = d00; dCenter = d00; fullPxC = fullBase; }
    if (!DofIsBackground(d10) && d10 > dBest) { dBest = d10; dCenter = d10; fullPxC = fullBase + int2(1, 0); }
    if (!DofIsBackground(d01) && d01 > dBest) { dBest = d01; dCenter = d01; fullPxC = fullBase + int2(0, 1); }
    if (!DofIsBackground(d11) && d11 > dBest) { dBest = d11; dCenter = d11; fullPxC = fullBase + int2(1, 1); }
    if (DofIsBackground(dCenter))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }

    float2 ndcC = SsrFullNdc(fullPxC, invDepth);
    float3 viewPos = DofUnprojectView(ndcC, dCenter, frame.invProj);

    // Depth-derived normal from full-res neighbors one half-step over
    // (same taps as ssao.frag.hlsl). A background neighbor means this
    // block sits on a silhouette: reuse the CENTER depth at the exact
    // neighbor NDC (fronto-parallel assumption). Unprojecting through
    // invProj makes the fallback sign-safe by construction — no
    // hand-rolled view-space offsets that can mirror an axis when the
    // Y-flip changes.
    int2 fullPxX = fullBase + int2(2, 0);
    int2 fullPxY = fullBase + int2(0, 2);
    float dX = depthTex.Load(int3(fullPxX, 0)).r;
    float dY = depthTex.Load(int3(fullPxY, 0)).r;
    float3 posX = DofIsBackground(dX)
        ? DofUnprojectView(SsrFullNdc(fullPxX, invDepth), dCenter, frame.invProj)
        : DofUnprojectView(SsrFullNdc(fullPxX, invDepth), dX, frame.invProj);
    float3 posY = DofIsBackground(dY)
        ? DofUnprojectView(SsrFullNdc(fullPxY, invDepth), dCenter, frame.invProj)
        : DofUnprojectView(SsrFullNdc(fullPxY, invDepth), dY, frame.invProj);
    float3 N = normalize(cross(posX - viewPos, posY - viewPos) + 1e-9f);
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;
    // Safe for double-sided glass: reflect() is invariant under N -> -N,
    // so the viewer-facing flip above never mirrors the reflection.

    // Fresnel (Schlick, dielectric F0): grazing angles — the bistro
    // shopfront seen from the street — reflect the most.
    float ndv = saturate(dot(N, V));
    float fres = sp.f0 + (1.0f - sp.f0) * pow(1.0f - ndv, 5.0f);

    float3 R = reflect(-V, N);
    // No dot(R, V) early-out: R is mirrored across the viewer-facing N,
    // so dot(R, N) = dot(V, N) > 0 by construction (always above the
    // surface). dot(R, V) < 0 is the COMMON valid case (floor, bistro
    // side glass at grazing: the reflection points away from the camera
    // into the scene). Behind-camera rays are handled in the march
    // (rayDist <= 0 break).

    uint nSteps = clamp(sp.steps, 1, 128);
    float stepLen = max(sp.maxDistance, 0.1f) / float(nSteps);
    // Dither the ray origin along R to break up step banding; bias 2 mm
    // toward the viewer so thin double-sided panes don't self-hit.
    float jitter = SsrIGN(float2(halfPx));
    float3 marchPos = viewPos + R * (stepLen * jitter) + N * 2e-3f;

    float2 hitUv = float2(-1.0f, -1.0f);
    float hitFade = 0.0f;
    float2 fullExtent = float2(1.0f / max(invDepth.x, 1e-9f),
                               1.0f / max(invDepth.y, 1e-9f));
    for (uint i = 0; i < 128; ++i)
    {
        if (i >= nSteps)
            break;
        marchPos += R * stepLen;
        float rayDist = -marchPos.z;
        if (rayDist <= 0.0f)
            break; // behind the camera

        float4 clip = mul(frame.proj, float4(marchPos, 1.0f));
        float invW = 1.0f / max(clip.w, 1e-9f);
        float2 marchNdc = clip.xy * invW;
        // Project to full-res depth UV (NDC y-down -> uv = ndc * 0.5 + 0.5,
        // same as ssao.frag.hlsl line ~148).
        float2 marchUv = marchNdc * 0.5f + 0.5f;
        if (marchUv.x < 0.0f || marchUv.x > 1.0f || marchUv.y < 0.0f || marchUv.y > 1.0f)
            break; // off-screen: miss (no history to stretch)

        int2 samplePx = int2(marchUv * fullExtent);
        float sampD = depthTex.Load(int3(samplePx, 0)).r;
        if (DofIsBackground(sampD))
            continue; // sky behind the ray: keep marching
        float2 sampNdc = SsrFullNdc(samplePx, invDepth);
        float sceneDist = DofViewDist(sampD, sampNdc, frame.invProj);

        // Hit when the ray has crossed into the surface slab:
        // ray behind the scene depth, within the thickness band.
        float delta = rayDist - sceneDist;
        if (delta > 0.0f && delta < max(sp.thickness, 0.01f))
        {
            // Binary refine: back up half-steps to tighten the hit.
            float3 refinePos = marchPos;
            float refineStep = stepLen * 0.5f;
            for (uint r = 0; r < 8; ++r)
            {
                if (r >= sp.refineSteps)
                    break;
                refinePos -= R * refineStep;
                float4 rc = mul(frame.proj, float4(refinePos, 1.0f));
                float2 rn = (rc.xy / max(rc.w, 1e-9f)) * 0.5f + 0.5f;
                int2 rp = int2(clamp(rn, float2(0.0f, 0.0f), float2(1.0f, 1.0f)) * fullExtent);
                float rd = depthTex.Load(int3(rp, 0)).r;
                if (!DofIsBackground(rd))
                {
                    float sd = DofViewDist(rd, SsrFullNdc(rp, invDepth), frame.invProj);
                    if (-refinePos.z > sd)
                        refinePos += R * refineStep; // still inside: go forward
                }
                refineStep *= 0.5f;
            }
            float4 hc = mul(frame.proj, float4(refinePos, 1.0f));
            hitUv = (hc.xy / max(hc.w, 1e-9f)) * 0.5f + 0.5f;
            // Screen-edge fade: hits near the border smear on motion.
            float2 edge = min(hitUv, 1.0f - hitUv);
            hitFade = saturate(min(edge.x, edge.y) / 0.1f);
            // Distance fade: far hits are exponentially less reliable.
            hitFade *= exp(-rayDist / max(sp.maxDistance, 1e-3f));
            break;
        }
        if (delta >= max(sp.thickness, 0.01f) && delta > stepLen * 2.0f)
        {
            // Ray tunneled deep behind geometry (missed a thin occluder
            // band entirely): keep marching — no early-out, so thin glass
            // mullions don't kill window reflections.
        }
    }

    if (hitUv.x < 0.0f || hitFade <= 0.0f)
    {
        // Miss: no scene geometry along the ray (sky behind the march,
        // off-screen exit, behind-camera). Fall back to the analytic sky
        // specular IBL along the WORLD-space reflection vector so mirrors
        // reflect the sky instead of going black. Respects the IBL toggle
        // (EvaluateSpecularEnv returns 0 when IBL is off).
        float3 Rw = mul(frame.invView, float4(R, 0.0f)).xyz;
        float3 sky = EvaluateSpecularEnv(normalize(Rw + 1e-9f),
            float3(fres, fres, fres), 0.05f, frame.env);
        OUT.outColor = float4(SanitizeHdr(sky) * sp.intensity, 1.0f);
        return OUT;
    }
    float3 reflected = SanitizeHdr(hdrTex.Sample(smp, hitUv).rgb);
    OUT.outColor = float4(reflected * (fres * sp.intensity), hitFade);
    return OUT;
}
