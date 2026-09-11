#pragma once
#include "globals.hlsli"
#include "light.hlsli"
#include "renderview.hlsli"

#define MAX_LIGHTS 64
#define MAX_MATERIALS 4096

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

// Exact sRGB transfer functions shared by the LDR-out passes: the swapchain
// is an _SRGB format (hardware encodes on store), while the SMAA/UI LDR
// intermediates are UNORM holding already-encoded bytes.
float3 SrgbEncode(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92f;
    float3 hi = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return select(c <= 0.0031308f, lo, hi);
}

float3 SrgbDecode(float3 c)
{
    c = saturate(c);
    float3 lo = c / 12.92f;
    float3 hi = pow((c + 0.055f) / 1.055f, 2.4f);
    // NOTE: decode threshold lives in the *encoded* domain (0.04045), not
    // the linear-domain 0.0031308 used by SrgbEncode above. Using the encode
    // threshold here lifts near-blacks by up to ~165% (e.g. 0.005 -> 2.6x).
    return select(c <= 0.04045f, lo, hi);
}

// -------------------------------------------------------------------------
// Dithered (screen-door) transparency for AlphaMode::Blend materials.
// Opaque pipeline state (depth write on, blending off); pixels are kept
// with probability ~= alpha via an ordered Bayer threshold. Temporal AA
// (SMAA T2x) integrates the pattern over frames; without it a static
// 4x4 Bayer reads as a fine stipple.
//
// The frame selects the technique via pc.pad_pc (0 = dithered, the
// default; 1 = classic alpha blend). Helpers take plain args so passes
// with their own push layout (GFXSEED/GFXFILTERED/...) still compile.
// -------------------------------------------------------------------------

// 4x4 Bayer threshold in (0,1): pixel coords -> threshold.
float DitherBayer4(float2 pix)
{
    uint x = (uint)pix.x & 3u;
    uint y = (uint)pix.y & 3u;
    uint idx = y * 4u + x;
    // Bayer 4x4 / 16, biased +0.5 so alpha==0 keeps nothing, alpha==1 keeps all.
    float bayer[16] = {
        0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f,
        12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f,
        3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f,
        15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f
    };
    return (bayer[idx] + 0.5f / 16.0f);
}

// True when a Blend-material fragment with the given alpha survives.
// Opaque / Mask materials never reach here (alphaMode check by caller).
bool DitherKeep(float alpha, float2 pix)
{
    if (alpha <= 0.0f)
        return false;
    if (alpha >= 1.0f)
        return true;
    return alpha > DitherBayer4(pix);
}

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

// Deferred-shadow buffer types (constants + fixed-size GPU structs with
// no dependencies). Included here so FrameUniforms below can carry typed
// pointers to them; the push struct + helpers live in deferredshadow.hlsli
// (included after FrameUniforms / ForwardPlusLight are complete).
#include "deferredshadow_types.hlsli"

// SG-GI probe volume types (Engine/GI/sg.h + probe_volume.h mirrors).
// Included here so FrameUniforms below can carry typed pointers; the
// sampling/evaluation helpers live in sggi.hlsli (consumers include it
// after common.hlsli).
#include "sggi_types.hlsli"

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
    float4 params2;          // 80 x=occlusionStrength, y=normalScale, z=emissiveStrength, w=alphaMode (0=Opaque,1=Mask,2=Blend)
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

// Moved below the Forward+ light structs so it can carry typed pointers
// into the deferred-shadow buffers (deferredshadow_types.hlsli above).
// Offsets mirror the C++ FrameUniforms in App/scene_renderer.h (asserted
// there): env at 512, deferred-shadow fields at 640, SG-GI fields at 672;
// visualizer fields (DS_DEBUG_VIZ only) follow at 720 — release keeps the
// 720 B prefix (GI included, visualizers compiled out).
struct FrameUniforms
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;

    float4x4 invView;
    float4x4 invProj;
    float4x4 invViewProj;
    float4x4 prevViewProj; // last frame's proj*view (post Y-flip) – motion vectors

    float3 cameraPos;
    float time;

    float2 viewportSize;
    float2 invViewportSize;

    float nearPlane;
    float farPlane;
    float exposure;
    // SSAO consumption (lighting-pass ambient): the FULL-res composed AO
    // (upscaled by the SsaoCompose pass with depth authority, see
    // ao.hlsli). Mirrors the C++ FrameUniforms in App/scene_renderer.h
    // (offsets asserted there: 492/496, env shifts to 512).
    uint aoTextureIndex; // full-res composed AO bindless, 0xffffffff = off
    float aoIntensity;   // ambient composite strength
    // Explicit pad: env must start at offset 512 (16-byte aligned).
    // Without this HLSL packs env at 500, which violates Block layout
    // rules for PhysicalStorageBuffer (spirv-val: member not aligned
    // to 16) and desyncs from the C++ struct, where the compiler pads
    // env to 512. Mirrors C++ FrameUniforms::aoPad_.
    float3 aoPad;

    // Configurable environment (skydome IBL + fog).
    // Must match C++ smvk::EnvironmentFrameData (8 x float4, 128 bytes).
    EnvironmentFrameData env;

    // ---- Deferred shadows (tiled/zBin pipeline, see deferredshadow*.hlsli).
    // Consumer-side view of this frame's per-flight buffers; the app
    // flips dsEnabled per frame (A/B against the inline forward+ path).
    vk::BufferPointer<DsTileStateBuffer> dsTileState;  // 640 {slot,count,approx,early}/tile
    vk::BufferPointer<DsLightListBuffer> dsLightList;  // 648 final entries
    vk::BufferPointer<DsVisibilityBuffer> dsVisibility; // 656 64B slots
    uint dsTileCountX;                                 // 664 8px tiles/row
    uint dsEnabled;                                    // 668 1 = consume lists
    // ---- SG-GI baked probe volume (see sggi.hlsli + Engine/GI). Static
    // buffers uploaded once at scene load; giEnabled flips every frame.
    // giIntensity scales the ambient contribution, giDebug selects the
    // F8 irradiance debug view (1 = flat probe diffuse, pre-exposure).
    // The release layout keeps the 720 B prefix property: these fields
    // precede the DS visualizer tail below.
    vk::BufferPointer<SgGiProbesBuffer> giProbes;   // 672 probe array (96 B each)
    vk::BufferPointer<SgGiGrid> giGrid;            // 680 level descriptors
    vk::BufferPointer<SgGiCellsBuffer> giCells;    // 688 uint2 (start,count)/cell
    vk::BufferPointer<SgGiIndexBuffer> giIndices;   // 696 u32 probe index list
    uint giEnabled;                                 // 704 1 = sample probes for ambient
    float giIntensity;                              // 708 ambient scale
    uint giDebug;                                    // 712 0 off, 1 irradiance view
    uint giPad;                                      // 716
    // ---- Deferred-shadow debug visualizers (paper 5.3/5.4). Debug-build
    // only (DS_DEBUG_VIZ): tail fields so the release layout (672 B) is a
    // strict prefix of the debug layout (704 B). The light-scoped modes
    // re-query the phase-1 (CPU) and phase-2 (GPU) TileGroup masks;
    // dsDebug* pick mode / light / flags / heat scale.
#if defined(DS_DEBUG_VIZ)
    vk::BufferPointer<DsTileLightMaskBuffer> dsTileMasks;     // 720 phase-1 masks
    vk::BufferPointer<DsTileLightMaskBuffer> dsTileGroupMasks; // 728 phase-2 masks
    uint dsDebugMode;      // 736 0=off, 1-4 counts, 5 cull step, 6 list, 7 vis
    uint dsDebugLight;     // 740 selected light (sorted index)
    uint dsDebugFlags;     // 744 bit0 = blend over lit
    uint dsDebugCountCeil; // 748 count-heat ceiling (lights)
#endif
};

// Deferred-shadow push constants + shared helpers (needs FrameUniforms
// and ForwardPlusLight above; includes deferredshadow_types.hlsli again,
// which is guarded and free-standing).
#include "deferredshadow.hlsli"

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
    // Transparency technique for AlphaMode::Blend materials (see DitherKeep
    // above): 0 = dithered screen-door (default, opaque state), 1 = classic
    // alpha blend. Must match App/push_constants.h pad_pc.
    uint pad_pc;
    // RenderView system (see renderview.hlsli): per-frame virtual cameras
    // (main game camera + one view per shadow cascade / spot tile / custom
    // pass). mainRenderView indexes the game camera view; renderViewIndex
    // is the view the current pass renders from, 0xFFFFFFFF when the pass
    // has no view of its own (compute / post passes).
    vk::BufferPointer<RenderViewBuffer>
    renderViews;
    uint mainRenderView;
    uint renderViewIndex;
    // Motion vector pass: last frame's model matrix per draw (192..255).
    float4x4 prevModel;
};

#if !defined(TONEMAP_PASS) && !defined(BLOOM_PASS) && !defined(MOTIONBLUR_PASS) && !defined(SSAO_PASS) && !defined(FORWARDPLUS_PASS) && !defined(UI_PASS) && !defined(GFXFILTER_PASS) && !defined(GFXFILTERED_PASS) && !defined(HZB_PASS) && !defined(GFXSEED_PASS) && !defined(EXPOSURE_PASS) && !defined(SMAA_PASS) && !defined(DS_PASS)
// ENGINE_HAS_PC: this translation unit sees the scene-wide `pc` push
// constants. Passes with their own push layout (FORWARDPLUS/HZB/DS/...)
// leave it undefined so headers can guard pc-dependent helpers (see
// shadows.hlsli FromPC wrappers).
#define ENGINE_HAS_PC
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;
#endif
