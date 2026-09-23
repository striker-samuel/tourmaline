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

// Must match Engine/Rendergraph/motionblur.h MotionBlurPush (52 bytes).
struct MotionBlurPush
{
    uint hdrTextureIndex;
    uint motionTextureIndex;
    uint tileVelTextureIndex; // UINT32_MAX = no tile pass (per-pixel only)
    uint samplerIndex;
    uint sampleCount; // 1..32 gather taps (1 = copy)
    float maxRadiusPx; // per-pixel blur clamp (px)
    float velocityScale; // shutter strength on motion vectors
    float2 texelSize;
    float2 jitterDelta; // (prevJitter - currJitter) UV: strip it from the
                        // sampled vectors so a jittered-but-still camera
                        // produces NO blur (the raw vectors must keep the
                        // jitter for SMAA T2x / DLSS reprojection)
    float2 tileScale; // tile-map texel count (xy): pixel -> tile point lookup
};

[[vk::push_constant]]
ConstantBuffer<MotionBlurPush> mp;

// Interleaved gradient noise (Jimenez 2014, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare"): a 0-cost analytic hash
// engineered to mimic interleaved blue-ish noise without a LUT. Used to
// phase-offset the gather taps so a low tap count reads as fine grain
// instead of banding; temporal AA (SMAA T2x / DLSS) integrates the grain
// away. The box average stays unbiased: every pixel still takes n taps
// over the same span, only the phase rotates.
float InterleavedGradientNoise(float2 pix)
{
    return frac(52.9829189 * frac(0.06711056 * pix.x + 0.00583715 * pix.y));
}

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

    // Tile dilation (CoD:AW): the dilated tile map carries the max
    // velocity of the 5x5 tile neighborhood (jitter-stripped + scaled
    // by phase A, same domain as vUv). Adopting it when it beats the
    // pixel's own makes revealed background streak with the foreground
    // that vacated it, instead of staying sharp around silhouettes.
    if (mp.tileVelTextureIndex != 0xffffffff)
    {
        Texture2D tileVel = textures[NonUniformResourceIndex(mp.tileVelTextureIndex)];
        int2 tileId = clamp(int2(floor(IN.uv * mp.tileScale)), int2(0, 0),
            int2(mp.tileScale) - 1);
        float2 vTileUv = tileVel.Load(int3(tileId, 0)).xy;
        if (isfinite(vTileUv.x + vTileUv.y))
        {
            float2 vTilePx = vTileUv / max(mp.texelSize, float2(1e-6, 1e-6));
            float lenTilePx = length(vTilePx);
            if (lenTilePx > lenPx)
            {
                vPx = vTilePx;
                lenPx = lenTilePx;
            }
        }
    }

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

    // Centered box gather, taps at (i + ign)/n - 0.5: the span stays
    // [-0.5, +0.5) around the pixel but the phase rotates per pixel, so
    // fast edges grain instead of banding at low tap counts.
    float2 pix = floor(IN.uv / max(mp.texelSize, float2(1e-6, 1e-6)));
    float ign = InterleavedGradientNoise(pix);
    float3 acc = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 32; ++i)
    {
        if (i >= n)
            break;
        float t = (float(i) + ign) / float(n) - 0.5;
        // Per-tap sanitize (bloom_down discipline): one +inf HDR texel
        // would poison the average written back into the RGBA16F chain.
        acc += SanitizeHdr(hdr.Sample(smp, IN.uv + vClampedUv * t).rgb);
    }
    OUT.outColor = float4(acc / float(n), 1.0f);
    return OUT;
}
