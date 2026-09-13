#define MOTIONBLUR_PASS
#include "common.hlsli"

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/motionblur.h MotionBlurPush (40 bytes).
struct MotionBlurPush
{
    uint hdrTextureIndex;
    uint motionTextureIndex;
    uint samplerIndex;
    uint sampleCount; // 1..32 gather taps (1 = copy)
    float maxRadiusPx; // per-pixel blur clamp (px)
    float velocityScale; // shutter strength on motion vectors
    float2 texelSize;
    float2 jitterDelta; // (prevJitter - currJitter) UV: strip it from the
                        // sampled vectors so a jittered-but-still camera
                        // produces NO blur (the raw vectors must keep the
                        // jitter for SMAA T2x / DLSS reprojection)
};

[[vk::push_constant]]
ConstantBuffer<MotionBlurPush> mp;

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D hdr = textures[NonUniformResourceIndex(mp.hdrTextureIndex)];
    Texture2D motionTex = textures[NonUniformResourceIndex(mp.motionTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(mp.samplerIndex)];

    // TRUE velocity in UV (prevUV - currUV minus the temporal-jitter
    // component), scaled by the shutter strength.
    float2 vUv = (motionTex.Sample(smp, IN.uv).rg - mp.jitterDelta)
        * mp.velocityScale;

    // Guard: first frame / camera reset clamps huge reprojects in the
    // motion pass already; still ignore NaN-scale deltas defensively.
    if (!isfinite(vUv.x + vUv.y))
        vUv = float2(0.0, 0.0);

    float2 vPx = vUv / max(mp.texelSize, float2(1e-6, 1e-6));
    float lenPx = length(vPx);

    // Sub-pixel motion: no visible streak, keep the sharp center tap.
    uint n = clamp(mp.sampleCount, 1, 32);
    if (lenPx < 0.5 || n == 1 || mp.maxRadiusPx <= 0.0f)
    {
        OUT.outColor = float4(SanitizeHdr(hdr.Sample(smp, IN.uv).rgb), 1.0f);
        return OUT;
    }

    // Clamp the streak length, keep its direction.
    float maxLen = min(mp.maxRadiusPx, lenPx);
    float2 vClampedUv = (vPx / max(lenPx, 1e-5)) * maxLen * mp.texelSize;

    // Centered box gather: t in [-0.5, +0.5] spreads the blur symmetrically
    // around the pixel so fast edges streak both ways instead of dragging
    // one-sided ghosts behind the motion.
    float3 acc = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 32; ++i)
    {
        if (i >= n)
            break;
        float t = (n == 1) ? 0.0 : (float(i) / float(n - 1) - 0.5);
        // Per-tap sanitize (bloom_down discipline): one +inf HDR texel
        // would poison the average written back into the RGBA16F chain.
        acc += SanitizeHdr(hdr.Sample(smp, IN.uv + vClampedUv * t).rgb);
    }
    OUT.outColor = float4(acc / float(n), 1.0f);
    return OUT;
}
