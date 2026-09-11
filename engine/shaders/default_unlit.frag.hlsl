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
    float4 clipPos : SV_Position;
    float3 color : COLOR0;
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

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    int baseColorIdx = mat.tex.x;
    int emissiveIdx = mat.tex2.x;

    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float emissiveStrength = mat.params2.z;

    float3 baseColorFactor = mat.diffuse.rgb;
    float baseAlphaFactor = mat.diffuse.a;
    float3 emissiveFactor = mat.emissiveFactor.rgb;

    float4 baseSample = float4(1, 1, 1, 1);
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseSample = baseTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
    }
    float3 baseColor = baseColorFactor * IN.color * baseSample.rgb;
    float baseAlpha = baseAlphaFactor * baseSample.a;

    // Masked cutout (matches lit).
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    // Dithered transparency for Blend (matches lit).
    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, IN.clipPos.xy))
        discard;

    float3 emissive = emissiveFactor * emissiveStrength;
    if (emissiveIdx >= 0)
    {
        uint slot = (uint)emissiveIdx;
        Texture2D eTex = textures[NonUniformResourceIndex(slot)];
        float3 eSample = eTex.Sample(samplers[0], IN.uv).rgb;
        TS_ReportUsage(slot, eTex, samplers[0], IN.uv, IN.clipPos.xy);
        emissive *= eSample;
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

    OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
