#define DOF_PASS
#include "common.hlsli"
#include "dof.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords (depth .Load)
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // R8: only .r survives
};

// Must match Engine/Rendergraph/dof.h DofCocPush (48 bytes).
struct DofCocPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint depthTextureIndex;
    uint pad0;
    float focusDistance;
    float focusRange;
    float maxRadiusPx; // reserved (normalizes future half-res CoC); unpack uses push max
    float invDepthW;
    float invDepthH;
    uint pad1;
    uint pad2;
    uint pad3;
};

[[vk::push_constant]]
ConstantBuffer<DofCocPush> cp;

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D depthTex = textures[NonUniformResourceIndex(cp.depthTextureIndex)];

    int2 px = int2(IN.pos.xy);
    float d = depthTex.Load(int3(px, 0)).r;
    float2 ndcXY = float2((float(px.x) + 0.5f) * cp.invDepthW * 2.0f - 1.0f,
                          (float(px.y) + 0.5f) * cp.invDepthH * 2.0f - 1.0f);
    float dist = DofViewDist(d, ndcXY, frame.invProj);
    float packed = DofPackCoc(dist, cp.focusDistance, cp.focusRange);
    OUT.outColor = float4(packed, packed, packed, 1.0f);
    return OUT;
}
