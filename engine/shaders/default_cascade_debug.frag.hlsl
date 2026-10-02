#include "light.hlsli"
#include "common.hlsli"

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

// Shadow cascade debug view (F4): flat color per cascade, brightness tracks
// the shadow factor so shadowed vs lit reads in one glance.
//   cascade 0 = red, 1 = green, 2 = blue, 3 = yellow, beyond = magenta.
// Pairs with default.vert.hlsl (same PSIn as default_lit). Alpha cutout +
// dithered Blend discard match the lit path so silhouettes agree.
//
// Like the other debug views the output is pre-scaled by 1/exposure: it
// travels through HDR + tonemap (hdr * exposure -> ACES) and a raw 0..1
// color would crush to black under physical-units exposure. No fog on
// purpose — this view is about cascade coverage, not depth cues.

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

float3 CascadeDebugColor(int cascade)
{
    if (cascade == 0) return float3(1.0f, 0.15f, 0.15f);
    if (cascade == 1) return float3(0.15f, 1.0f, 0.15f);
    if (cascade == 2) return float3(0.25f, 0.45f, 1.0f);
    if (cascade == 3) return float3(1.0f, 0.9f, 0.15f);
    return float3(1.0f, 0.15f, 1.0f); // beyond / unshadowed
}

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // Alpha cutout so holes match the lit view (color itself is debug).
    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float baseAlpha = mat.diffuse.a;
    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseAlpha *= baseTex.Sample(samplers[0], IN.uv).a;
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, clipPos.xy, pc.textureFeedbackEnabled != 0u);
    }
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, clipPos.xy))
        discard;

    FrameUniforms frame = pc.frameUniforms.Get();
    Light light = pc.bLights.Get().lights[0];
    GpuDirectionalLightDescription dir = pc.cascadeBuffer.Get().dir;

    float3 color;
    if (light.shadow_map_index < 0 || dir.numCascades <= 0 || pc.numCascades == 0)
    {
        color = float3(0.5f, 0.5f, 0.5f); // no cascades: neutral gray
    }
    else
    {
        float3 viewPos = mul(frame.view, float4(IN.worldPos, 1.0f)).xyz;
        float viewDepth = max(-viewPos.z, 0.0f);
        int cascade = SelectCascadeGpu(viewDepth, dir.cascades, dir.numCascades);

        float3 faceN = normalize(IN.normal);
        if (!IN.isFrontFace)
            faceN = -faceN;
        // PCF depth atlas uses SampleLevel(samplers[]) + manual compare:
        // regular sampler 0, never the comparison sampler 1.
        float shadow = GetShadowFactorCascadedFromPC(0, IN.worldPos, faceN, light);

        color = CascadeDebugColor(cascade) * (0.3f + 0.7f * shadow);
    }

    float exp = max(frame.exposure, 1e-6f);
    color /= exp;

    OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
