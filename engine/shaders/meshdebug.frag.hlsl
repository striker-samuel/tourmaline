// Meshlet debug view fragment: flat push color, no lighting/textures.
// C++ mirror: app::MeshDebugPush (144 bytes).
struct MeshDebugPush
{
    float4x4 model;
    float4x4 viewProj;
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<MeshDebugPush> pcD;

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
     PSOut OUT;
     // Gamma encode so debug colors survive the tonemap pass's
     // gamma decode (pow(..., 1/2.2)) — otherwise they appear too dark.
     OUT.outColor = pow(IN.color, 1.0f / 2.2f);
     return OUT;
}
