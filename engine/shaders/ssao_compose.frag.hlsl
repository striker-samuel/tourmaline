// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSAO_PASS
#include "common.hlsli"
#include "ao.hlsli"
#include "hzb.hlsli"

// SSGI AO compose: joint-bilateral (depth + normal) upsample of the
// half-res blurred AO to FULL res, then the HDR AO reconstruction.
//
// AO application moved OFF the lighting shader: Main no longer samples
// AO — instead it exports everything AO is allowed to attenuate as a
// second MRT output (default_lit_body.hlsli outAoTerm, target1):
//     outColor   = direct * occ + aoTerm  (+ emissive, fog, ...)
//     outAoTerm  = (ambient + ambientSpec) * occ   [PRE-ao]
// and this pass reconstructs exactly:
//     hdr' = hdr - aoTerm * (1 - aoEff)
// The subtraction is EXACT reconstruction (the exported term == the
// visible HDR contribution AO may attenuate, after all blending up to
// here) because every factor between Main's store and this read is
// linear — occ, emissive, the fog lerp, alpha blending (target1 carries
// the SAME blend state: the RHI replicates attachment[0]'s factors to
// every attachment), and the 2x MSAA resolve (averaging commutes with
// the linear subtraction). Transparency composites its aoTerm through
// the identical equation its HDR contribution uses, so the invariant
// holds through the transparent leg of Main.
//
// PAIRED ENV-SPEC SCALING (the SSR replacement-composite contract, the
// second MRT output below): Main ALSO exports the env-specular term on
// target2 (outEnvSpec = ambientSpec * occ — the exact slice aoTerm
// holds it at). The hdr's env-spec slice after this pass = ambientSpec
// * occ * aoEff, so the export must carry the SAME aoEff for the SSR
// composite to subtract exactly what its base holds:
//     envSpec' = envSpec * aoEff   (outEnvSpec, target2)
// Sky passthrough: the export holds Main's value (envSpec carries no
// AO-sensitive... no — the aoEff applies to target2 ONLY on non-sky
// pixels; the sky early-out passes the export through UNCHANGED (the
// lit never ran there — the export is the 0 clear; aoEff on 0 is 0,
// but the passthrough keeps the contract total and branch-free).
// A 0xffffffff envSpecTextureIndex (the SSR feature off / the target
// absent) passes the input through — the compose stays usable for the
// plain AO chain without the export.
//
// Loads per full-res pixel (7, + the HDR read the pass exists to modify —
// the old chain spent 9 with a corner-texel depth guess per tap):
//   1 center depth   (HZB mip0 — the exact 1:1 depth copy, BDA scalar)
//   1 center normal  (shared full-res G-buffer oct normal; optional)
//   1 aoTerm         (Main's MRT export)
//   4 half-res taps  (ao + repDist + repNormal — one .Load each)
// The old 4x depthTex.Load(hp * 2) block-representative guesses are gone:
// the chain's own self-consistent .g distance replaces them, and the
// joint normal weight keeps AO on the surface it was computed for.

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F: HDR - aoTerm*(1-aoEff)
    // Paired env-spec export (the SSR replacement contract, see the
    // header note): envSpec * aoEff — the export at the exact depth
    // the hdr's env-spec slice holds post-AO. The pass-through variant
    // (no envSpec bound) leaves this unwritten — bound ONLY when the
    // second attachment is attached (see ssgi.h's compose wiring).
    float4 outEnvSpec : SV_Target1;
};

// Must match Engine/Rendergraph/ssgi.h SsgiAoComposePush (64 bytes):
// the envSpec field consumed the old pad0. uint64 BDAs lead (see
// SsrClassifyPush's note); scalars after.
struct SsgiAoComposePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms; // nearPlane (center dist)
    vk::BufferPointer<HzbBuffer> hzb; // MAX-HZB: mip0 center depth, mip1 reps
    uint aoTextureIndex; // half-res blurred (ao, encDist, octN)
    uint normalTextureIndex; // shared full-res oct normal (0xffffffff = depth-only)
    uint aoTermTextureIndex; // Main MRT export (pre-AO ambient term)
    uint hdrTextureIndex; // full-res HDR (the image this pass rewrites)
    uint envSpecTextureIndex; // Main MRT env-spec export (0xffffffff = pass-through)
    uint fullW; // full-res width (HZB mip0 row stride)
    uint fullH;
    uint hzbMip1Offset; // float offset of mip1 in the HZB buffer
    uint hzbMip1Stride; // mip1 row stride (floats)
    float sharpness; // depth edge weight
    float intensity; // aoEff = lerp(1, ao, intensity) (the old hot.aoIntensity)
    float maxDistance; // rep-dist decode (matches raw/blur)
};

[[vk::push_constant]]
ConstantBuffer<SsgiAoComposePush> cp;

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    float nearPlane = max(frame.nearPlane, 1e-4f);
    Texture2D aoTex = textures[NonUniformResourceIndex(cp.aoTextureIndex)];
    Texture2D aoTermTex = textures[NonUniformResourceIndex(cp.aoTermTextureIndex)];
    Texture2D hdrTex = textures[NonUniformResourceIndex(cp.hdrTextureIndex)];
    // Paired env-spec export (see the header note): read Main's export,
    // write it back at the aoEff depth the hdr's env-spec slice holds.
    // 0xffffffff = the pass-through variant (no second attachment
    // bound): the output stays unwritten (the attachment is absent).
    const bool envSpecOn = cp.envSpecTextureIndex != 0xffffffffu;

    int2 fullPx = int2(IN.pos.xy);
    // Center depth from HZB mip0 (the exact 1:1 depth copy the build pass
    // made) — no depth image attachment/sampling anywhere in this pass.
    float d = cp.hzb.Get().texels[
        (uint) fullPx.y * cp.fullW + (uint) fullPx.x];

    // Sky center: Main's lit FS never ran there, so nothing AO-attenuable
    // was exported (target1 holds the black clear) — straight passthrough
    // (the env-spec export too: the 0 clear passes through unchanged).
    if (AoIsBackground(d))
    {
        OUT.outColor = float4(hdrTex.Load(int3(fullPx, 0)).rgb, 1.0f);
        if (envSpecOn) {
            Texture2D envSpecTex =
                textures[NonUniformResourceIndex(cp.envSpecTextureIndex)];
            OUT.outEnvSpec = envSpecTex.Load(int3(fullPx, 0));
        }
        return OUT;
    }
    float centerDist = AoViewDist(d, nearPlane);

    // Center normal from the shared full-res G-buffer (written by the
    // Forward+ prepass when SSR OR AO wants it — see ssgi.h); absent
    // (MSAA frames / legacy) the joint bilateral falls back to
    // depth-only weights.
    float3 centerN = float3(0.0f, 0.0f, 1.0f);
    bool haveN = cp.normalTextureIndex != 0xffffffffu;
    if (haveN)
    {
        Texture2D normTex = textures[NonUniformResourceIndex(cp.normalTextureIndex)];
        centerN = AoOctDecode(normTex.Load(int3(fullPx, 0)).xy);
    }

    // 2x2 nearest half-res texels around this full-res pixel (same
    // addressing as the old AoUpsample), jointly weighted by the tap's
    // carried (repDist, repNormal) against the pixel's own (depth, normal).
    int2 halfExtent = max(int2((int) cp.fullW, (int) cp.fullH) / 2, int2(1, 1));
    int2 halfMin = int2(floor((float2(fullPx) - 1.0f) * 0.5f));
    int2 halfMax = halfExtent - 1;

    float aoAcc = 0.0f;
    float wSum = 0.0f;
    [unroll]
    for (int ty = 0; ty < 2; ++ty)
    {
        [unroll]
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = clamp(halfMin + int2(tx, ty), int2(0, 0), halfMax);
            float4 tap = aoTex.Load(int3(hp, 0));
            float tapDist = AoDecodeRepDist(tap.g, cp.maxDistance);
            float w = AoEdgeWeight(tapDist, centerDist, cp.sharpness);
            if (haveN)
                w *= AoNormalWeight(AoOctDecode(tap.zw), centerN);
            aoAcc += tap.r * w;
            wSum += w;
        }
    }
    float ao = (wSum > 1e-4f) ? aoAcc / wSum : 1.0f;
    // The intensity lerp moved here from the lighting shader (was
    // hot.aoIntensity) — identical formula, identical values.
    float aoEff = lerp(1.0f, saturate(ao), saturate(cp.intensity));

    float3 hdr = hdrTex.Load(int3(fullPx, 0)).rgb;
    float3 term = aoTermTex.Load(int3(fullPx, 0)).rgb;
    // Exact reconstruction: hdr == direct*occ + term + emissive/fog/...
    // => hdr - term*(1-aoEff) == direct*occ + term*aoEff + those (all
    // linear in between). Sanitize: a silhouette-weight mismatch (the
    // joint-bilateral aoEff vs the ao the export assumed — they agree to
    // fp noise on static scenes) could push a dark tap minutely negative.
    float3 outC = hdr - term * (1.0f - aoEff);
    OUT.outColor = float4(SanitizeHdr(outC), 1.0f);
    // Paired env-spec scaling (the SSR replacement contract, see the header
    // note): the hdr's env-spec slice now holds envSpec * aoEff, so the
    // export writes the SAME aoEff — the SSR composite then subtracts
    // exactly what its base holds.
    if (envSpecOn) {
        Texture2D envSpecTex =
            textures[NonUniformResourceIndex(cp.envSpecTextureIndex)];
        OUT.outEnvSpec = float4(
            envSpecTex.Load(int3(fullPx, 0)).rgb * aoEff, 1.0f);
    }
    return OUT;
}
