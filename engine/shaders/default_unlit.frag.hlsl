#include "common.hlsli"
#include "environment.hlsli"

// Unlit debug view (F2): baseColor + emissive only, no lighting/shadows.
// Pairs with default.vert.hlsl (same PSIn as default_lit). Alpha cutout +
// dithered Blend discard match the lit path so silhouettes agree.
//
// Debug colors travel through HDR + tonemap (hdr * exposure -> ACES) with a
// physical-units exposure (~0.001) that would crush albedo to black, so the
// output is pre-scaled by 1/exposure (same trick as the meshlet debug view).
// Fog still applies in HDR so depth cues survive.

struct PSIn
{
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
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

    // Layered materials (same convention as default_lit): vertex color
    // RGBA weights blend up to 4 layers; layer 0 is this material.
    int layerMode = mat.layerMaterials.w;
    const bool layered = (layerMode > 1);
    float4 layerW = layered
        ? IN.color / max(IN.color.x + IN.color.y + IN.color.z + IN.color.w, 1e-6f)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);
    MaterialValues ml0 = mat;
    MaterialValues ml1 = mat;
    MaterialValues ml2 = mat;
    MaterialValues ml3 = mat;
    if (layered)
    {
        int id1 = mat.layerMaterials.x;
        int id2 = mat.layerMaterials.y;
        int id3 = mat.layerMaterials.z;
        if (layerMode > 1 && id1 >= 0 && id1 < MAX_MATERIALS)
            ml1 = pc.bMats.Get().mats[id1];
        if (layerMode > 2 && id2 >= 0 && id2 < MAX_MATERIALS)
            ml2 = pc.bMats.Get().mats[id2];
        if (layerMode > 3 && id3 >= 0 && id3 < MAX_MATERIALS)
            ml3 = pc.bMats.Get().mats[id3];
    }

    int baseColorIdx = mat.tex.x;
    int emissiveIdx = mat.tex2.x;

    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float emissiveStrength = layered
        ? ml0.params2.z * layerW.x + ml1.params2.z * layerW.y
        + ml2.params2.z * layerW.z + ml3.params2.z * layerW.w
        : mat.params2.z;

    float3 baseColorFactor = layered
        ? ml0.diffuse.rgb * layerW.x + ml1.diffuse.rgb * layerW.y
        + ml2.diffuse.rgb * layerW.z + ml3.diffuse.rgb * layerW.w
        : mat.diffuse.rgb;
    float baseAlphaFactor = layered
        ? ml0.diffuse.a * layerW.x + ml1.diffuse.a * layerW.y
        + ml2.diffuse.a * layerW.z + ml3.diffuse.a * layerW.w
        : mat.diffuse.a;
    float3 emissiveFactor = layered
        ? ml0.emissiveFactor.rgb * layerW.x + ml1.emissiveFactor.rgb * layerW.y
        + ml2.emissiveFactor.rgb * layerW.z + ml3.emissiveFactor.rgb * layerW.w
        : mat.emissiveFactor.rgb;

    float4 baseSample = float4(1, 1, 1, 1);
    if (!layered && baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseSample = baseTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, clipPos.xy, pc.textureFeedbackEnabled != 0u);
    }
    if (layered)
    {
        baseSample = float4(0, 0, 0, 0);
        int4 bcIdx = int4(ml0.tex.x, ml1.tex.x, ml2.tex.x, ml3.tex.x);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? bcIdx.x : (li == 1) ? bcIdx.y : (li == 2) ? bcIdx.z : bcIdx.w;
            if (w < 0.004f)
                continue;
            float4 s = float4(1, 1, 1, 1);
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D t = textures[NonUniformResourceIndex(slot)];
                s = t.Sample(samplers[0], IN.uv);
                TS_ReportUsage(slot, t, samplers[0], IN.uv, clipPos.xy, pc.textureFeedbackEnabled != 0u);
            }
            baseSample += s * w;
        }
    }
    float3 vcolTint = layered ? float3(1.0f, 1.0f, 1.0f) : IN.color.rgb;
    float3 baseColor = baseColorFactor * vcolTint * baseSample.rgb;
    float baseAlpha = baseAlphaFactor * baseSample.a;

    // Masked cutout (matches lit).
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    // Dithered transparency for Blend (matches lit).
    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, clipPos.xy))
        discard;

    float3 emissive = emissiveFactor * emissiveStrength;
    if (!layered && emissiveIdx >= 0)
    {
        uint slot = (uint)emissiveIdx;
        Texture2D eTex = textures[NonUniformResourceIndex(slot)];
        float3 eSample = eTex.Sample(samplers[0], IN.uv).rgb;
        TS_ReportUsage(slot, eTex, samplers[0], IN.uv, clipPos.xy, pc.textureFeedbackEnabled != 0u);
        emissive *= eSample;
    }
    if (layered)
    {
        float3 eAcc = float3(0.0f, 0.0f, 0.0f);
        int4 eIdx = int4(ml0.tex2.x, ml1.tex2.x, ml2.tex2.x, ml3.tex2.x);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? eIdx.x : (li == 1) ? eIdx.y : (li == 2) ? eIdx.z : eIdx.w;
            if (w < 0.004f)
                continue;
            float3 s = float3(1.0f, 1.0f, 1.0f);
            if (idx >= 0)
            {
                uint slot = (uint)idx;
                Texture2D eTex = textures[NonUniformResourceIndex(slot)];
                s = eTex.Sample(samplers[0], IN.uv).rgb;
                TS_ReportUsage(slot, eTex, samplers[0], IN.uv, clipPos.xy, pc.textureFeedbackEnabled != 0u);
            }
            eAcc += s * w;
        }
        emissive *= eAcc;
    }

    float3 color = baseColor + emissive;

    FrameUniforms frame = pc.frameUniforms.Get();

    // Exposure compensation first (albedo -> HDR), then fog in HDR with the
    // same fog color as the lit view so depth cues match across F1/F2.
    float exp = max(frame.exposure, 1e-6f);
    color /= exp;

    float viewDist = length(frame.cameraPos - IN.worldPos);
    float fogF = ComputeFogFactor(viewDist, IN.worldPos.y, frame.env);
    if (fogF > 0.0f)
    {
        float3 fogHdr = frame.env.fogColor.rgb * max(frame.env.fogColor.w, 0.0f);
        color = lerp(color, fogHdr, fogF);
    }

    OUT.outColor = float4(SanitizeHdr(color), (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
