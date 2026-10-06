// Chrome-ball visualization of the loaded cubemap probes (see
// Rendergraph/probeball.h): a DEDICATED pass — no scene pc, its own push
// block (ProbeBallPush, mirrored by the C++ struct in probeball.h).
// VSIn: the terrain FULL format's attributes 0 + 1 only (POSITION +
// NORMAL — the tangent/color attributes are bound but undeclared, so
// they are simply not fetched).
#define PROBEBALL_PASS
#include "common.hlsli"

// Mirrors smvk::probeball::ProbeBallPush (C++, probeball.h) exactly.
struct ProbeBallPush
{
    float4x4 viewProj; // 0  the rendering view's VP (reversed-Z, Y-flipped)
    float4 eye;         // 64 xyz camera, w unused
    float4 xform;       // 80 xyz ball center, w radius
    float4 probe;       // 96 x cubemap index (float, cast), y mips,
                        //    z intensity, w sampler slot
};

[[vk::push_constant]]
ConstantBuffer<ProbeBallPush> pb;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 normal : NORMAL;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    const float radius = max(pb.xform.w, 1e-3);
    OUT.worldPos = pb.xform.xyz + IN.position * radius;
    OUT.pos = mul(pb.viewProj, float4(OUT.worldPos, 1.0));
    // Uniform scaling (radius) keeps normals valid: scale * n.
    OUT.normal = normalize(IN.normal.xyz);
    return OUT;
}
