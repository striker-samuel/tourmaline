// =============================================
// HLSL (for Vulkan via DXC)
// =============================================

#ifndef MAX_INSTANCES
#define MAX_INSTANCES 1000
#endif

[[vk::push_constant]]
struct PC_All
{
    float4x4 view;
    float4x4 proj;
    int material_instance_index;
    int3 _pad0; // 16B pad to keep 16-byte alignment

    float4 ambient;
    float4 diffuse;
    uint texAmbient;
    uint texDiffuse;
    uint texAlpha;
    uint _pad1; // keep size at 192 bytes total
} pc;

[[vk::binding(2,0)]] Texture2D materialTex[]; // sampled images
[[vk::binding(3,0)]] SamplerState materialSamp[]; // samplers

struct PSIn
{
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
};
struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    // Index is uniform per draw (push constants), so nonuniform isn’t required.
    uint texIdx = pc.texDiffuse;

    OUT.outColor = materialTex[texIdx].Sample(materialSamp[0], IN.uv);
    // If you really want per-texture samplers too, use materialSamp[pc.texAmbient] or similar.

    return OUT;
}


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



//struct VSOut
//{
//    float4 pos : SV_Position; // gl_Position
//    float3 fragColor : COLOR0; // layout(location=0) out
//    float2 fragTexCoord : TEXCOORD0; // layout(location=1) out
//};

struct VSOut
{
    float4 pos : SV_Position;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;

    // Find which instance's model matrix to use:
    // GLSL: instance.model[inn.indices[gl_InstanceIndex]]
    uint idx = indicesStd140[IN.instanceID].i;

    // Position transform:
    // GLSL: pc.viewproj.proj * pc.viewproj.view * M * vec4(inPosition, 1.0)
    float4 world = float4(IN.inPosition, 1.0f);
    float4x4 model = instance_data[idx].transform;
    float4 clip = mul(pc.proj, mul(pc.view, mul(model, world)));

    OUT.pos = clip;
    OUT.color = instance_data[idx].color;
    //OUT.fragColor = IN.inColor;
    OUT.uv = IN.inTexCoord;
    return OUT;
}
