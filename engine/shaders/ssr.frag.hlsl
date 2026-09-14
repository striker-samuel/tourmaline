#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords (depth .Load)
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/ssr.h SsrPush (56 bytes).
struct SsrPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint steps;
    float2 texelSize;
    float maxDistance;
    float thickness;
    float intensity;
    float f0;
    uint refineSteps;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsrPush> sp;

// Interleaved gradient noise: ray-origin dither against banding.
float SsrIGN(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = sp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(sp.hdrTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    int2 px = int2(IN.pos.xy);
    float dCenter = depthTex.Load(int3(px, 0)).r;
    if (DofIsBackground(dCenter))
    {
        // Sky: nothing to reflect off — keep the base color.
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }

    float2 invExtent = sp.texelSize;
    float2 ndcXY = float2((float(px.x) + 0.5f) * invExtent.x * 2.0f - 1.0f,
                          (float(px.y) + 0.5f) * invExtent.y * 2.0f - 1.0f);
    float3 viewPos = DofUnprojectView(ndcXY, dCenter, frame.invProj);

    // Depth-derived normal from view-position neighbors (one pixel over).
    // A background neighbor means a silhouette: fall back to the face
    // normal of the valid side instead of fabricating a planar normal
    // across the edge (same discipline as ssao.frag.hlsl).
    int2 pxX = px + int2(1, 0);
    int2 pxY = px + int2(0, 1);
    float dX = depthTex.Load(int3(pxX, 0)).r;
    float dY = depthTex.Load(int3(pxY, 0)).r;
    float2 ndcX = ndcXY + float2(2.0f * invExtent.x * 2.0f, 0.0f);
    float2 ndcY = ndcXY + float2(0.0f, 2.0f * invExtent.y * 2.0f);
    float3 posX = DofIsBackground(dX)
        ? viewPos + float3(2.0f * invExtent.x * max(-viewPos.z, 0.1f), 0.0f, 0.0f)
        : DofUnprojectView(ndcX, dX, frame.invProj);
    float3 posY = DofIsBackground(dY)
        ? viewPos + float3(0.0f, 2.0f * invExtent.y * max(-viewPos.z, 0.1f), 0.0f)
        : DofUnprojectView(ndcY, dY, frame.invProj);
    float3 N = normalize(cross(posX - viewPos, posY - viewPos) + 1e-9f);
    float3 V = -viewPos / max(length(viewPos), 1e-6f);
    if (dot(N, V) < 0.0f)
        N = -N;

    // Fresnel (Schlick, dielectric F0): grazing angles — the bistro
    // shopfront seen from the street — reflect the most.
    float ndv = saturate(dot(N, V));
    float fres = sp.f0 + (1.0f - sp.f0) * pow(1.0f - ndv, 5.0f);

    float3 R = reflect(-V, N);
    if (dot(R, V) < 0.0f)
    {
        // Reflection goes behind the surface (grazing numeric): no march.
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }

    uint nSteps = clamp(sp.steps, 1, 128);
    float stepLen = max(sp.maxDistance, 0.1f) / float(nSteps);
    // Dither the ray origin along R to break up step banding.
    float jitter = SsrIGN(float2(px));
    float3 marchPos = viewPos + R * (stepLen * jitter) + N * 1e-3f;

    float2 hitUv = float2(-1.0f, -1.0f);
    float hitFade = 0.0f;
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
        float2 marchUv = marchNdc * 0.5f + 0.5f;
        if (marchUv.x < 0.0f || marchUv.x > 1.0f || marchUv.y < 0.0f || marchUv.y > 1.0f)
            break; // off-screen: miss (no history to stretch)

        int2 samplePx = int2(marchUv * float2(1.0f / max(invExtent.x, 1e-9f),
                                              1.0f / max(invExtent.y, 1e-9f)));
        float sampD = depthTex.Load(int3(samplePx, 0)).r;
        if (DofIsBackground(sampD))
            continue; // sky behind the ray: keep marching
        float2 sampNdc = float2((float(samplePx.x) + 0.5f) * invExtent.x * 2.0f - 1.0f,
                                (float(samplePx.y) + 0.5f) * invExtent.y * 2.0f - 1.0f);
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
                int2 rp = int2(clamp(rn, float2(0.0f, 0.0f), float2(1.0f, 1.0f))
                    * float2(1.0f / max(invExtent.x, 1e-9f),
                             1.0f / max(invExtent.y, 1e-9f)));
                float rd = depthTex.Load(int3(rp, 0)).r;
                if (!DofIsBackground(rd))
                {
                    float2 rndc = float2((float(rp.x) + 0.5f) * invExtent.x * 2.0f - 1.0f,
                                         (float(rp.y) + 0.5f) * invExtent.y * 2.0f - 1.0f);
                    float sd = DofViewDist(rd, rndc, frame.invProj);
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

    float3 outColor = base;
    if (hitUv.x >= 0.0f)
    {
        float3 reflected = SanitizeHdr(hdrTex.Sample(smp, hitUv).rgb);
        outColor += reflected * (fres * sp.intensity * hitFade);
    }
    OUT.outColor = float4(outColor, 1.0f);
    return OUT;
}
