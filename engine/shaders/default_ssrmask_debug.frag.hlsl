#include "common.hlsli"
#include "environment.hlsli"

// SSR receiver-mask debug view (UI combo + VULKANTOY_DEBUGVIEW=ssrmask;
// no F-key: F-keys are exhausted): which surfaces the SSR trace will
// reflect on, per material. Pairs with default.vert.hlsl (same PSIn as
// default_lit). Alpha cutout + dithered Blend discard match the lit path
// so silhouettes agree.
//
// The classification mirrors the trace's receiver gate EXACTLY
// (ssr.frag.hlsl: the thin G-buffer tap + the matte early-out), so what
// you see is what the march will shade — not an approximation:
//   black: matte (roughness >= 0.94) — the trace early-outs these pixels
//          (march skipped, output black). Includes the G-buffer clear
//          sentinel (roughness 1) and anything the prepass didn't cover.
//   green: smooth dielectric receiver (metal flag 0).
//   blue:  smooth metal receiver (metal flag 1, F0 rises toward 1).
// Blend (transparent) pixels render DIMMED: the prepass is opaque-only
// so Blend surfaces never write the G-buffer — the trace reads the
// seen-through surface behind them (pane reflections ride the diffuse
// GI read, by design). A full-bright color here would claim a receiver
// the trace never sees; the dim flag marks the exclusion instead.
//
// The combine mirrors gbuffer.frag.hlsl (NOT the lit FS): factors are
// glTF multipliers over the same metallicRoughness tap (G=roughness,
// B=metallic), roughness clamped to [0.04, 1], metal flag at 0.50.
// Layered materials resolve the same way the G-buffer writer does
// (base material only — keep in sync if that ever grows a layer loop).
//
// Debug colors travel through HDR + tonemap (hdr * exposure -> ACES) with
// a physical-units exposure (~0.001) that would crush flat colors to
// black, so the palette is pre-scaled by 1/exposure (same trick as the
// meshlet debug view and default_unlit). NO fog: debug views stay
// unfogged by discipline (see DebugVisualizationActive).

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

    int baseColorIdx = mat.tex.x;
    int mrIdx = mat.tex.y;

    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float metallicFactor = mat.params.y;
    float roughnessFactor = mat.params.z;
    float baseAlphaFactor = mat.diffuse.a;

    // Base alpha for the cutout test (matches lit: factor * texture).
    float baseAlpha = baseAlphaFactor;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint) baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseAlpha *= baseTex.Sample(samplers[0], IN.uv).a;
    }

    // Masked cutout (matches lit).
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    // Dithered transparency for Blend (matches lit).
    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, clipPos.xy))
        discard;

    // Receiver roughness/metal: the G-buffer writer's exact combine
    // (gbuffer.frag.hlsl) — factors multiplied by the
    // metallicRoughness tap, NOT the layered blend the lit FS does.
    float roughness = roughnessFactor;
    float metallic = metallicFactor;
    if (mrIdx >= 0)
    {
        uint slot = (uint) mrIdx;
        Texture2D mrTex = textures[NonUniformResourceIndex(slot)];
        float4 mrSample = mrTex.Sample(samplers[0], IN.uv);
        roughness *= mrSample.g;
        metallic *= mrSample.b;
    }
    roughness = clamp(roughness, 0.04f, 1.0f);
    const float metalFlag = (saturate(metallic) >= 0.50f) ? 1.0f : 0.0f;

    // The trace's matte early-out (ssr.frag.hlsl): roughness >= ~0.94
    // means glossFade (1-r)^2 < 1/255 — sub-quantization black. These
    // pixels never march.
    float3 pal;
    if (roughness >= 0.94f)
        pal = float3(0.0f, 0.0f, 0.0f);
    else if (metalFlag > 0.5f)
        pal = float3(0.0f, 0.25f, 1.0f);
    else
        pal = float3(0.20f, 0.90f, 0.30f);

    // Blend surfaces never write the G-buffer (opaque-only prepass):
    // dim the swatch so the view doesn't claim a receiver the trace
    // never sees (see the header).
    if (isBlendMat)
        pal *= 0.25f;

    FrameUniforms frame = pc.frameUniforms.Get();

    // Exposure compensation (flat palette -> HDR). No fog — see header.
    float exp = max(frame.exposure, 1e-6f);
    float3 color = pal / exp;

    OUT.outColor = float4(SanitizeHdr(color), (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
