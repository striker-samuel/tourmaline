// =============================================
// HLSL (for Vulkan via DXC) port of your GLSL FS
// =============================================
#ifndef MAX_MATERIALS
#define MAX_MATERIALS 100
#endif

// Push constants (same layout/size as VS)
struct PushConsts
{
    float4x4 view;
    float4x4 proj;
    int material_index;
    int _pad0;
    int _pad1;
    int _pad2;
};

[[vk::push_constant]]
ConstantBuffer<PushConsts> pc;

[[vk::binding(2,0)]] Texture2D materialTex[MAX_MATERIALS];
[[vk::binding(3,0)]] SamplerState materialSamp[MAX_MATERIALS];

struct PSIn
{
    float3 fragColor : COLOR0; // layout(location=0) in
    float2 fragTexCoord : TEXCOORD0; // layout(location=1) in
};

struct PSOut
{
    float4 outColor : SV_Target; // layout(location=0) out
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    // pc.material_index is uniform (push constant), so no NonUniformResourceIndex needed.
    uint idx = (uint) pc.material_index;
    OUT.outColor = float4(IN.fragColor.x, IN.fragColor.y, IN.fragColor.z, 1.0f);
    //OUT.outColor = materialTex[idx].Sample(materialSamp[idx], IN.fragTexCoord);

    return OUT;
}
