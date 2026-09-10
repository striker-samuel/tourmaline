#define SSAO_PASS
#include "common.hlsli"
#include "ao.hlsli"

// SSAO compose: depth-guided bilateral upsample of the half-res blurred AO
// to FULL resolution (see ao.hlsli AoUpsample). Runs in its own pass so the
// full-res depth is only ever a barriered sampled input here; consumers
// (opaque ambient in default_lit.frag) then plain .Load the composed R8 —
// passes that ATTACH depth must never also sample it (layout clash = UB).

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // R8: only .r survives
};

// Must match Engine/Rendergraph/ssao.h SsaoComposePush (32 bytes).
struct SsaoComposePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms; // nearPlane
    uint aoTextureIndex; // half-res blurred AO
    uint depthTextureIndex; // full-res depth (authority)
    float sharpness; // upsample edge weight
    float invDepthW; // 1 / full-res depth width
    float invDepthH; // 1 / full-res depth height
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsaoComposePush> cp;

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D aoTex = textures[NonUniformResourceIndex(cp.aoTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(cp.depthTextureIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    // Half-res AO extent derived from the full-res depth extent
    // (max(1, floor(n / 2)), same as Ssao::HalfExtentFor).
    float2 halfExtent = max(
        floor(float2(1.0f / cp.invDepthW, 1.0f / cp.invDepthH) * 0.5f),
        float2(1.0f, 1.0f));

    // Background full-res depth -> AO 1.0 (the invariant); geometry gets
    // the depth-weighted 2x2 half-res taps (see ao.hlsli).
    float ao = AoUpsample(aoTex, depthTex, int2(IN.pos.xy),
        halfExtent, nearPlane, cp.sharpness);
    OUT.outColor = float4(ao, ao, ao, 1.0f);
    return OUT;
}
