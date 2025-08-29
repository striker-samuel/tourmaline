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

// In GLSL you had a combined sampler2D array at binding=2.
// In HLSL, textures and samplers are separate objects.
// Map it to a Texture2D array + a sampler.
Texture2D gMaterials[MAX_MATERIALS] : register(t2, space0); // set=0, binding=2
SamplerState gSampler : register(s0, space0); // choose a set/binding you use for your sampler

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
    OUT.outColor = gMaterials[idx].Sample(gSampler, IN.fragTexCoord);

    return OUT;
}
