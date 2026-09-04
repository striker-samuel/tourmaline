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

// Configurable environment block (skydome IBL + fog).
// Must match C++ smvk::EnvironmentFrameData (8 x float4, 128 bytes).
struct EnvironmentFrameData
{
    float4 skyZenith;      // rgb zenith color (linear 0..1), w skyIntensity (HDR)
    float4 skyHorizon;     // rgb horizon color (linear 0..1), w gradient exponent
    float4 ground;         // rgb ground color (linear 0..1), w iblIntensity
    float4 sunDirection;   // xyz dir TO sun (normalized), w disc cos(radius)
    float4 sunColor;       // rgb sun color (linear 0..1), w sunIntensity (lux-like)
    float4 fogColor;       // rgb fog color (linear 0..1), w fogIntensity (HDR)
    float4 fogDensityHeight; // x density (exp2), y height, z heightFalloff, w linear start
    float4 fogEndAndFlags; // x linear end, y skyMode, z fogMode, w iblEnabled
};

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

    // Configurable environment (skydome IBL + fog).
    // Must match C++ smvk::EnvironmentFrameData (8 x float4).
    EnvironmentFrameData env;
};


struct LightBuffer
{
    Light lights[MAX_LIGHTS];
    //uint lightCount;
};

struct MaterialValues
{
    float4 ambient;          // 0  sky-ambient factor: skyColor * skyLux/PI (albedo multiplier, lux convention)
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

// ---- Forward+ tiled lights (point/spot; directional stays on CSM path) ----
// Must match C++ smvk::ForwardPlusLight (96 bytes).
// flags bit0 = shadow request (kForwardPlusFlag_CastShadow); shadowTile >= 0
// means this frame assigned a spot-shadow tile (spot only, points ignore).
// cookieIndex >= 0 = bindless gobo texture projected through the spot cone
// (spot only). functionType > 0 = procedural light function (see
// light.hlsli EvaluateLightFunction): 1 flicker, 2 pulse, 3 cone bands.
struct ForwardPlusLight
{
    float4 color;       // 0  linear RGB + pad
    float3 position;    // 16 world pos
    float range;        // 28 cutoff (m); <=0 = infinite
    float3 direction;   // 32 normalized spot axis
    float intensity;    // 44 candela (lm/sr)
    float innerAngle;   // 48 radians (spot)
    float outerAngle;   // 52 radians (spot)
    int type;           // 56 0=point, 1=spot
    int flags;          // 60 bit0 = cast-shadow request
    int shadowTile;     // 64 tile in spot atlas, -1 = unshadowed
    int shadowMapIndex; // 68 bindless spot atlas index, -1 = unshadowed
    float shadowBias;   // 72 receiver-depth bias (perspective)
    float pad2_;        // 76
    int cookieIndex;    // 80 bindless gobo texture, -1 = none
    int functionType;   // 84 0=none, 1=flicker, 2=pulse, 3=bands
    float functionSpeed; // 88 animation speed multiplier
    float functionScale; // 92 modulation depth / pattern scale
};

#define MAX_SPOT_SHADOWS 8

// Spot-shadow tile descriptors. Reuses GpuCascadeDescription layout
// (lightVP + atlas UV transform + bias); splitEnd stores the spot range.
struct SpotShadowBuffer
{
    GpuCascadeDescription tiles[MAX_SPOT_SHADOWS];
};

#define MAX_FP_LIGHTS 1024
#define MAX_FP_TILES 32768
#define MAX_FP_PER_TILE 128

struct ForwardPlusLightBuffer
{
    ForwardPlusLight lights[MAX_FP_LIGHTS];
};

struct ForwardPlusTileCountBuffer
{
    uint counts[MAX_FP_TILES];
};

struct ForwardPlusTileIndexBuffer
{
    uint indices[MAX_FP_TILES * MAX_FP_PER_TILE];
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
    // Forward+ (opaque pass; ignored when fpLightCount == 0)
    vk::BufferPointer<ForwardPlusLightBuffer>
    fpLights;
    vk::BufferPointer<ForwardPlusTileCountBuffer>
    fpTileCounts;
    vk::BufferPointer<ForwardPlusTileIndexBuffer>
    fpTileIndices;
    uint fpTileCountX;
    uint fpLightCount;
    uint fpTileSize;
    uint fpMaxPerTile;
    // Spot shadows (opaque pass sampling + spot depth passes).
    // spotShadowBuffer: tile descriptors (GpuCascadeDescription tiles).
    // spotShadowMapIndex: bindless spot atlas, UINT32_MAX when none.
    // spotShadowCount: active tiles this frame (<= MAX_SPOT_SHADOWS).
    // spotIndex: tile rendered by the current shadow pass,
    //   0xFFFFFFFF when the pass is NOT a spot shadow pass.
    vk::BufferPointer<SpotShadowBuffer>
    spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint spotIndex;
    uint pad_pc;
};

#if !defined(TONEMAP_PASS) && !defined(BLOOM_PASS) && !defined(FORWARDPLUS_PASS)
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;
#endif
