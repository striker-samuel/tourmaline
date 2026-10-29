// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"
#include "ssr_receiver.hlsli"

// SSR blur: depth-aware bilateral blur of the half-res reflection
// layer (rgb = reflected x fresnel x intensity, a = confidence), with
// the kernel radius SCALED BY THE RECEIVER ROUGHNESS for METALS ONLY
// (2/3/4 rings = 5x5/7x7/9x9) — the per-material spread the old fixed
// 5x5 "could not do" that the trace's dielectric (1-r)^2 energy fade
// substituted for (metals now keep full trace energy; this blur owns
// their spread; dielectrics keep fade + 5x5, bit-identical).
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

// Must match Engine/Rendergraph/ssr.h SsrBlurPush (64 bytes).
// All-scalar layout on purpose: float2 members would land at offsets 28
// and 36, which are not 8-byte aligned — spirv-val rejects the module
// ("improperly straddling vector") and the driver is free to load the
// whole push block at shifted offsets, corrupting every member after
// pad0. The C++ mirror already uses separate floats; this matches it.
// Receiver-data note: the classify's packed receiver metadata (the
// SELECTED receiver's material + normal) — one BDA scalar per read,
// replacing the old top-left-texel G-buffer Load (a material-edge block
// borrowed a neighbor's radius bucket).
struct SsrBlurPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<HzbBuffer> hzb;  // MAX-HZB: mip1 (nearest-of-2x2) reads
    vk::BufferPointer<SsrReceiverData> receiverData; // packed receiver per half-res pixel
    uint reflTextureIndex;  // half-res raw reflection
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

    // Receiver roughness + metal -> kernel radius bucket: the per-material
    // lobe spread the old fixed 5x5 could not do — the trace's dielectric
    // (1-r)^2 energy fade substituted for it. The metal-aware trace now
    // keeps METALS at full energy, so the spread has to live HERE — for
    // metals only: dielectrics keep BOTH the fade and the 5x5
    // (bit-identical, the validated look, and the blur taps stay off the
    // whole-screen budget — the measured cost of a roughness-only gate
    // was +2..3.5 ms for zero visible dielectric gain). The radius is a
    // coarse per-material knob. MATERIAL SOURCE: the classify's receiver
    // metadata (the SELECTED nearest-valid texel of the block — the same
    // surface the classifier gated rays on; the old top-left-texel read
    // let a material-edge block borrow a neighbor's bucket). No
    // G-buffer = the sentinel (matte, non-metal) -> radius 2, the old
    // behavior exactly.
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

    // Gaussian-ish bilateral blur (radius-scaled), depth-rejected
    // across discontinuities. rgb and confidence share the same
    // bilateral weights so the layer stays premultiplied-consistent.
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
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

            float w = SsrAxisWeight(x) * SsrAxisWeight(y)
                * SsrEdgeWeight(tapDist, centerDist, bp.sharpness);
            // Normal agreement (the receiver metadata's oct normals,
            // world space both sides): separates same-depth surfaces
            // the depth weight cannot tell apart (a wall meeting a
            // floor). Gated so the no-G-buffer fallback keeps the
            // exact old depth-only weights.
            if (haveNormals)
            {
                float4 tapGb = SsrUnpackReceiver(bp.receiverData.Get().v[
                    (uint) thp.y * bp.halfW + (uint) thp.x]);
                w *= SsrNormalWeight(centerN, SsrOctDecode(tapGb.xy));
            }
            acc += tap * w;
            wSum += w;
        }
    }
    OUT.outColor = (wSum > 1e-5f) ? acc / wSum : float4(0.0f, 0.0f, 0.0f, 0.0f);
    return OUT;
}
