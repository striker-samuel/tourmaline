#ifndef MAX_LIGHTS
#define MAX_LIGHTS 10
#endif


[[vk::push_constant]]
struct PC_Draw
{
    float4x4 model;
    uint materialIndex;
    float _pad[3];

    //Material
    float4 ambient;
    float4 diffuse;
    uint4 tex; //diffuse, ambient, spec
    //float4 _pad;
} pc;


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
    float padding; // keep 16-byte alignment
};

