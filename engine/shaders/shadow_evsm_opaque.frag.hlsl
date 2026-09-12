#include "common.hlsli"
#include "evsm.hlsli"

// EVSM moment pass — OPAQUE-caster leg (paired with shadow_evsm.frag.hlsl,
// the alpha-tested MASK leg). Opaque geometry needs no alpha test, so this
// shader skips the material read, the base-color fetch and the
// TS_ReportUsage atomic entirely: per-fragment cost is two exp() and the
// RG32F store (~the PCF depth-only pass cost). Binding the full material
// path for opaque casters measured ~30x PCF (texture sample + global
// atomic at 2xMSAA rates over a 2048^2 tile).
//
// Vertex shader: shadow.vert.hlsl mainVS (same lightVP convention as the
// depth-only path). Runs with a 2xMSAA RG32F color target + 2xMSAA D32
// depth; the resolve averages the warped moments. Exponent: the shared
// EVSM_EXPONENT define (NOT a spec constant — see shadow_evsm.frag.hlsl).

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
    // SV_Position.z is window-space depth in [0,1] (Vulkan ZO) — the
    // light-space depth d the EVSM warp expects.
    PSOut OUT;
    OUT.moments = EvsmWarp(saturate(IN.clipPos.z), EVSM_EXPONENT);
    return OUT;
}
