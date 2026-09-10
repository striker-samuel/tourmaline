#include "common.hlsli"

// Alpha-tested shadow depth for cutout (MASK) materials.
//
// The cascaded shadow pass has no color attachment; this shader only
// discards fragments whose baseColor alpha is under the material cutoff,
// so foliage casts leaf-shaped shadows instead of full quads. Lighting,
// normal mapping etc. are intentionally skipped — depth comes from SV_Position.
// Opaque casters keep using the depth-only (VS-only) program.

struct PSIn
{
    float4 clipPos : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
};

void mainPS(PSIn IN)
{
    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    float alphaCutoff = mat.params.x;
    float baseAlpha = mat.diffuse.a;

    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        float4 s = textures[NonUniformResourceIndex((uint)baseColorIdx)]
            .Sample(samplers[0], IN.uv);
        baseAlpha *= s.a;
    }

    // Dithered Blend (alphaMode == 2, dithered frame): screen-door discard
    // so the depth prepass stores dithered depth for transparent materials.
    // Masked and blended-Blend keep the classic alpha-cutout test.
    float alphaModeF = mat.params2.w;
    if (alphaModeF == 2.0f && pc.pad_pc == 0)
    {
        if (!DitherKeep(baseAlpha, IN.clipPos.xy))
            discard;
    }
    else if (baseAlpha < alphaCutoff)
        discard;
}
