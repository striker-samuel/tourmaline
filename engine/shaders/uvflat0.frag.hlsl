// Flat UV (diffuse) fragment: sample the material's baseColor texture
// at the unwrapped uv, tinted by the baseColor factor — the "what does
// the diffuse layout look like over its texture" view. Alpha cutout +
// dithered Blend discard match the lit path so shells agree.
#include "common.hlsli"

struct PSIn
{
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    float2 lightmapUv : TEXCOORD3;
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

    float3 c = mat.diffuse.rgb;
    float alpha = mat.diffuse.a;
    const int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        const uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        const float4 t = baseTex.Sample(samplers[0], IN.uv);
        c *= t.rgb;
        alpha *= t.a;
    }

    // Alpha cutout so holes match the lit view.
    const float alphaCutoff = mat.params.x;
    const float shadingModelF = mat.params.w;
    if (shadingModelF == 2.0f && alpha < alphaCutoff)
        discard;

    const float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(alpha, clipPos.xy))
        discard;

    OUT.outColor = float4(SanitizeHdr(c), 1.0);
    return OUT;
}
