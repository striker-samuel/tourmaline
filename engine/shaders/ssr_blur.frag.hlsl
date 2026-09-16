#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"

// SSR blur: depth-aware bilateral 5x5 blur of the half-res reflection
// layer (rgb = reflected x fresnel x intensity, a = confidence).
// Depth stays .Load'ed at full res so edges don't bleed; the center
// block's view distance is the authority (exact unprojection through
// invProj: flip / finite-far agnostic, same discipline as dof.hlsli).

struct PSIn
{
    float4 pos : SV_Position; // half-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F blurred reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrBlurPush (48 bytes).
// All-scalar layout on purpose: float2 members would land at offsets 28
// and 36, which are not 8-byte aligned — spirv-val rejects the module
// ("improperly straddling vector") and the driver is free to load the
// whole push block at shifted offsets, corrupting every member after
// pad0. The C++ mirror already uses separate floats; this matches it.
struct SsrBlurPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint reflTextureIndex;  // half-res raw reflection
    uint depthTextureIndex; // full-res depth (edge stops)
    uint samplerIndex;      // linear clamp (reflection taps only)
    uint pad0;
    float sharpness; // depth edge weight (higher = tighter)
    float texelX;    // 1 / half-res width
    float texelY;    // 1 / half-res height
    float invDepthW; // 1 / full-res depth width
    float invDepthH; // 1 / full-res depth height
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<SsrBlurPush> bp;

// Relative-distance edge weight: 1 on the same surface, ~0 across a
// discontinuity. Background taps carry the sky sentinel distance, so a
// geometry|sky step collapses the weight to ~0 at worst.
float SsrEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = bp.frameUniforms.Get();
    Texture2D reflTex = textures[NonUniformResourceIndex(bp.reflTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(bp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];

    // Center 2x2 block from full-res depth. Nearest valid tap (reverse-Z:
    // max depth) is the authority; an all-background block carries no
    // reflector and blurs to nothing.
    int2 halfPx = int2(IN.pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;
    float dCenter = 0.0f;
    int2 fullPxC = fullBase;
    if (!DofIsBackground(d00) && d00 > dCenter) { dCenter = d00; fullPxC = fullBase; }
    if (!DofIsBackground(d10) && d10 > dCenter) { dCenter = d10; fullPxC = fullBase + int2(1, 0); }
    if (!DofIsBackground(d01) && d01 > dCenter) { dCenter = d01; fullPxC = fullBase + int2(0, 1); }
    if (!DofIsBackground(d11) && d11 > dCenter) { dCenter = d11; fullPxC = fullBase + int2(1, 1); }
    if (DofIsBackground(dCenter))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }
    float2 invDepthSize = float2(bp.invDepthW, bp.invDepthH);
    float2 ndcC = float2((float(fullPxC.x) + 0.5f) * invDepthSize.x * 2.0f - 1.0f,
                         (float(fullPxC.y) + 0.5f) * invDepthSize.y * 2.0f - 1.0f);
    float centerDist = DofViewDist(dCenter, ndcC, frame.invProj);

    // 5x5 gaussian-ish blur, depth-rejected across discontinuities.
    // rgb and confidence share the same bilateral weights so the layer
    // stays premultiplied-consistent.
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float2 tapUv = IN.uv + float2(float(x) * bp.texelX,
                                          float(y) * bp.texelY);
            float4 tap = reflTex.Sample(smp, tapUv);

            int2 tapFull = fullBase + int2(x * 2, y * 2);
            float tapD = depthTex.Load(int3(tapFull, 0)).r;
            float tapDist;
            if (DofIsBackground(tapD))
            {
                tapDist = 1e5; // sky sentinel (matches dof.hlsli scale)
            }
            else
            {
                float2 tapNdc = float2((float(tapFull.x) + 0.5f) * invDepthSize.x * 2.0f - 1.0f,
                                       (float(tapFull.y) + 0.5f) * invDepthSize.y * 2.0f - 1.0f);
                tapDist = DofViewDist(tapD, tapNdc, frame.invProj);
            }

            float wx = (x == 0) ? 6.0f : ((abs(x) == 1) ? 4.0f : 1.0f);
            float wy = (y == 0) ? 6.0f : ((abs(y) == 1) ? 4.0f : 1.0f);
            float w = wx * wy * SsrEdgeWeight(tapDist, centerDist, bp.sharpness);
            acc += tap * w;
            wSum += w;
        }
    }
    OUT.outColor = (wSum > 1e-5f) ? acc / wSum : float4(0.0f, 0.0f, 0.0f, 0.0f);
    return OUT;
}
