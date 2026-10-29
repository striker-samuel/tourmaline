// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSGI_DIFFUSE_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"
#include "ssr_receiver.hlsli"

// Diffuse SSGI compose: the energy-conserving integration of the GI
// layer into the FULL-res HDR. This is the chain's reason to exist —
// NOT an additive bounce stacked on the baked GI (that would count the
// indirect lighting twice), but a REPLACEMENT of the diffuse-ambient
// slice the base already holds, wherever the screen-space trace is
// trustworthy:
//
//     L_final = L_base + s * c * (L_SSGI_diffuse - L_probe_diffuse)
//
//   s = the user intensity, c = the trace confidence (0 = the baked
//   GI/probe fallback stands EXACTLY as authored — crucial at screen
//   edges where rays cannot see off-screen geometry), the difference
//   is SIGNED (a dim/black local radiance DARKENS a receiver whose
//   probe ambient was too bright — never clamped to positive), and
//   only the final color is clamped >= 0.
//
// The receiver reflectance is applied HERE (not in the trace): the
// half-res blur/temporal filters must not smear a per-receiver albedo
// across neighborhood boundaries — the filters see raw radiance, the
// compose applies
//     L_SSGI_diffuse = albedo * kd * Li
// with kd = (1 - metal) * 0.96 (the conventional dielectric F0 0.04
// approximation — the G-buffer's metal flag is binary, so intermediate
// metallics read 0/1; the production version should use the material's
// true energy partition).
//
// The baseline (L_probe_diffuse) — the diffuse-ambient slice the base
// holds, reconstructed from Main's MRT exports (default_lit_body):
//     aoTerm  = (ambient + ambientSpec) * occ   [target1, pre-AO]
//     envSpec = ambientSpec * occ               [target2, pre-AO]
//     diffuse ambient = max(aoTerm - envSpec, 0) * aoEff
// Two sourcing modes (exactly one binds):
//   - AO ON:  the AO compose's THIRD output (ssao_compose.frag.hlsl
//     outDiffuseAmbient) already carries the aoEff-scaled baseline.
//   - AO OFF: aoEff = 1 — this pass derives it inline from Main's raw
//     aoTerm/envSpec exports.
//
// The upsample is ssr_compose.frag.hlsl's joint-bilateral 2x2 (HZB
// mip0 center depth, mip1 block reps, receiver-metadata tap normals,
// tent spatial factor, confidence-aware accumulation) — depth-guided
// so GI never bleeds across silhouettes.
//
// Debug views (cp.debugView, the ViewMode Ssgi* entries — the chain
// keeps running under them, unlike the flat FS views): 1 = hit
// confidence, 2 = raw trace radiance, 3 = the filtered layer this
// compose consumes, 4 = the ambient baseline, 5 = the signed
// contribution. The visualization REPLACES the pixel (it still writes
// the composed target, so the downstream chain stays intact); values
// are raw HDR — exposure holds its last scene value under a debug
// view, so pair with TOURMALINE_AUTO_EXPOSURE=0 for readable captures.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: the GI-replaced scene HDR
};

// Must match Engine/Rendergraph/ssgi_diffuse.h SsgiDiffuseComposePush
// (104 bytes). uint64 BDAs lead; scalars after.
struct SsgiDiffuseComposePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<HzbBuffer> hzb; // MAX-HZB: mip0 center, mip1 reps
    vk::BufferPointer<SsrReceiverData> receiverData; // packed receiver per half-res pixel
    uint giTextureIndex;       // half-res GI layer (temporal slot or blurred)
    uint hdrTextureIndex;      // full-res base HDR (the FROZEN pre-compose source)
    uint samplerIndex;         // linear clamp
    uint gbufferTextureIndex;  // full-res thin G-buffer (metal + center normal)
    uint gbuffer2TextureIndex; // full-res albedo layer (baseColor texel)
    uint ambientTextureIndex;  // AO-compose baseline output (0xffffffff = inline)
    uint aoTermTextureIndex;   // Main MRT export (the AO-off inline path)
    uint envSpecTextureIndex;  // Main/AO-composed env-spec export (the AO-off path)
    uint rawTextureIndex;      // half-res raw trace (debug view 2; 0xffffffff = off)
    float invDepthW;           // 1 / full-res depth extent
    float invDepthH;
    float sharpness;           // upsample edge weight
    float intensity;           // s (the user SSGI strength)
    uint fullW;                // full-res width (HZB mip0 row stride)
    uint fullH;
    uint hzbMip1Offset;        // float offset of mip1 in the HZB buffer
    uint hzbMip1Stride;        // mip1 row stride (floats)
    uint debugView;            // 0 off, 1 conf, 2 raw, 3 layer, 4 baseline, 5 contribution
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<SsgiDiffuseComposePush> cp;

float SsgiUpEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D giTex = textures[NonUniformResourceIndex(cp.giTextureIndex)];
    Texture2D hdrTex = textures[NonUniformResourceIndex(cp.hdrTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(cp.samplerIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    int2 fullPx = int2(pos.xy);
    // Center depth from HZB mip0 (the exact 1:1 depth copy).
    float d = cp.hzb.Get().texels[(uint) fullPx.y * cp.fullW + (uint) fullPx.x];
    if (DofIsBackgroundOrGarbage(d))
    {
        // Sky: no receiver — the base color stands (the analytic sky
        // already carries its own ambient).
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }
    float2 ndcC = float2((float(fullPx.x) + 0.5f) * cp.invDepthW * 2.0f - 1.0f,
                         (float(fullPx.y) + 0.5f) * cp.invDepthH * 2.0f - 1.0f);
    float centerDist = DofViewDist(d, ndcC, frame.invProj);

    // The pixel's OWN full-res G-buffer texel: the center normal (the
    // upsample's normal agreement) + the metal flag (kd). Absent
    // G-buffer (MSAA/legacy hosts — the chain gates off, but the
    // shader stays total): the neutral sentinel, kd 1.
    bool haveGbuffer = cp.gbufferTextureIndex != 0xffffffffu;
    float3 centerN = float3(0.0f, 0.0f, 1.0f);
    float metal = 0.0f;
    if (haveGbuffer)
    {
        Texture2D gbTex = textures[NonUniformResourceIndex(cp.gbufferTextureIndex)];
        float4 gb = gbTex.Load(int3(fullPx, 0));
        centerN = SsrOctDecode(gb.xy);
        metal = (gb.w >= 0.5f) ? 1.0f : 0.0f;
    }

    // The receiver albedo: the F0-chroma layer 2 (baseColorFactor x
    // texel — gbuffer.frag.hlsl's SV_Target1; it IS textured base
    // color, SSR merely reads it as metal F0). Absent = the GI bake's
    // white-world 0.7 convention.
    float3 albedo = float3(0.7f, 0.7f, 0.7f);
    if (cp.gbuffer2TextureIndex != 0xffffffffu)
    {
        Texture2D gb2Tex = textures[NonUniformResourceIndex(cp.gbuffer2TextureIndex)];
        albedo = saturate(gb2Tex.Load(int3(fullPx, 0)).rgb);
    }

    // Half-res extent: EXACT integer math from the push's uint
    // dimensions (the dark-triangle float-reconstruction lesson — see
    // ssr_compose.frag.hlsl's halfExtentU note).
    const uint2 halfExtentU = max(uint2(1u, 1u),
        uint2(cp.fullW / 2u, cp.fullH / 2u));
    const int2 halfExtentI = int2(halfExtentU);
    const float2 halfExtentF = float2(halfExtentU);

    // 2x2 nearest half-res texels, joint-bilateral (the ssr_compose
    // discipline): depth (mip1 reps) + normal (receiver metadata vs
    // the pixel's own) + tent spatial + confidence-aware accumulation.
    // Border candidates are SKIPPED (the normalization absorbs them).
    int2 halfMin = int2(floor((float2(fullPx) - 1.0f) * 0.5f));
    float2 fullCenter = float2(fullPx) + 0.5f;
    float3 accRgb = float3(0.0f, 0.0f, 0.0f);
    float accA = 0.0f;
    float wSum = 0.0f;
    [loop]
    for (int ty = 0; ty < 2; ++ty)
    {
        [loop]
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = halfMin + int2(tx, ty);
            if (hp.x < 0 || hp.y < 0 || hp.x >= halfExtentI.x || hp.y >= halfExtentI.y)
                continue;
            float bd = cp.hzb.Get().texels[
                cp.hzbMip1Offset + (uint) hp.y * cp.hzbMip1Stride + (uint) hp.x];
            float bDist = DofIsBackgroundOrGarbage(bd)
                ? 1e5
                : nearPlane / max(bd, 1e-6f);
            float2 halfUv = (float2(hp) + 0.5f) / halfExtentF;
            float4 tap = giTex.Sample(smp, halfUv);
            float2 tapCenterFull = float2(hp) * 2.0f + 1.0f;
            float2 delta = abs(fullCenter - tapCenterFull) * 0.5f;
            float2 axisWeight = saturate(1.0f - delta);
            float spatialWeight = axisWeight.x * axisWeight.y;
            float w = spatialWeight
                * SsgiUpEdgeWeight(bDist, centerDist, cp.sharpness);
            if (haveGbuffer)
            {
                float4 repGb = SsrUnpackReceiver(cp.receiverData.Get().v[
                    (uint) hp.y * halfExtentU.x + (uint) hp.x]);
                w *= SsrNormalWeight(centerN, SsrOctDecode(repGb.xy));
            }
            float confidence = saturate(tap.a);
            accRgb += tap.rgb * confidence * w;
            accA += confidence * w;
            wSum += w;
        }
    }
    const float conf = (wSum > 1e-4f) ? saturate(accA / wSum) : 0.0f;
    const float3 layer = (accA > 1e-5f)
        ? accRgb / accA
        : float3(0.0f, 0.0f, 0.0f);

    // The diffuse ambient baseline (L_probe_diffuse — see the header).
    // AO ON: the AO compose's third output (already aoEff-scaled).
    // AO OFF: derived inline, aoEff = 1.
    float3 baseline = float3(0.0f, 0.0f, 0.0f);
    if (cp.ambientTextureIndex != 0xffffffffu)
    {
        Texture2D ambTex =
            textures[NonUniformResourceIndex(cp.ambientTextureIndex)];
        baseline = ambTex.Load(int3(fullPx, 0)).rgb;
    }
    else
    {
        Texture2D aoTermTex =
            textures[NonUniformResourceIndex(cp.aoTermTextureIndex)];
        Texture2D envSpecTex =
            textures[NonUniformResourceIndex(cp.envSpecTextureIndex)];
        baseline = max(
            aoTermTex.Load(int3(fullPx, 0)).rgb
                - envSpecTex.Load(int3(fullPx, 0)).rgb,
            float3(0.0f, 0.0f, 0.0f));
    }

    // The energy-conserving replacement (the header's formula).
    // kd = (1 - metal) * 0.96: the conventional dielectric F0 0.04
    // approximation (binary G-buffer metal flag — noted in the header).
    const float kd = (1.0f - metal) * 0.96f;
    const float3 localDiffuse = albedo * kd * layer;
    const float3 delta = conf * (localDiffuse - baseline); // SIGNED
    const float3 outColor = max(base + saturate(cp.intensity) * delta,
        float3(0.0f, 0.0f, 0.0f));

    // ---- Debug views (cp.debugView): replace the pixel with the
    // visualization; the composed target keeps flowing downstream.
    // GAIN note: these write RAW HDR values through the scene exposure
    // (auto-exposure holds under a debug view — see the compose
    // header), so the UNITLESS confidence channel needs the dbgGain
    // pre-scale the flat debug FS views carry (Main's dbgGain = 1/
    // exposure: physical light units put the exposure at ~0.001, and a
    // bare 0..1 lerp tonemaps to black). The scene-HDR-scale views
    // (raw/layer/baseline/contribution) ride the scene's own exposure
    // like the base does — no gain.
    if (cp.debugView != 0u)
    {
        float3 dbg = base;
        if (cp.debugView == 1u)
        {
            // Hit confidence, the COMPOSE-PATH value (the upsampled,
            // filtered confidence that actually scales the composite's
            // replacement — what the final image consumes): valid local
            // neighborhoods read bright, the smooth screen-boundary
            // loss is the expected read. The dbgGain pre-scale (1/
            // exposure, Main's flat-debug-view trick) puts conf 1.0 at
            // display white — the unitless channel would otherwise
            // tonemap to black under the physical-units exposure.
            float dbgGain = 1.0f / max(frame.exposure, 1e-6f);
            dbg = float3(conf, conf, conf) * dbgGain;
        }
        else if (cp.debugView == 2u)
        {
            // Raw trace radiance (pre-blur): noisy, recognizable
            // incoming lighting. Nearest half-res texel, no upsample —
            // the noise IS the view.
            Texture2D rawTex = textures[NonUniformResourceIndex(cp.rawTextureIndex)];
            int2 rawPx = clamp(int2(fullPx / 2), int2(0, 0),
                halfExtentI - 1);
            dbg = rawTex.Load(int3(rawPx, 0)).rgb;
        }
        else if (cp.debugView == 3u)
        {
            // The filtered layer this compose consumes (temporal slot
            // when TAA ran, else the spatial blur).
            dbg = layer;
        }
        else if (cp.debugView == 4u)
        {
            // The ambient baseline being replaced.
            dbg = baseline;
        }
        else if (cp.debugView == 5u)
        {
            // The SIGNED contribution s*(ssgi - baseline): GREEN =
            // positive (the local GI brightens the receiver over its
            // baked ambient), RED = negative (the local radiance is
            // dimmer than the ambient it replaces - the intended
            // darkening of an over-bright probe ambient), black = no
            // change. HDR scale (rides the scene exposure like the
            // base); a bare signed value would clamp the negative half
            // to black in the HDR target.
            float3 d = saturate(cp.intensity) * delta;
            float s = d.r + d.g + d.b;
            dbg = (s >= 0.0f)
                ? float3(0.0f, s, 0.0f)
                : float3(-s, 0.0f, 0.0f);
        }
        OUT.outColor = float4(SanitizeHdr(dbg), 1.0f);
        return OUT;
    }

    OUT.outColor = float4(SanitizeHdr(outColor), 1.0f);
    return OUT;
}
