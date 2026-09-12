#include "common.hlsli"
#include "evsm.hlsli"

// EVSM moment pass (directional CSM + spot tiles share it).
//
// Vertex shader: shadow.vert.hlsl mainVS (same lightVP convention as the
// depth-only path). This FS runs with a 2xMSAA RG32F color target + 2xMSAA
// D32 depth (depth test on, depth write on). Each sample writes the warped
// moments; the MSAA resolve averages them — real 2x supersampling of the
// depth signal, then the async compute prefilter blurs the resolved image.
//
// Push constants: same scene-wide `pc` as shadow.frag (material lookup for
// the alpha-tested cutout path). EVSM exponent comes from the C++ EvsmDesc
// via a specialization-constant override below; default matches
// smvk::kEvsmExponent so a missing spec still renders correctly.
//
// Alpha-tested (MASK) casters discard under the cutoff like shadow.frag;
// opaque casters write moments unconditionally.

[[vk::constant_id(0)]] const float EVSM_C = 42.0f;

struct PSIn
{
    float4 clipPos : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float2 moments : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    float alphaCutoff = mat.params.x;
    float baseAlpha = mat.diffuse.a;

    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        float4 s = baseTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
        baseAlpha *= s.a;
    }

    float alphaModeF = mat.params2.w;
    if (alphaModeF == 2.0f && pc.pad_pc == 0)
    {
        if (!DitherKeep(baseAlpha, IN.clipPos.xy))
            discard;
    }
    else if (baseAlpha < alphaCutoff)
        discard;

    // SV_Position.z is window-space depth in [0,1] (Vulkan ZO) — exactly the
    // light-space depth d the EVSM warp expects. No further transform.
    float d = saturate(IN.clipPos.z);

    PSOut OUT;
    OUT.moments = EvsmWarp(d, EVSM_C);
    return OUT;
}
