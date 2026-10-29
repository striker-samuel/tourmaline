// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"
#include "ssr_receiver.hlsli"

// SSR separable blur: the H+V twin of ssr_blur.frag.hlsl (the 2D
// bilateral stays the quality baseline — SsrBlurMode::Bilateral2D).
// The 2D pass costs (2r+1)^2 taps/px (25/49/81 at the radius 2/3/4
// buckets); this shader runs the SAME bilateral weights as two 1D
// passes (H then V, selected by bp.axis) for 2*(2r+1) taps/px across
// both (10/14/18) — the intermediate lands in ssrBlurTemp, the V pass
// writes the same ssrBlur target the temporal/composite chain already
// consumes.
//
// Depth discipline (the correctness-critical part): BOTH passes weight
// their taps against the ORIGINAL scene HZB mip1 depths, never against
// the intermediate image — the H pass already spreads color+confidence
// across boundaries the bilateral depth weight would have rejected, and
// a V pass weighting against filtered data could not undo that. Reading
// the raw scene depth in V keeps the cross-surface rejection the 2D
// kernel had; the residual difference is that H-crossed values at a
// SAME-depth V tap survive (a small bright-edge bleed vs the 2D pass —
// raise SsrDesc.blurSharpness if it reads).
//
// Everything else is the 2D shader verbatim: the center mip1 quad-max
// read + background rejection, the infinite reversed-Z near/d view
// distance, the receiver roughness/metal radius buckets (2/3/4 rings,
// metals only — dielectrics keep the 5x5-equivalent radius 2), the
// {6,4,1}+halving axis weights, and the whole-float4 accumulation so
// rgb and the alpha confidence stay premultiplied-consistent.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F blurred reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrSeparableBlurPush (72 bytes).
// Field order is SsrBlurPush verbatim through gbufferTextureIndex, then
// the axis selector + pad (the H and V passes share ONE compiled
// program — axis is a push constant, not a shader variant). All-scalar
// layout on purpose (see SsrBlurPush's straddling-vector comment).
// receiverData: the classify's packed receiver metadata (the SELECTED
// receiver's material + normal) — one BDA scalar per read, replacing
// the old top-left-texel G-buffer Load.
struct SsrSeparableBlurPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<HzbBuffer> hzb;  // MAX-HZB: mip1 (nearest-of-2x2) reads
    vk::BufferPointer<SsrReceiverData> receiverData; // packed receiver per half-res pixel
    uint reflTextureIndex;  // half-res source (raw for H, blurTemp for V)
    uint samplerIndex;      // linear clamp (reflection taps only)
    float sharpness; // depth edge weight (higher = tighter)
    float texelX;    // 1 / half-res width
    float texelY;    // 1 / half-res height
    uint hzbMip1Offset; // float offset of mip1 in the HZB buffer
    uint hzbMip1Stride; // mip1 row stride (floats)
    uint halfW;    // half-res extent (BDA tap clamp domain)
    uint halfH;
    uint gbufferTextureIndex; // thin G-buffer gate: 0xffffffff = no
                              // G-buffer (the receiver buffer holds the
                              // no-data sentinel -> radius 2, the old
                              // 5x5 exactly); the G-buffer TEXTURE is
                              // no longer sampled by this pass
    uint axis;  // 0 = horizontal (raw -> blurTemp), 1 = vertical (blurTemp -> blurred)
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsrSeparableBlurPush> bp;

// Relative-depth edge weight: 1 on the same surface, ~0 across a
// discontinuity. Background taps carry the sky sentinel distance, so a
// geometry|sky step collapses the weight to ~0 at worst.
float SsrEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

// Axis weight: the 5x5's {6,4,1} table kept EXACT for radius 2 (the
// validated smooth look, bit-identical); wider radii continue the
// falloff (halving per ring past 2 — the tail spreads the rough lobe,
// the bilateral depth weights still own the seams).
float SsrAxisWeight(int k)
{
    const int a = abs(k);
    if (a == 0) return 6.0f;
    if (a == 1) return 4.0f;
    if (a == 2) return 1.0f;
    return exp2(-(float)(a - 2)); // |k| 3 -> 0.5, 4 -> 0.25
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

    // Receiver roughness + metal -> kernel radius bucket (the 2D
    // shader's comment verbatim): metals keep full trace energy in the
    // trace, so the blur owns their lobe spread — for metals only;
    // dielectrics keep BOTH the (1-r)^2 fade and the radius-2 kernel
    // (bit-identical to the old fixed 5x5). MATERIAL SOURCE: the
    // classify's receiver metadata (the SELECTED nearest-valid texel of
    // the block — the same surface the classifier gated rays on; the
    // old top-left-texel read let a material-edge block borrow a
    // neighbor's bucket). No G-buffer = the sentinel (matte, non-metal)
    // -> radius 2.
    float roughness = 0.0f;
    bool metal = false;
    bool haveNormals = false;
    float3 centerN = float3(0.0f, 0.0f, 1.0f);
    if (bp.gbufferTextureIndex != 0xffffffffu)
    {
        float4 gb = SsrUnpackReceiver(bp.receiverData.Get().v[
            (uint) halfPx.y * bp.halfW + (uint) halfPx.x]);
        roughness = saturate(gb.b);
        metal = gb.a >= 0.5f;
        centerN = SsrOctDecode(gb.xy);
        haveNormals = true;
    }
    const int radius = (metal && roughness >= 0.35f)
        ? (roughness < 0.7f ? 3 : 4)
        : 2;

    // The pass direction: one compiled program, axis via push constant.
    const int2 dir = (bp.axis == 0u) ? int2(1, 0) : int2(0, 1);

    // Confidence-aware 1D accumulation (the 2D shader's note verbatim:
    // rgb weighted by confidence x bilateral weight, normalized by the
    // confidence mass — a miss no longer dilutes valid radiance).
    // BOTH axes run this convention: the H pass OUTPUTS unpremultiplied
    // rgb + reconstructed confidence, the V pass consumes and re-emits
    // the same pair — an H axis that averaged independently would
    // poison the V axis's confidence weighting at the source.
    float3 rgbAcc = float3(0.0f, 0.0f, 0.0f);
    float confAcc = 0.0f;
    float wSum = 0.0f;
    [loop]
    for (int k = -radius; k <= radius; ++k)
    {
        const int2 offset = k * dir;
        float2 tapUv = IN.uv + float2(float(offset.x) * bp.texelX,
                                      float(offset.y) * bp.texelY);
        float4 tap = reflTex.Sample(smp, tapUv);

        // Tap rep depth: ORIGINAL scene mip1 at the tap's half-res
        // coords (never the intermediate image's depth — see the header
        // note). The reflection TAPS are edge-clamped (the linear-clamp
        // sampler); clamping the rep the same way pairs them
        // consistently (border-ring taps survive with the edge rep).
        int2 thp = clamp(
            halfPx + offset,
            int2(0, 0),
            int2((int) bp.halfW - 1, (int) bp.halfH - 1));
        float tapD = bp.hzb.Get().texels[
            bp.hzbMip1Offset + (uint) thp.y * bp.hzbMip1Stride + (uint) thp.x];
        float tapDist = DofIsBackgroundOrGarbage(tapD)
            ? 1e5 // sky sentinel (matches dof.hlsli scale)
            : nearPlane / max(tapD, 1e-6f);

        float w = SsrAxisWeight(k)
            * SsrEdgeWeight(tapDist, centerDist, bp.sharpness);
        // Normal agreement (the receiver metadata's oct normals, world
        // space both sides — the 2D shader's note): separates
        // same-depth surfaces the depth weight cannot tell apart.
        // Gated so the no-G-buffer fallback keeps the old depth-only
        // weights.
        if (haveNormals)
        {
            float4 tapGb = SsrUnpackReceiver(bp.receiverData.Get().v[
                (uint) thp.y * bp.halfW + (uint) thp.x]);
            w *= SsrNormalWeight(centerN, SsrOctDecode(tapGb.xy));
        }
        float confidence = saturate(tap.a);
        rgbAcc += tap.rgb * confidence * w;
        confAcc += confidence * w;
        wSum += w;
    }
    // Reconstructed confidence + confidence-weighted radiance mean (the
    // 2D shader's tail note verbatim).
    float outConf = (wSum > 1e-5f) ? saturate(confAcc / wSum) : 0.0f;
    float3 outRgb = (confAcc > 1e-5f)
        ? rgbAcc / confAcc
        : float3(0.0f, 0.0f, 0.0f);
    OUT.outColor = float4(SanitizeHdr(outRgb), outConf);
    return OUT;
}
