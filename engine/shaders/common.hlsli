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

// Reflection cubemaps for specular IBL (kBinding_Cubemaps = 6).
// Uses the same bindless index space as textures[].
[[vk::binding(6, 0)]]
TextureCube cubemaps[];

// 3D volume textures (SG probe grids, kBinding_VolumeTextures): slots
// mirror the bindless texture indices; only 3D views are written here.
[[vk::binding(5, 0)]]
Texture3D volumeTextures[];

// Texture-streaming GPU feedback (Skull & Bones style, see
// texture_streaming.hlsli): one uint per bindless texture slot, published
// by materials with InterlockedMin. Always bound (RHI-owned per-flight
// ring, rewritten to the current slice every frame); consumed on the CPU
// every 4th frame.
[[vk::binding(4, 0)]]
RWStructuredBuffer<uint> g_texFeedback;
#include "texture_streaming.hlsli"

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

// Max finite half-float value. The HDR scene target is RGBA16F: any
// lighting result above this rounds to +inf on store (fp16 round-to-inf),
// and a single inf texel poisons the whole downstream chain (bloom Karis
// weights 1/(1+inf) = 0, tonemap inf/inf = NaN -> saturate(NaN) = 0:
// flickering black blocks). HDR writers clamp to it; consumers sanitize
// their taps against it.
static const float kMaxFiniteHalf = 65504.0f;

// NaN -> 0, clamp into [0, max finite half]. Identical look for all
// finite input: only values that would store as inf change.
float3 SanitizeHdr(float3 c)
{
    c = select(isnan(c), float3(0.0f, 0.0f, 0.0f), c);
    return clamp(c, float3(0.0f, 0.0f, 0.0f), float3(kMaxFiniteHalf, kMaxFiniteHalf, kMaxFiniteHalf));
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

// SG lightmap + regular probe-volume types (Engine/GI/lightmap.h +
// regular_volume.h mirrors): chunk descriptors, volume OBBs. Included so
// FrameUniforms can carry typed pointers; helpers live in
// sg_lightmap.hlsli / sg_regular.hlsli.
#include "sg_lightmap_types.hlsli"

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
    int4 layerMaterials;     // 112 x/y/z = layer 1..3 material ids (this material is layer 0; -1 = none), w = layer mode (0 = vertex-color tint, 1..4 = layered with N layers)
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

// ---- Forward+ tiled lights (point/spot/probe; directional stays on CSM path) ----
// Must match C++ smvk::ForwardPlusLight (96 bytes).
// flags bit0 = shadow request (kForwardPlusFlag_CastShadow); shadowTile >= 0
// means this frame assigned a spot-shadow tile (spot only, points ignore).
// cookieIndex >= 0 = bindless gobo texture projected through the spot cone
// (spot only). functionType > 0 = procedural light function (see
// light.hlsli EvaluateLightFunction): 1 flicker, 2 pulse, 3 cone bands.
// type 2 = reflection probe (specular IBL bucketing, see below): position =
// probe center, range = influence + fade (cull outer sphere), direction =
// (fadeRadius, cubemapIndex, mipLevels) — NOT a light, skipped by every
// direct-lighting loop via `li >= nlights` (probes sort last, nlights is
// lights-only) and consumed by EvaluateSpecularEnv's type filter.
static const int kForwardPlusProbe = 2;
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
// Dedicated per-tile slots for type-2 probe entries (specular IBL): the
// tile index lists pack lights at [0..perTile) and probes at
// [perTile..perTile+MAX_FP_PROBE_TILE_SLOTS) with ONE stride
// (perTile + slots, matches the C++ allocation), so light-saturated
// tiles can never pop their probes (a chrome ball flipping to the global
// fallback cubemap just because a light grid came on). Probe overflow
// beyond the slots pops instead (first-come = upload order, pinned
// probes first).
#define MAX_FP_PROBE_TILE_SLOTS 4

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

// ---- Forward+ tiled decals (projected splats; see forwardplus.h) ----
// Must match C++ smvk::ForwardPlusDecal (128 bytes). An oriented box
// projected along local +Z (xform2 xyz = axisZ/halfZ): a pixel receives the
// decal when its world position maps into the unit cube via the 3 rows AND
// the geometric normal faces the projector (dot(geomN, axisZ) < 0).
// UV = local.xy * 0.5 + 0.5. material: x = albedo tex (rgba, a = coverage),
// y = normal map (tangent xy, Z reconstructed), z = flags (1=albedo,
// 2=normal, 4=rough/metal), w reserved. color: rgb tint, a opacity.
// params: x normalStrength, y roughnessTarget, z metalTarget,
// w depthFadeStart (|local.z| fade toward 1). emissive: rgb add, a strength.
struct ForwardPlusDecal
{
    float4 bounds;   // 0   xyz world center, w bounding-sphere radius
    float4 xform0;   // 16  world->unit-cube row 0 (axisX/halfX, -c*axisX/halfX)
    float4 xform1;   // 32  row 1
    float4 xform2;   // 48  row 2 (projection axis in xyz)
    int4 material;   // 64  albedoTex, normalTex, flags, -1
    float4 color;    // 80  rgb tint, a opacity
    float4 params;   // 96  normalStrength, roughTarget, metalTarget, depthFadeStart
    float4 emissive; // 112 rgb emissive add, a strength
};

#define MAX_FP_DECALS 256
#define MAX_FP_DECALS_PER_TILE 16
#define FP_DECAL_ALBEDO 1
#define FP_DECAL_NORMAL 2
#define FP_DECAL_MATERIAL 4

struct ForwardPlusDecalBuffer
{
    ForwardPlusDecal decals[MAX_FP_DECALS];
};

struct ForwardPlusDecalTileCountBuffer
{
    uint counts[MAX_FP_TILES];
};

struct ForwardPlusDecalTileIndexBuffer
{
    uint indices[MAX_FP_TILES * MAX_FP_DECALS_PER_TILE];
};

// Moved below the Forward+ light structs so it can carry typed pointers
// into the deferred-shadow buffers (deferredshadow_types.hlsli above).
// Offsets mirror the C++ FrameUniforms in App/scene_renderer.h (asserted
// there): env at 512, cubemap block at 640, specular knobs at 656,
// deferred-shadow fields at 672, SG-GI fields at 704, evsmMode at 748,
// decal fields at 752, lightmap fields at 784, regular-volume fields at
// 848; visualizer fields (DS_DEBUG_VIZ only) follow at 1104.
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

    // Reflection cubemap for specular IBL (bindless TextureCube index).
    // NOTE: this 16-byte block must stay exactly 16 bytes (offsets
    // asserted in App/scene_renderer.h): index + sampler + intensity +
    // debug. Everything after it (specular knobs at 656, DS at 672, ...)
    // shifts otherwise and evsmMode/shadow addresses read garbage.
    uint cubemapIndex;
    uint cubemapSampler;   // bindless sampler index (clamp-to-edge, linear mips)
    float cubemapIntensity; // specular IBL scale (UI)
    uint cubemapDebug;      // 0 off, 1 cubemap-only, 2 probe weight

    // Specular IBL knobs (16 bytes, dsTileState stays at 672). Reflection-
    // probe DATA rides ForwardPlusLight type-2 entries in the FP light
    // buffer (see kForwardPlusProbe above) — no probe storage buffer, no
    // parallel tile lists. probeTileFresh tells the lit shader the tile
    // cull filed probe entries this frame (0 = stale -> global cubemap
    // fallback); probeEntries is the type-2 tail length, so the specular
    // walk's index bound is fpLightCount + probeEntries (lights [0,L),
    // probes [L,L+P) — a lights-only bound would skip every probe).
    float cubemapLodBias; // 656 added to the roughness-derived cubemap LOD (UI)
    uint probeTileFresh;  // 660 1 = tile lists hold probe entries this frame
    uint probeEntries;    // 664 type-2 entries in the light-buffer tail
    uint probePad;        // 668 (16-align tail)

    // ---- Deferred shadows (tiled/zBin pipeline, see deferredshadow*.hlsli).
    // Consumer-side view of this frame's per-flight buffers; the app
    // flips dsEnabled per frame (A/B against the inline forward+ path).
    vk::BufferPointer<DsTileStateBuffer> dsTileState;  // 672 {slot,count,approx,early}/tile
    vk::BufferPointer<DsLightListBuffer> dsLightList;  // 680 final entries
    vk::BufferPointer<DsVisibilityBuffer> dsVisibility; // 688 64B slots
    uint dsTileCountX;                                 // 696 8px tiles/row
    uint dsEnabled;                                    // 700 1 = consume lists
    // ---- SG-GI baked probe volume (see sggi.hlsli + Engine/GI). Static
    // buffers uploaded once at scene load; giEnabled flips every frame.
    // giIntensity scales the ambient contribution, giDebug selects the
    // F8 irradiance debug view (1 = flat probe diffuse, pre-exposure).
    // The release layout keeps the 720 B prefix property: these fields
    // precede the DS visualizer tail below.
    vk::BufferPointer<SgGiProbesBuffer> giProbes;   // 704 probe array (96 B each)
    vk::BufferPointer<SgGiGrid> giGrid;            // 712 level descriptors
    vk::BufferPointer<SgGiCellsBuffer> giCells;    // 720 uint2 (start,count)/cell
    vk::BufferPointer<SgGiIndexBuffer> giIndices;   // 728 u32 probe index list
    uint giEnabled;                                 // 736 1 = sample probes for ambient
    float giIntensity;                              // 740 ambient scale
    uint giDebug;                                    // 744 0 off, 1 irradiance view
    uint evsmMode;                                   // 748 0 = PCF depth, 1 = EVSM moments (was padding)
    // ---- Forward+ decals (tiled projected splats, see ForwardPlusDecal).
    // decals points at this frame's flight slice of the app decal ring;
    // the tile lists are the ForwardPlus builder's decal buffers, refreshed
    // by the tile-cull dispatch every frame it runs (the app guarantees
    // decalCount > 0 only when the cull ran, and the tile pointers are
    // valid whenever the cull can run — the cull stores a count of 0 for
    // every tile even with no decals, so the shading pass never sees
    // stale lists). decalMaxPerTile is the list stride (matches the
    // builder allocation).
    vk::BufferPointer<ForwardPlusDecalBuffer> decals;        // 752
    vk::BufferPointer<ForwardPlusDecalTileCountBuffer> decalTileCounts; // 760
    vk::BufferPointer<ForwardPlusDecalTileIndexBuffer> decalTileIndices; // 768
    uint decalCount;                                         // 776 0 = decals off
    uint decalMaxPerTile;                                    // 780 list stride
    // ---- SG 2D lightmaps (STATIC geometry; Engine/GI/lightmap.h).
    // The atlas layers ride the bindless Texture2D array; the atlas-space
    // UVs are COOKED into the mesh's texCoord2 and the lobe count rides
    // the draw ring — no chunk buffer exists. Static/dynamic separation:
    // draws flagged with a lobe count sample the atlas; everything else
    // (dynamic objects) samples the regular 3D probe grids below or the
    // fallback ambient. lmLayerKind selects the GPU layout: 0 = the
    // classic 9 RGBA16F stream layers (half h = lobe*3 + channel ->
    // layer h/4), 1 = BC6H one-texture-per-lobe (12 slots, texture l =
    // lobe l, .rgb = the amplitude). ----
    uint lmLayers[12];                                       // 784 atlas bindless slots
uint lmEnabled;                                          // 832 1 = lightmap branch active
    uint lmLayerKind;                                        // 836 0 = RGBA16F stream, 1 = BC6H per-lobe
    uint2 lmPad;                                              // 840 (align regObbs to 8)
    // ---- SG regular 3D probe volumes (DYNAMIC objects; Ready-at-Dawn
    // grids, Engine/GI/regular_volume.h). Hand-placed artist OBBs with
    // density params, baked per volume; 7 RGBA16F 3D textures each,
    // hardware trilinear filtering between neighboring probes. ----
    vk::BufferPointer<SgRegularObbBuffer> regObbs;           // 848 OBB array
    uint regLayers[56];                                       // 856 8 volumes x 7 layer slots
    uint regCount;                                             // 1080 active volumes
    uint regEnabled;                                           // 1084 1 = grid branch active
    uint4 regPad;                                              // 1088 (16-align tail)
    // ---- Deferred-shadow debug visualizers (paper 5.3/5.4). Debug-build
    // only (DS_DEBUG_VIZ): tail fields so the release layout (1104 B) is a
    // strict prefix of the debug layout. The light-scoped modes
    // re-query the phase-1 (CPU) and phase-2 (GPU) TileGroup masks;
    // dsDebug* pick mode / light / flags / heat scale.
#if defined(DS_DEBUG_VIZ)
    vk::BufferPointer<DsTileLightMaskBuffer> dsTileMasks;     // 1104 phase-1 masks
    vk::BufferPointer<DsTileLightMaskBuffer> dsTileGroupMasks; // 1112 phase-2 masks
    uint dsDebugMode;      // 1120 0=off, 1-4 counts, 5 cull step, 6 list, 7 vis
    uint dsDebugLight;     // 1124 selected light (sorted index)
    uint dsDebugFlags;     // 1128 bit0 = blend over lit
    uint dsDebugCountCeil; // 1132 count-heat ceiling (lights)
    uint4 dsDebugPad;      // 1136 (16-align tail)
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

#if !defined(TONEMAP_PASS) && !defined(BLOOM_PASS) && !defined(MOTIONBLUR_PASS) && !defined(SSAO_PASS) && !defined(GTAO_PASS) && !defined(FORWARDPLUS_PASS) && !defined(UI_PASS) && !defined(GFXFILTER_PASS) && !defined(GFXFILTERED_PASS) && !defined(HZB_PASS) && !defined(GFXSEED_PASS) && !defined(EXPOSURE_PASS) && !defined(SMAA_PASS) && !defined(DS_PASS) && !defined(EVSM_BLUR_PASS) && !defined(DOF_PASS) && !defined(SSR_PASS) && !defined(VOLUMETRIC_PASS) && !defined(PARTICLES_PASS) && !defined(UVFLAT1_PASS) && !defined(DEFAULT_DIRECT_PASS)
// ENGINE_HAS_PC: this translation unit sees the scene-wide `pc` push
// constants. Passes with their own push layout (FORWARDPLUS/HZB/DS/...)
// leave it undefined so headers can guard pc-dependent helpers (see
// shadows.hlsli FromPC wrappers).
#define ENGINE_HAS_PC
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;
#endif
