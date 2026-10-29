// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"

// World-space MAPPED normals debug view (combo/env only - see
// ViewMode::NormalsMapped): the SHADING normal - the interpolated
// vertex normal perturbed by the material's normal map - as false
// color, (n * 0.5 + 0.5) -> RGB. The sibling Normals view
// (default_normals_debug) shows the GEOMETRIC normal only ("no
// normal-map perturbation - the POM view owns mapped normals"); this
// view takes that role over for the non-POM case: the plain
// tangent-space fetch, verbatim default_lit_body.hlsli math (guarded
// Gram-Schmidt TBN, sanitized handedness, two-channel cooked maps
// with Z reconstruction, glTF normalScale). NO POM marching - the
// parallax-displaced normal stays the POM view's own readout.
//
// Layered materials fall back to the GEOMETRIC normal on purpose: the
// per-layer weight-blended TBN blend is the lit body's domain (the
// layer weights ride the terrain path's own VS machinery); a partial
// approximation here would read as a bug, an honest flat normal reads
// as "no map applied".
//
// Family contract: same PSIn as default_normals_debug over
// default.vert (direct fallback leg) and gfxfiltered.vert (GPU-driven
// leg) - see AppResources::debugPrograms. Debug colors travel through
// HDR + tonemap with a physical-units exposure (~0.001), so the output
// is pre-scaled by 1/exposure (the family trick). NO fog (flat
// readouts stay unfogged by discipline).
//
// Unlike the geometric sibling this FS READS the material table
// (pc.bMats + pc.materialId, the gbuffer.frag pattern) + the uv +
// tangent VS outputs.
//
// MASK cutout + closeup dither mirror default_unlit.frag.hlsl (and the
// geometric sibling's matching block): masked surfaces must keep their
// authored silhouette in the readout or their transparent texels draw
// as solid false color - flicker the lit/unlit paths discard.

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    // ALL inputs carry EXPLICIT locations (DXC rejects partial explicit
    // assignment; see default_unlit.frag.hlsl's note). The locations the
    // FS does not consume (lightmapUv 5 / lmChunk 6) stay undeclared -
    // decorations of the declared set must match the VS exactly.
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];
    const bool layered = (mat.layerMaterials.w > 1);
    int normalIdx = mat.tex.z;
    float normalScale = mat.params2.y;

    // Match the lit path's closeup dissolve (default_unlit's block,
    // verbatim): a flagged subtree must screen-door away here too or it
    // renders SOLID in this view while it dissolves in lit.
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameEarly = pc.frameUniforms.Get();
        FramePSHot hotEarly = frameEarly.frameHot.Get();

        if (!CloseupDitherKeep(
                IN.worldPos, hotEarly.cameraPos, clipPos.xy))
            discard;
    }

    // Single-layer MASK material (matches lit/unlit): discard below the
    // alpha cutoff so masked cutouts keep their authored silhouette
    // instead of drawing transparent texels as false color. Layered
    // materials keep the lit body's weight-blended alpha as its own
    // domain (same convention as the mapped-normal fallback below).
    if (mat.params.w == 2.0f)
    {
        float alpha = mat.diffuse.a;
        int baseColorIdx = mat.tex.x;

        if (baseColorIdx >= 0)
        {
            alpha *= textures[
                NonUniformResourceIndex((uint)baseColorIdx)]
                .Sample(samplers[0], IN.uv).a;
        }

        if (alpha < mat.params.x)
            discard;
    }

    float3 N = normalize(IN.normal);
    // Degenerate-interpolation guard (the lit body's bistro-glass
    // lesson): a near-zero interpolated normal underflows normalize()
    // to NaN and would NaN the TBN below; the dot test rejects NaN and
    // non-unit garbage alike.
    if (!(dot(N, N) >= 0.5f))
        N = float3(0.0f, 1.0f, 0.0f);
    // Double-sided handling (masked phases bind material-following
    // cull, back faces reach here carrying away-pointing normals - the
    // lit family's flip, so the mapped frame is built on the
    // viewer-facing normal).
    if (!IN.isFrontFace)
        N = -N;

    // The mapped perturbation: NON-layered materials with a normal
    // texture (the lit body's !layered branch, verbatim). Layered and
    // mapless materials keep the geometric normal (see the header).
    if (!layered && normalIdx >= 0)
    {
        // TBN from interpolated vertex data: Gram-Schmidt
        // re-orthogonalize the tangent against the normal, guarded
        // against the degenerate (T near-parallel N, mirrored UV
        // seams interpolating through zero).
        float3 T = IN.tangent.xyz;
        float3 Tg = T - N * dot(N, T);
        float tgLen = length(Tg);
        if (tgLen > 1e-6f)
        {
            T = Tg / tgLen;
        }
        else
        {
            float3 axis = abs(N.y) < 0.99f
                ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            T = normalize(cross(axis, N));
        }

        // tangent.w = handedness (bitangent sign); sanitize 0/NaN to +1
        // (the importer's Lengyel convention default).
        float tw = IN.tangent.w;
        tw = (tw == 0.0f || isnan(tw)) ? 1.0f : sign(tw);
        float3 B = cross(N, T) * tw;

        // Two-channel cooked normal maps (toktx --normal_mode, UASTC
        // RRRG): reconstruct Z on the unit sphere - sampling the
        // transcoded B=0 as Z would flip every normal into the
        // surface (0*2-1 = -1).
        float2 mapXY = textures[NonUniformResourceIndex((uint) normalIdx)]
            .Sample(samplers[0], IN.uv).xy * 2.0f - 1.0f;
        mapXY *= normalScale;
        float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
        float3 mapN = normalize(float3(mapXY, mapZ));

        // Tangent-space -> world-space; never normalize blind.
        float3 Nc = T * mapN.x + B * mapN.y + N * mapN.z;
        float ncLen = length(Nc);
        N = ncLen > 1e-12f ? Nc / ncLen : N;
    }

    float3 color = N * 0.5f + 0.5f;

    // Exposure compensation (readout -> HDR): pre-scale by 1/exposure
    // so the 0..1 false color survives the hdr * exposure -> ACES path.
    FrameUniforms frame = pc.frameUniforms.Get();
    color /= max(frame.exposure, 1e-6f);

    OUT.outColor = float4(SanitizeHdr(color), 1.0f);
    return OUT;
}
