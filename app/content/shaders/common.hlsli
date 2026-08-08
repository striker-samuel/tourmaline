
// Default bindings
[[vk::binding(0,0)]] Texture2D materialTex[]; // sampled images
[[vk::binding(1,0)]] SamplerState materialSamp[]; // samplers

struct FrameUniforms
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;

    float4x4 invView;
    float4x4 invProj;
    float4x4 invViewProj;

    float3 cameraPos;
    float time;

    float2 viewportSize;
    float2 invViewportSize;

    float nearPlane;
    float farPlane;
    float exposure;
    float padding;
};

struct PC_Draw
{
    float4x4 model;
    float4 ambient;
    float4 diffuse;
    uint4 tex;
    vk::BufferPointer<FrameUniforms>frameUniforms;
};

[[vk::push_constant]]
ConstantBuffer<PC_Draw> pc;
