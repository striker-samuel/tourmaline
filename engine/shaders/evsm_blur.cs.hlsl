// EVSM separable Gaussian prefilter (async compute, pipelined).
//
// One dispatch per tile per direction (caller loops tiles; rendergraph
// barriers come from one AsyncCompute pass per direction — H then V).
// H: moments(resolved MSAA) -> temp. V: temp -> blurred (sampled by light).
// Reads use the linear clamp sampler through textures[] (filterable RG32F);
// writes go through the bindless storage array at the SAME slot index
// (see VulkanResourceManager::UpdateDescriptors: binding 2 mirrors binding
// 0 with GENERAL layout, so src/dst indices are interchangeable).
//
// Tile discipline: every tap clamps into [uvMin, uvMax] (half-texel inset,
// same as the cascade uvClamp) so sibling tiles never bleed. Out-of-tile
// threads exit early; the destination tile is fully covered.
//
// C++ mirror: smvk::EvsmBlurPush in Engine/Rendergraph/evsm.h (64 bytes).

#define EVSM_BLUR_PASS
#include "common.hlsli"

// Bindless storage image array (set 0, binding 2). Declared here — the only
// TU that writes storage images — so no other pass pays the declaration.
[[vk::binding(2, 0)]]
RWTexture2D<float4> evsmStorage[];

struct EvsmBlurPush
{
    uint srcIndex;
    uint dstIndex;
    uint samplerIndex;
    uint direction; // 0 = H, 1 = V
    float texelU;
    float texelV;
    float uvMinU;
    float uvMinV;
    float uvMaxU;
    float uvMaxV;
    uint tileOffsetX;
    uint tileOffsetY;
    uint tileWidth;
    uint tileHeight;
    uint blurRadius;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<EvsmBlurPush> pcBlur;

// 5-tap Gaussian (sigma ~1.0). The kernel is STRIDE-DILATED for wide
// prefiltering: step = blurRadius/2 texels per tap, so push.blurRadius
// is the effective half-width in texels (radius 2 = classic 5-tap;
// radius 8 = the same weights spread over a 17-texel window — the EVSM
// softness knob drives this; the moments stay a valid expectation
// because the weights are normalized).
static const float kBlurW[5] = { 0.0545f, 0.2442f, 0.4026f, 0.2442f, 0.0545f };

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pcBlur.tileWidth || tid.y >= pcBlur.tileHeight)
        return;

    uint2 dstPix = uint2(pcBlur.tileOffsetX + tid.x, pcBlur.tileOffsetY + tid.y);
    uint srcIdx = NonUniformResourceIndex(pcBlur.srcIndex);
    uint dstIdx = NonUniformResourceIndex(pcBlur.dstIndex);

    // Destination texel center in atlas UV.
    float2 baseUV = (float2(dstPix) + 0.5f) * float2(pcBlur.texelU, pcBlur.texelV);

    float2 acc = float2(0.0f, 0.0f);
    float wSum = 0.0f;
    // Stride dilation: taps at -2..+2 steps of (blurRadius/2) texels.
    const int step = max(1, (int)min(pcBlur.blurRadius, 8u) / 2);
    [unroll]
    for (int t = -2; t <= 2; ++t)
    {
        float w = kBlurW[t + 2];
        float2 tapUV = baseUV;
        if (pcBlur.direction == 0u)
            tapUV.x += float(t * step) * pcBlur.texelU;
        else
            tapUV.y += float(t * step) * pcBlur.texelV;
        tapUV = clamp(tapUV, float2(pcBlur.uvMinU, pcBlur.uvMinV), float2(pcBlur.uvMaxU, pcBlur.uvMaxV));
        float2 m = textures[srcIdx].SampleLevel(samplers[pcBlur.samplerIndex], tapUV, 0).rg;
        // Sanitize once: a single inf/NaN moment (fp32 overflow under a
        // huge exponent, or uncleared tile) poisons the whole blur window.
        // Floor care: the NEGATIVE channel spans e^-c (d=1) .. 1 (d=0) —
        // with c=42 that is 5.7e-19 — a 1e-6 floor here would clamp the
        // channel up to e^-13.8, erasing all blocker-depth information
        // below d ~ 0.33 and killing the soft-shadow penumbra estimator
        // (EvsmBlockerDepth reads this channel). Keep the floor ~e^-c.
        m = clamp(m, float2(1e-6f, 1e-20f), float2(3.0e37f, 1.0f));
        acc += m * w;
        wSum += w;
    }
    acc /= max(wSum, 1e-6f);

    evsmStorage[dstIdx][dstPix] = float4(acc, 0.0f, 1.0f);
}
