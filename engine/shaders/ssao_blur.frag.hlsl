#define SSAO_PASS
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

// Must match Engine/Rendergraph/ssao.h SsaoBlurPush (32 bytes).
struct SsaoBlurPush
{
    uint aoTextureIndex; // raw half-res AO
    uint depthTextureIndex; // full-res depth (edge stops)
    uint samplerIndex; // linear clamp (AO taps only; depth uses Load)
    uint pad0;
    float sharpness; // depth edge weight (higher = tighter)
    float nearPlane;
    float2 texelSize; // 1 / half-res extent
};

[[vk::push_constant]]
ConstantBuffer<SsaoBlurPush> bp;

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D aoTex = textures[NonUniformResourceIndex(bp.aoTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(bp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];
    float nearPlane = max(bp.nearPlane, 1e-4f);

    // Center 2x2 block from full-res depth. Strict classification, same
    // rule as the raw pass: any background tap (sky / clear / OOB Load)
    // marks the block unoccluded — raw already wrote 1.0 there — and the
    // gather is skipped entirely so mixed blocks never average geo taps
    // into sky pixels.
    int2 fullBase = int2(IN.pos.xy) * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;
    if (AoIsBackground(d00) || AoIsBackground(d10) ||
        AoIsBackground(d01) || AoIsBackground(d11))
    {
        OUT.outColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return OUT;
    }
    float centerDist = AoViewDist((d00 + d10 + d01 + d11) * 0.25f, nearPlane);

    // 5x5 gaussian-ish blur, depth-rejected across discontinuities.
    float acc = 0.0f;
    float wSum = 0.0f;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float2 tapUv = IN.uv + float2(float(x) * bp.texelSize.x,
                                          float(y) * bp.texelSize.y);
            float tapAo = aoTex.Sample(smp, tapUv).r;

            int2 tapFull = fullBase + int2(x * 2, y * 2);
            float tapD = depthTex.Load(int3(tapFull, 0)).r;
            float tapDist = AoIsBackground(tapD)
                ? kAoSkyDist
                : AoViewDist(tapD, nearPlane);

            float wx = (x == 0) ? 6.0f : ((abs(x) == 1) ? 4.0f : 1.0f);
            float wy = (y == 0) ? 6.0f : ((abs(y) == 1) ? 4.0f : 1.0f);
            float w = wx * wy * AoEdgeWeight(tapDist, centerDist, bp.sharpness);
            acc += tapAo * w;
            wSum += w;
        }
    }
    float ao = (wSum > 1e-5f) ? acc / wSum : 1.0f;
    OUT.outColor = float4(ao, ao, ao, 1.0f);
    return OUT;
}
