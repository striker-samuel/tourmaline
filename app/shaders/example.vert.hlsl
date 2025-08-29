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

// --- Index UBO (GLSL: layout(set=0, binding=0, std140) uniform IndexUBO { uint indices[MAX_INSTANCES]; } inn; )
cbuffer IndexUBO : register(b0, space0)
{
    uint indices[MAX_INSTANCES];
};

// --- Instance data buffer with model matrices
// GLSL SSBO (read-only in your shader) -> HLSL StructuredBuffer
// GLSL: layout(set=0, binding=1, std140) buffer InstanceData { mat4 model[MAX_INSTANCES]; } instance;
StructuredBuffer<float4x4> instanceModel : register(t1, space0);

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
    uint idx = indices[IN.instanceID];
    float4x4 M = instanceModel[idx];

    // Position transform:
    // GLSL: pc.proj * pc.view * M * vec4(inPosition, 1.0)
    float4 world = float4(IN.inPosition, 1.0f);
    float4 clip = mul(pc.proj, mul(pc.view, mul(M, world)));

    OUT.pos = clip;
    OUT.fragColor = IN.inColor;
    OUT.fragTexCoord = IN.inTexCoord;
    return OUT;
}
