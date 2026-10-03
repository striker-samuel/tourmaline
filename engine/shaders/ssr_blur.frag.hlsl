#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"

// SSR blur: depth-aware bilateral 5x5 blur of the half-res reflection
// layer (rgb = reflected x fresnel x intensity, a = confidence).
//
// SSGI shared-HZB rework: every depth read comes from the final-depth
// MAX-HZB (hzb_max.cs.hlsl, mip1 = nearest-of-2x2 under reversed-Z) via
// buffer-device-address scalars — ZERO depth-texture loads (the old pass
// did 29: a 4-load quad max for the center block + one corner-texel Load
// per tap). The mip1 value IS the center block's quad max (exact), and
// the tap reps move from single CORNER samples to the same
// nearest-of-block convention the SSR compose's block reps use. The view
// distance is the infinite reversed-Z near/d form (exact for this
// projection — the old invProj unprojection evaluated it at the winning
// tap's sub-texel NDC; the difference is sub-texel placement only).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F blurred reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrBlurPush (56 bytes).
// All-scalar layout on purpose: float2 members would land at offsets 28
// and 36, which are not 8-byte aligned — spirv-val rejects the module
// ("improperly straddling vector") and the driver is free to load the
// whole push block at shifted offsets, corrupting every member after
// pad0. The C++ mirror already uses separate floats; this matches it.
struct SsrBlurPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<HzbBuffer> hzb;  // MAX-HZB: mip1 (nearest-of-2x2) reads
    uint reflTextureIndex;  // half-res raw reflection
    uint samplerIndex;      // linear clamp (reflection taps only)
    float sharpness; // depth edge weight (higher = tighter)
    float texelX;    // 1 / half-res width
    float texelY;    // 1 / half-res height
    uint hzbMip1Offset; // float offset of mip1 in the HZB buffer
    uint hzbMip1Stride; // mip1 row stride (floats)
    uint halfW;    // half-res extent (BDA tap clamp domain)
    uint halfH;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsrBlurPush> bp;

// Relative-depth edge weight: 1 on the same surface, ~0 across a
// discontinuity. Background taps carry the sky sentinel distance, so a
// geometry|sky step collapses the weight to ~0 at worst.
float SsrEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = bp.frameUniforms.Get();
    Texture2D reflTex = textures[NonUniformResourceIndex(bp.reflTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    int2 halfPx = int2(pos.xy);

    // Center 2x2 block: HZB mip1 IS the block's quad MAX — the nearest
    // valid tap (reverse-Z: max depth; sky writes 0 so the max ignores
    // it; an all-background block reads 0) — the exact value the old
    // 4-load quad max derived, as ONE BDA scalar.
    float dCenter = bp.hzb.Get().texels[
        bp.hzbMip1Offset + (uint) halfPx.y * bp.hzbMip1Stride + (uint) halfPx.x];
    if (DofIsBackgroundOrGarbage(dCenter))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }
    // Infinite reversed-Z: view distance = near / d (exact for this
    // projection; the old code unprojected the winning tap's texel
    // center — a sub-texel NDC placement difference only).
    float centerDist = nearPlane / max(dCenter, 1e-6f);

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

            // Tap rep depth: mip1 at the tap's half-res coords. The
            // reflection TAPS were always edge-clamped (the linear-clamp
            // sampler), but the old depth Loads returned 0 (background)
            // out of bounds — a mismatched pair. Clamping the rep the
            // same way pairs them consistently (a border-ring behavior
            // change: edge taps now survive with the edge rep).
            int2 thp = clamp(
                halfPx + int2(x, y),
                int2(0, 0),
                int2((int) bp.halfW - 1, (int) bp.halfH - 1));
            float tapD = bp.hzb.Get().texels[
                bp.hzbMip1Offset + (uint) thp.y * bp.hzbMip1Stride + (uint) thp.x];
            float tapDist = DofIsBackgroundOrGarbage(tapD)
                ? 1e5 // sky sentinel (matches dof.hlsli scale)
                : nearPlane / max(tapD, 1e-6f);

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
