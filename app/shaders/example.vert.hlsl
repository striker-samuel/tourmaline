// =============================================
// HLSL (for Vulkan via DXC) port of your GLSL
// =============================================

#ifndef MAX_INSTANCES
#define MAX_INSTANCES 1000
#endif

// --- Push constants (GLSL: layout(push_constant) uniform PushConsts { ... } pc; )
struct PushConsts
{
    float4x4 view;
    float4x4 proj;
    int material_index;
    int padding0;
    int padding1;
    int padding2;
};

// Tell DXC this ConstantBuffer is a Vulkan push-constant block.
[[vk::push_constant]]
ConstantBuffer<PushConsts> pc;

// std140 array element for a scalar: 16 bytes
struct IndexStd140
{
    uint i;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

cbuffer IndexUBO : register(b0, space0)
{
    IndexStd140 indicesStd140[MAX_INSTANCES];
}

// This can easily get outdated. Be careful.
struct InstanceData
{
    float4x4 transform;
    float4 color;
};

// Explicit Vulkan binding (set=0, binding=1)
[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> instance_data;

// --- Vertex I/O
// GLSL: layout(location=0/1/2) in ... ; and location 0/1 out ...
struct VSIn
{
    float3 inPosition : POSITION; // location 0
    float3 inColor : COLOR0; // location 1
    float2 inTexCoord : TEXCOORD0; // location 2
    uint instanceID : SV_InstanceID; // gl_InstanceIndex
};



struct VSOut
{
    float4 pos : SV_Position; // gl_Position
    float3 fragColor : COLOR0; // layout(location=0) out
    float2 fragTexCoord : TEXCOORD0; // layout(location=1) out
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;

    // Find which instance's model matrix to use:
    // GLSL: instance.model[inn.indices[gl_InstanceIndex]]
    uint idx = indicesStd140[IN.instanceID].i;

    // Position transform:
    // GLSL: pc.proj * pc.view * M * vec4(inPosition, 1.0)
    float4 world = float4(IN.inPosition, 1.0f);
    float4x4 model = instance_data[idx].transform;
    float4 clip = mul(pc.proj, mul(pc.view, mul(model, world)));

    OUT.pos = clip;
    OUT.fragColor = instance_data[idx].color;
    //OUT.fragColor = IN.inColor;
    OUT.fragTexCoord = IN.inTexCoord;
    return OUT;
}
