#define SSR_PASS
#include "common.hlsli"

// Ground-truth stencil view (VULKANTOY_SSR_DUMP +
// VULKANTOY_SSR_STENCILVIEW=<src>): debug probe for the receiver-material
// stencil read path. The selected source value is written into ALL FOUR
// channels so the dump's stderr stats line reports its exact min/mean/max
// (flattened over (v,v,v,v) == v itself) — no reliance on the PNG encoder
// or the layout-fragile raw depth/stencil readback.
//
// Sources (VULKANTOY_SSR_STENCILVIEW value):
//   stencil      (1, default): uintTextures[stencilIdx].Load().r — the
//                trace's exact receiver-material read (the suspect).
//   depth        (2): uintTextures[depthIdx].Load().r — a uint-typed load
//                of the KNOWN-GOOD depth view: exercises the uintTextures[]
//                alias path in isolation (raw float bits, huge values when
//                the alias path works).
//   stencilfloat (3): textures[stencilIdx].Load().r — a float-typed load of
//                the SAME stencil view: if this reads 0/1/64/65-style
//                classification while "stencil" reads ~110 mean depth-byte
//                garbage, the uint ALIAS is at fault; if both read garbage,
//                the S8 view itself is at fault.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// All scalars (no float2 members — see the SsrBlurPush layout bug).
// Must stay 16 bytes.
struct SsrStencilViewPush
{
    uint srcSelect;     // 1 = stencil (uint), 2 = depth (uint), 3 = stencil (float)
    uint stencilTextureIndex;
    uint depthTextureIndex;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SsrStencilViewPush> sp;

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    const int2 px = int2(pos.xy);
    float v = 0.0f;
    if (sp.srcSelect == 2u && sp.depthTextureIndex != 0xffffffffu) {
        v = float(uintTextures[NonUniformResourceIndex(sp.depthTextureIndex)]
            .Load(int3(px, 0)).r);
    } else if (sp.srcSelect == 3u && sp.stencilTextureIndex != 0xffffffffu) {
        v = textures[NonUniformResourceIndex(sp.stencilTextureIndex)]
            .Load(int3(px, 0)).r;
    } else if (sp.stencilTextureIndex != 0xffffffffu) {
        // Quantized classification census: 0 = empty, 1 = STENCIL_SSR,
        // 2 = STENCIL_SSR_METAL, 3 = both, 4 = UNKNOWN (any other byte).
        // The 8-bit dump PNG then yields exact per-bucket fractions.
        const uint st = uintTextures[NonUniformResourceIndex(sp.stencilTextureIndex)]
            .Load(int3(px, 0)).r;
        v = (st == 0u) ? 0.0f
            : (st == 1u) ? 1.0f
            : (st == 64u) ? 2.0f
            : (st == 65u) ? 3.0f
            : 4.0f;
    }
    // All four channels carry v: the dump's flattened min/mean/max
    // stats line then reports v's exact distribution.
    OUT.outColor = float4(v, v, v, v);
    return OUT;
}
