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

// PixelShader/FragmentShader Input 
struct PSIn
{
    uint instanceID : SV_InstanceID;
    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
};

struct PSOut
{
    float4 pos : SV_Position;
    float3 outColor : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    // Index is uniform per draw (push constants), so nonuniform isn’t required.
    uint texIdx = pc.texDiffuse;

    
    OUT.outColor = (1.0f, 1.0f, 1.0f);
    
    //OUT.outColor = materialTex[texIdx].Sample(materialSamp[0], IN.uv);
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
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float4 tangent : TANGENT;
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;

    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;

    uint idx = indicesStd140[IN.instanceID].i;
    float4x4 model = instance_data[idx].transform;

    // World position
    float4 world = mul(model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;

    // View / projection
    float4 viewPos = mul(pc.view, world);
    float4 clip = mul(pc.proj, viewPos);
    OUT.pos = clip;

    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = IN.uv;

    // Properly transform normal to world space
    float3 worldNormal = mul((float3x3) model, IN.normal);
    OUT.normal = normalize(worldNormal);

    return OUT;
}
