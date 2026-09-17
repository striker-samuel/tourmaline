#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"

// SSR composite: depth-guided bilateral upsample of the half-res blurred
// reflection layer onto the FULL-res base HDR (additive).
// Runs in its own pass so the full-res depth is only ever a barriered
// sampled input here; downstream passes then sample plain HDR.

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F HDR + reflections
};

// Must match Engine/Rendergraph/ssr.h SsrComposePush (48 bytes).
struct SsrComposePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint blurTextureIndex;  // half-res blurred reflection (rgb, confidence)
    uint depthTextureIndex; // full-res depth (authority)
    uint hdrTextureIndex;   // full-res base HDR
    uint samplerIndex;      // linear clamp (HDR + reflection taps)
    float2 invDepthSize; // 1 / full-res depth extent
    float sharpness; // upsample edge weight
    float pad0;
    uint maskTextureIndex; // glass layer bindless, 0xffffffff = none
    uint pad2;
};

[[vk::push_constant]]
ConstantBuffer<SsrComposePush> cp;

float SsrUpEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D blurTex = textures[NonUniformResourceIndex(cp.blurTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(cp.depthTextureIndex)];
    Texture2D hdrTex = textures[NonUniformResourceIndex(cp.hdrTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(cp.samplerIndex)];

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    int2 fullPx = int2(IN.pos.xy);
    float d = depthTex.Load(int3(fullPx, 0)).r;
    // Glass layer (see glassmask.frag.hlsl): R = pane presence. A pane
    // pixel over sky/empty background is still a REFLECTOR — the trace
    // traced off the pane, not the background — so the sky early-out
    // must not drop its reflection. Depth authority below is the
    // through-content: sky taps all carry the 1e5 view-distance
    // sentinel, so they agree with a sky-through center and the
    // depth-guided upsample still splits at real silhouettes.
    // Sentinel guard mirrors the AO optional-texture discipline
    // (default_lit_body.hlsli): the bindless index only happens inside
    // the if — a raw && would let SPIR-V's non-short-circuit logical
    // and touch textures[0xffffffff].
    bool panePx = false;
    if (cp.maskTextureIndex != 0xffffffffu)
    {
        Texture2D glassTex = textures[NonUniformResourceIndex(cp.maskTextureIndex)];
        panePx = glassTex.Load(int3(fullPx, 0)).r > 0.5f;
    }
    if (DofIsBackgroundOrGarbage(d) && !panePx)
    {
        // Sky: nothing to reflect off — keep the base color.
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }
    float2 ndcC = float2((float(fullPx.x) + 0.5f) * cp.invDepthSize.x * 2.0f - 1.0f,
                         (float(fullPx.y) + 0.5f) * cp.invDepthSize.y * 2.0f - 1.0f);
    float centerDist = DofViewDist(d, ndcC, frame.invProj);

    // Half-res reflection extent derived from the full-res depth extent
    // (max(1, floor(n / 2)), same as Ssao::HalfExtentFor).
    float2 halfExtent = max(
        floor(float2(1.0f / cp.invDepthSize.x, 1.0f / cp.invDepthSize.y) * 0.5f),
        float2(1.0f, 1.0f));

    // 2x2 nearest half-res texels around this full-res pixel, weighted by
    // depth agreement with the pixel's own depth — the same discipline as
    // ao.hlsli AoUpsample, so reflection never bleeds across silhouettes.
    int2 halfMin = int2(floor((float2(fullPx) - 1.0f) * 0.5f));
    int2 halfMax = int2(halfExtent) - 1;
    float3 accRgb = float3(0.0f, 0.0f, 0.0f);
    float accA = 0.0f;
    float wSum = 0.0f;
    for (int ty = 0; ty < 2; ++ty)
    {
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = clamp(halfMin + int2(tx, ty), int2(0, 0), halfMax);
            int2 tapFull = hp * 2;
            // Representative depth of the tapped block: nearest valid tap
            // of its full-res 2x2 (reverse-Z max). Fully-background blocks
            // carry the sky sentinel -> weight ~0 at silhouettes.
            float b00 = depthTex.Load(int3(tapFull, 0)).r;
            float b10 = depthTex.Load(int3(tapFull + int2(1, 0), 0)).r;
            float b01 = depthTex.Load(int3(tapFull + int2(0, 1), 0)).r;
            float b11 = depthTex.Load(int3(tapFull + int2(1, 1), 0)).r;
            float bd = 0.0f;
            int2 bPx = tapFull;
            if (!DofIsBackgroundOrGarbage(b00) && b00 > bd) { bd = b00; bPx = tapFull; }
            if (!DofIsBackgroundOrGarbage(b10) && b10 > bd) { bd = b10; bPx = tapFull + int2(1, 0); }
            if (!DofIsBackgroundOrGarbage(b01) && b01 > bd) { bd = b01; bPx = tapFull + int2(0, 1); }
            if (!DofIsBackgroundOrGarbage(b11) && b11 > bd) { bd = b11; bPx = tapFull + int2(1, 1); }
            float bDist;
            if (DofIsBackgroundOrGarbage(bd))
            {
                bDist = 1e5;
            }
            else
            {
                float2 bNdc = float2((float(bPx.x) + 0.5f) * cp.invDepthSize.x * 2.0f - 1.0f,
                                     (float(bPx.y) + 0.5f) * cp.invDepthSize.y * 2.0f - 1.0f);
                bDist = DofViewDist(bd, bNdc, frame.invProj);
            }
            float2 halfUv = (float2(hp) + 0.5f) / max(halfExtent, float2(1.0f, 1.0f));
            float4 tap = blurTex.Sample(smp, halfUv);
            float w = SsrUpEdgeWeight(bDist, centerDist, cp.sharpness);
            accRgb += tap.rgb * w;
            accA += tap.a * w;
            wSum += w;
        }
    }
    float3 outColor = base;
    if (wSum > 1e-4f)
        outColor += (accRgb / wSum) * (accA / wSum);
    OUT.outColor = float4(SanitizeHdr(outColor), 1.0f);
    return OUT;
}
