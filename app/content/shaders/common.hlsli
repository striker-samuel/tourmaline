#pragma once
#include "light.hlsli"

#define MAX_LIGHTS 64
#define MAX_MATERIALS 1024

//#define kBinding_Textures 0
//#define kBinding_Samplers 1

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
    //uint lightCount;
};

struct MaterialValues
{
    float4 ambient;          // 0  legacy ambient
    float4 diffuse;          // 16 baseColorFactor
    int4 tex;                // 32 x=baseColor, y=metallicRoughness, z=normal, w=occlusion
    int4 tex2;               // 48 x=emissive, yzw unused (-1)
    float4 params;           // 64 x=alphaCutoff, y=metallic, z=roughness, w=shadingModel
    float4 params2;          // 80 x=occlusionStrength, y=normalScale, z=emissiveStrength, w=unused
    float4 emissiveFactor;   // 96 xyz=emissiveFactor, w=pad
};

struct MaterialsBuffer
{
    MaterialValues mats[MAX_MATERIALS];
};

// ---- Cascaded shadow map structures (mirrors Engine/Renderer/light.h) ----
struct GpuCascadeDescription
{
    float4x4 lightVP;
    float splitEnd;
    float bias;
    float normalBias;
    float pad_;
    float2 invShadowSize;
    float2 uvScale;
    float2 uvBias;
    float2 uvClampMin;
    float2 uvClampMax;
    float2 pad2;
};

struct GpuDirectionalLightDescription
{
    float4 color;
    float3 direction;
    int flags;
    float intensity;
    int shadow_map_index;
    int numCascades;
    float pad_; // scalar pad: keeps lightVP 16-byte aligned (no straddling vec3)
    float4x4 lightVP; // fallback / cascade 0
    GpuCascadeDescription cascades[4];
    // total 624 bytes – matches C++ GpuDirectionalLightDescription exactly
};

struct DirectionalCascadeBuffer
{
    GpuDirectionalLightDescription dir;
};

struct PushConstants
{
    float4x4 model;
    vk::BufferPointer<FrameUniforms>
    frameUniforms;
    vk::BufferPointer<LightBuffer>
    bLights;
    vk::BufferPointer<MaterialsBuffer>
    bMats;
    vk::BufferPointer<DirectionalCascadeBuffer>
    cascadeBuffer;
    uint materialId;
    uint comparisonSamplerId;
    uint numCascades;
    uint cascadeIndex;
};

#ifndef TONEMAP_PASS
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;
#endif
