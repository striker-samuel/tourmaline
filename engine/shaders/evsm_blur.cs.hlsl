// EVSM separable Gaussian prefilter (async compute, pipelined).
//
// One dispatch per tile per direction (caller loops tiles; rendergraph
// barriers come from one AsyncCompute pass per direction — H then V).
// H: moments(resolved MSAA) -> temp. V: temp -> blurred (sampled by light).
// Reads use the linear clamp sampler through textures[] (filterable RGBA32F);
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
// float4 MUST match the RGBA32F moment atlas: an OpTypeImage Format operand
// mismatched against the view's VkFormat is undefined for the whole image
// (GPU-assisted validation flags it; "undefined values" poison the
// loads/stores driver-dependently).
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

// Dense 9-tap Gaussian (offsets -4..+4 texels, sigma from blurRadius).
// The previous kernel was STRIDE-DILATED (5 taps spread over up to 17
// texels with gaps): sparse sampling of the exponential moment field
// aliases depth edges into bands/moire, and the gaps leak unfiltered
// texels through. A dense kernel has no gaps: every texel in the support
// contributes, so the filtered expectation is smooth. blurRadius sets the
// Gaussian sigma (radius 2 ~= classic soft, 8 = wide); support stays +-4
// (wider sigmas just flatten within the support — no aliasing gaps).
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

    // Sigma ~ half the requested radius, clamped so the 9-tap support
    // (+-4 texels) covers +-1.3..+-4 sigma: small radii stay tight and
    // detailed, large radii go wide without sparse-tap gaps.
    const float sigma = clamp(float(pcBlur.blurRadius) * 0.5f, 1.0f, 3.0f);
    const float inv2s2 = 0.5f / max(sigma * sigma, 1e-4f);

    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;
    [unroll]
    for (int t = -4; t <= 4; ++t)
    {
        float w = exp(-float(t * t) * inv2s2);
        float2 tapUV = baseUV;
        if (pcBlur.direction == 0u)
            tapUV.x += float(t) * pcBlur.texelU;
        else
            tapUV.y += float(t) * pcBlur.texelV;
        tapUV = clamp(tapUV, float2(pcBlur.uvMinU, pcBlur.uvMinV), float2(pcBlur.uvMaxU, pcBlur.uvMaxV));
        float4 m = textures[srcIdx].SampleLevel(samplers[pcBlur.samplerIndex], tapUV, 0).rgba;
        // Sanitize once per channel: a single inf/NaN moment (fp32 overflow
        // under a huge exponent, or an uncleared tile) poisons the whole
        // blur window. Channel layout is (M1+, M2+, M1-, M2-) of the two
        // exponential warps (see evsm.hlsli). Floor care: the NEGATIVE
        // first moment spans e^-c (d=1) .. 1 (d=0) — with c=42 that is
        // 5.7e-19 — and the negative second moment spans e^-2c .. 1
        // (3.3e-37 at c=42, still a normal fp32). Floors must stay BELOW
        // those leges: clamping M1- up to e.g. 1e-6 would erase all
        // blocker-depth information below d ~ 0.33 and kill the
        // soft-shadow penumbra estimator (EvsmBlockerDepth reads M1-);
        // clamping M2- above e^-2c poisons the negative Cantelli variance.
        m = clamp(m,
            float4(1e-6f, 1e-6f, 1e-20f, 1e-38f),
            float4(3.0e37f, 3.0e37f, 1.0f, 1.0f));
        acc += m * w;
        wSum += w;
    }
    acc /= max(wSum, 1e-6f);

    // float4 store: the atlas is RGBA32F — all four moments are real.
    // (The old float2 store against an RG32F view was the format-mismatch
    // hazard; the same rule applies now with one more channel.)
    evsmStorage[dstIdx][dstPix] = acc;
}
