#pragma once
#include "light.hlsli"

#define MAX_LIGHTS 64
#define MAX_MATERIALS 1024

[[vk::binding(0, 0)]]
//[[vk::binding(kBinding_Textures, 0)]]
Texture2D textures[];
[[vk::binding(1, 0)]]
//[[vk::binding(kBinding_Samplers, 0)]]
SamplerState samplers[];
[[vk::binding(1, 0)]] // aliases to samplers! CAREFUL
SamplerComparisonState comparisonSamplers[];

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


struct LightBuffer
{
    Light lights[MAX_LIGHTS];
};

struct MaterialValues
{
    float4 ambient;
    float4 diffuse;
    uint4 tex;
};

struct MaterialsBuffer
{
    MaterialValues mats[MAX_MATERIALS];
};

struct PC_Draw
{
    float4x4 model;
    vk::BufferPointer<FrameUniforms>
    frameUniforms;
    vk::BufferPointer<LightBuffer>
    bLights;
    vk::BufferPointer<MaterialsBuffer>
    bMats;
    uint materialId;
    uint comparisonSamplerId;
};

[[vk::push_constant]]
ConstantBuffer<PC_Draw> pc;
