// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"
#include "material_coverage.hlsli"

// Motion vector FS: per-pixel UV delta (prevUV - currUV) into an RG16F
// target. MASK (alpha-tested) cutouts and dithered-Blend screen-door use
// the SHARED coverage keep-test (TM_CoverageKeep, material_coverage.hlsli)
// so foliage gets leaf-shaped motion and the kept samples match the color
// pass exactly (vertex-color alpha + layered blending included). Opaque
// materials skip the texture fetch.

struct PSIn
{
    float4 pos : SV_Position;
    float4 prevClip : PREVCLIP;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct PSOut
{
    float2 motion : SV_Target0;
    // NOTE: no SV_Depth output — the motion pass tests read-only against
    // prepass depth (writeEnable=false), so a DepthReplacing flag would
    // buy nothing and risks the post-shader depth path on some drivers.
    // Drivers already early-test demote-discard.
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // Single shared coverage keep-test (TM_CoverageKeep) — the SAME
    // evaluator the lit body, depth prepass and G-buffer use, so the motion
    // pass keeps exactly the samples the color pass kept. The previous
    // inline copy had drifted (dropped vertex-color alpha and ignored
    // layered-material blending), so cutout geometry could keep pixels the
    // color pass discarded — texture-shaped holes in the motion buffer.
    // Opaque materials skip the coverage machinery entirely (no texture
    // fetch), exactly as before; only Masked / dithered-Blend need it.
    const bool needsCoverage =
        (mat.params.w == 2.0f) ||                          // ShadingModel::Masked
        (mat.params2.w == 2.0f && pc.pad_pc == 0);         // dithered AlphaMode::Blend
    if (needsCoverage)
    {
        const int layerMode = mat.layerMaterials.w;
        const bool layered = (layerMode > 1);
        // Layered: vertex-color RGBA are layer weights (normalized below),
        // so coverage alpha comes from the per-layer factor/texel blend and
        // the vertex-color-alpha factor is 1. Non-layered: vertex color.a
        // is part of coverage (seam ribbons).
        float4 layerW = layered
            ? IN.color / max(IN.color.x + IN.color.y + IN.color.z + IN.color.w, 1e-6f)
            : float4(1.0f, 0.0f, 0.0f, 0.0f);
        MaterialValues ml0 = mat;
        MaterialValues ml1 = mat;
        MaterialValues ml2 = mat;
        MaterialValues ml3 = mat;
        if (layered)
        {
            const int id1 = mat.layerMaterials.x;
            const int id2 = mat.layerMaterials.y;
            const int id3 = mat.layerMaterials.z;
            if (id1 >= 0 && id1 < MAX_MATERIALS) ml1 = pc.bMats.Get().mats[id1];
            if (id2 >= 0 && id2 < MAX_MATERIALS) ml2 = pc.bMats.Get().mats[id2];
            if (id3 >= 0 && id3 < MAX_MATERIALS) ml3 = pc.bMats.Get().mats[id3];
        }
        const float baseAlphaFactor = layered
            ? ml0.diffuse.a * layerW.x + ml1.diffuse.a * layerW.y
            + ml2.diffuse.a * layerW.z + ml3.diffuse.a * layerW.w
            : mat.diffuse.a;
        float4 baseSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
        if (layered)
        {
            baseSample = float4(0.0f, 0.0f, 0.0f, 0.0f);
            const int4 bcIdx = int4(ml0.tex.x, ml1.tex.x, ml2.tex.x, ml3.tex.x);
            [unroll]
            for (int li = 0; li < 4; ++li)
            {
                const float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
                if (w < 0.004f)
                    continue;
                float4 s = float4(1.0f, 1.0f, 1.0f, 1.0f);
                const int idx = bcIdx[li];
                if (idx >= 0)
                {
                    Texture2D t = textures[NonUniformResourceIndex((uint)idx)];
                    s = t.Sample(samplers[0], IN.uv);
                }
                baseSample += s * w;
            }
        }
        else if (mat.tex.x >= 0)
        {
            Texture2D baseTex = textures[NonUniformResourceIndex((uint)mat.tex.x)];
            baseSample = baseTex.Sample(samplers[0], IN.uv);
        }
        const float coverageAlpha =
            TM_CoverageAlpha(baseAlphaFactor, baseSample.a, layered ? 1.0f : IN.color.a);
        if (!TM_CoverageKeep(coverageAlpha, mat.params.x, mat.params.w, mat.params2.w,
                             (pc.pad_pc == 0), IN.pos.xy))
            discard;
    }

    FrameUniforms frame = pc.frameUniforms.Get();

    // SV_Position is post-divide pixel coords; same NDC->UV convention as
    // the Y-flipped proj both frames share.
    float2 currUV = IN.pos.xy * frame.invViewportSize;
    float2 prevUV = (IN.prevClip.xy / IN.prevClip.w) * 0.5 + 0.5;
    float2 motion = prevUV - currUV;

    // Guard: geometry behind the previous camera, first frame or camera
    // reset produces huge reprojected deltas – treat them as "no history".
    if (dot(motion, motion) > 1.0)
        motion = float2(0.0, 0.0);

    OUT.motion = motion;
    return OUT;
}
