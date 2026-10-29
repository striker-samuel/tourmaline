// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "globals.hlsli"
#include "light.hlsli"
#include "renderview.hlsli"

// Shared CPU/HLSL layout-contract limits (materials, lights, bindless
// array bounds) — single source: engine/shaders/engine_limits.hlsli.
// The C++ side bridges the same file via Engine/Core/Limits.h.
#include "engine_limits.hlsli"

// Specialization constant IDs (driver will constant-fold these)
// Must match CPU-side pipeline creation (App::CreatePipelines)
#define SPEC_GI_DEBUG_MODE   0  // giDebug mode: 0=off, 1=flat irradiance, 2=source palette
#define SPEC_EVSM_MODE       1  // 0=PCF depth, 1=EVSM moments
#define SPEC_SSR_ENABLED     2  // 0=disable SSR, 1=enable SSR
#define SPEC_GI_INTENSITY    3  // intensity scale (float, driver promotes to constant)

#define SPEC_GI_VARIANT     10  // GI variant: 0=GI_NONE, [1 retired — GI_OCTREE], 2=GI_REGULAR, 3=GI_VOLUMES

// Far-LOD quality knobs for the lit program family (see ShaderLodPolicy
// + World::FarShadingAt): applied as specialization constants on the
// fragment stage when a farthest cooked level stays lit. Unspecialized
// pipelines fold the defaults (full quality) - every existing caller
// keeps today's behavior with zero changes.
#define SPEC_LIT_SPECULAR    20  // 0=Lambert-only ("simplified BRDF": no GGX, no specular env/FP terms), 1=full specular
#define SPEC_LIT_NORMALMAP   21  // 0=geometric normal (no normal-texture fetch/TBN), 1=tangent-space normal mapping

//#define kBinding_Textures 0
//#define kBinding_Samplers 1

#ifdef TOURMALINE_DXIL
// ---------------------------------------------------------------------------
// The D3D12 backend's bounded-table form (the five 8192-wide regions —
// see dx12_rhi.h's kBindlessCapacity comment): the SAME bindless slot
// space as the Vulkan arrays below, but explicit registers + BOUNDED
// sizes (the root signature's SRV table covers t0..t32767 — an
// unbounded array would claim the whole heap). The type aliases
// (uintTextures / comparisonSamplers) keep the compile-time-fiction
// trick working: same register, different fetch type.
// The DXIL build passes -DTOURMALINE_DXIL=1 (CompileShaders.cmake).
// NOTE: this shim only covers the RESOURCE arrays. vk::BufferPointer
// (SPIR-V-only, 52 files) is the remaining DXIL rework — see the
// porting notes' shader-milestone plan.
// ---------------------------------------------------------------------------
Texture2D textures[MAX_TEXTURES] : register(t0);
Texture2D<uint4> uintTextures[MAX_TEXTURES] : register(t0); // alias, same slots
// The register bases derive from the same bounds (registers cannot be
// macro expressions): t8192 = MAX_TEXTURES, t16384 = 2*MAX_TEXTURES,
// t24576 = 3*MAX_TEXTURES.
Texture3D volumeTextures[MAX_VOLUME_TEXTURES] : register(t8192);
TextureCube cubemaps[MAX_TEXTURES] : register(t16384);
Texture2DMS<float4> texturesMS[MAX_TEXTURES] : register(t24576);
SamplerState samplers[MAX_SAMPLERS] : register(s0);
SamplerComparisonState comparisonSamplers[MAX_SAMPLERS] : register(s0); // alias
#else
[[vk::binding(0, 0)]]
//[[vk::binding(kBinding_Textures, 0)]]
Texture2D textures[];
[[vk::binding(1, 0)]]
//[[vk::binding(kBinding_Samplers, 0)]]
SamplerState samplers[];
[[vk::binding(1, 0)]] // aliases to samplers! CAREFUL
SamplerComparisonState comparisonSamplers[];
[[vk::binding(0, 0)]]
Texture2D<uint4> uintTextures[];

// Reflection cubemaps for specular IBL (kBinding_Cubemaps).
// Uses the same bindless index space as textures[].
[[vk::binding(5, 0)]]
TextureCube cubemaps[];

// 3D volume textures (SG probe grids, kBinding_VolumeTextures): slots
// mirror the bindless texture indices; only 3D views are written here.
[[vk::binding(4, 0)]]
Texture3D volumeTextures[];

// Multisampled 2D textures (kBinding_MsaaTextures): slots mirror the
// bindless texture indices; only MSAA views are written here (the EVSM
// 2x MSAA depth — see evsm_resolve.cs.hlsl). .Load(pos, sample) only.
[[vk::binding(6, 0)]]
Texture2DMS<float4> texturesMS[];
#endif // TOURMALINE_DXIL
// End of the DXIL/Vulkan binding-block split — the remaining shared
// code (transfer functions, material values, light tables) is identical.

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

// -------------------------------------------------------------------------
// Dither-on-closeup (Scene::ditherCloseup; see scene.h): the flagged
// subtree screen-doors away as the camera closes on its anchor — LOD-pop
// hiding for impostors/foliage. The CPU routes the per-frame per-draw state
// (DitherCloseupStateFor): Hidden draws are CPU-culled outright; Fade draws
// carry a per-draw flag (the gfx Draw ring's GFX_DRAW_DITHER_CLOSEUP ->
// the filtered VS / mesh MS forward it flat at location 8, or
// PushConstants::padEnd.x -> default.vert / shadow.vert forward it) and
// every discarding leg — the lit FS family, the masked depth/shadow FSs —
// screen-doors with THIS keep-test: the same ordered Bayer pattern as
// dithered transparency, so SMAA T2x integrates both identically. The
// factor is per-PIXEL (worldPos vs the camera eye), so the CPU pushes
// only the 0/1 flag. Band constants mirror C++ kDitherCloseup* (scene.h).
// -------------------------------------------------------------------------
#define DITHER_CLOSEUP_HIDE_M   0.5f  // fully hidden at/below (m from the anchor)
#define DITHER_CLOSEUP_SOLID_M  3.0f  // fully solid at/beyond (tight band: the
                                     // old [1, 8] started dissolving at doorway
                                     // distance and read too strong)

// Closeup keep FACTOR (0..1): the camera distance [hide, solid] mapped
// hidden -> 0, solid -> 1 (the per-pixel distance, so the CPU pushes only
// the 0/1 flag). The A2C lit twin folds this into its output coverage.
float CloseupDitherFactor(float3 worldPos, float3 eye)
{
    float d = distance(worldPos, eye);
    return saturate(
        (d - DITHER_CLOSEUP_HIDE_M)
        / (DITHER_CLOSEUP_SOLID_M - DITHER_CLOSEUP_HIDE_M));
}

// True when a dither-closeup-flagged fragment survives at this pixel.
bool CloseupDitherKeep(float3 worldPos, float3 eye, float2 pix)
{
    return DitherKeep(CloseupDitherFactor(worldPos, eye), pix);
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
    Light lights[MAX_SIMULTANEOUS_LIGHTS];
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
// means this frame assigned a local-light shadow tile: SPOTS get one
// perspective tile (index into the atlas descriptor array), POINTS get SIX
// consecutive cube-face tiles (shadowTile = the first; the sampler adds the
// dominant-axis face number - see shadows.hlsli GetPointShadowFactorTile).
// shadowMapIndex is the bindless atlas the light samples (the D32 tile
// atlas for points; the blurred EVSM moments for spots when evsmMode on).
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
    int shadowTile;     // 64 spot: tile index; point: FIRST of 6 face tiles; -1 = unshadowed
    int shadowMapIndex; // 68 bindless atlas index, -1 = unshadowed
    float shadowBias;   // 72 receiver-depth bias (perspective)
    float pad2_;        // 76
    int cookieIndex;    // 80 bindless gobo texture, -1 = none
    int functionType;   // 84 0=none, 1=flicker, 2=pulse, 3=bands
    float functionSpeed; // 88 animation speed multiplier
    float functionScale; // 92 modulation depth / pattern scale
};

// Local-light shadow tiles: 16 spot tiles + 6 cube-face tiles per shadowed
// point light (2 max). MUST match C++ kMaxSpotShadows / kMaxPointShadowLights
// / kMaxLocalShadowTiles (spotshadow.h) - both sides size the per-frame
// descriptor ring off these.
#define MAX_SPOT_SHADOWS 16
#define MAX_POINT_SHADOW_LIGHTS 2
#define MAX_LOCAL_SHADOW_TILES (MAX_SPOT_SHADOWS + 6 * MAX_POINT_SHADOW_LIGHTS)

// Local-light shadow tile descriptors (spot tiles + point cube-face tiles;
// the frame numbering is spots first, faces after). Reuses
// GpuCascadeDescription layout (lightVP + atlas UV transform + bias);
// splitEnd stores the light range.
struct SpotShadowBuffer
{
    GpuCascadeDescription tiles[MAX_LOCAL_SHADOW_TILES];
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
// probes first). 8 slots: dense fitted-pocket placement (influence
// ~2.6-5 m + ~1.5 m fade on a ~3.75 m cell grid) puts MORE than 4
// covering probes on one 16 px tile near geometry; the 4-slot
// first-come overflow dropped a 5th probe in one tile but kept it in
// the neighbor (a wider probe missed that tile's frustum), changing
// the blend weight set across the tile boundary - the seam read as
// 16 px TILES in the raw-cubemap debug view. 8 keeps the overflow to
// extreme stacks only.
#define MAX_FP_PROBE_TILE_SLOTS 8

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

// Relevance-sort capacity of the cull's tile lists (surface AND fog): the
// desc's maxLightsPerTile / maxFogPerTile must stay <= it (16 today). A
// larger push clamps HERE (both the inserted slots and the stored counts
// follow), so list addressing can never leave the allocated rows.
#define MAX_FP_TILE_SORT_SLOTS 32

// Effective per-tile light-slot capacity — the ONE clamp chain the cull
// PRODUCER (forwardplus.cs.hlsl) and EVERY surface-list consumer
// (environment.hlsli's probe walk, particles_draw.frag.hlsl's light walk)
// must share to address the same rows of the tile index buffer. The
// producer's sorted insertion can only track MAX_FP_TILE_SORT_SLOTS
// entries, so the configured maxLightsPerTile clamps to it; the consumers
// used to clamp only against MAX_FP_PER_TILE (128), so any configured
// value in (32, 128] made the two sides compute DIFFERENT row strides
// (producer 32+8 vs consumer 64+8) and read garbage rows. The C++
// allocation + both push values come from the same desc field, and
// ForwardPlus::Create rejects a desc beyond the capacity — this clamp is
// the GPU-side defense in depth that keeps producer/consumer addressing
// identical whatever arrives in the pushes.
uint FpTileLightSlots(uint maxPerTile)
{
    return min(min(maxPerTile, (uint) MAX_FP_PER_TILE),
        (uint) MAX_FP_TILE_SORT_SLOTS);
}

// Per-tile FOG lists (the volumetric march's light candidates): SEPARATE
// buffers from the surface lists because their candidacy ignores the tile
// depth slab — march samples live in front of the nearest opaque surface,
// so slab cuts would drop lights the fog needs (the same no-cut rationale
// the GiVolumeTileMaskBuffer leg documents). Probes never ride them (the
// march is scalar — no specular IBL). Row stride = the desc's
// maxFogPerTile (the type row is the sort-cap upper bound, matching the
// surface-list pattern where the type row is MAX_FP_PER_TILE).
struct ForwardPlusFogTileCountBuffer
{
    uint counts[MAX_FP_TILES];
};

struct ForwardPlusFogTileIndexBuffer
{
    uint indices[MAX_FP_TILES * MAX_FP_TILE_SORT_SLOTS];
};

// Per-tile GI-volume bitmask (the volume leg of forwardplus.cs.hlsl):
// bit v set = volume v (upload order = priority order, sg_gi_volume.hlsli)
// can reach the tile. Written UNCONDITIONALLY by the tile cull (fresh 0
// with no volumes / GI volumes off), same grid as the light/decal tile lists.
struct GiVolumeTileMaskBuffer
{
    uint masks[MAX_FP_TILES];
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

// -------------------------------------------------------------------------
// FramePSHot — tiny per-frame data frequently accessed by the pixel shader.
// Separate from FrameUniforms to keep register pressure low: the PS loads
// only this struct (via FrameUniforms::frameHot.Get() — the FS reaches the
// hot slab through FrameUniforms, NOT a pc member) instead of the full
// 1360+ byte FrameUniforms. Offsets mirror the C++ FramePSHot in
// world_renderer.h.
// -------------------------------------------------------------------------
struct FramePSHot
{
    float3 cameraPos;        // 0
    float  time;             // 12 -> 16
    float2 viewportSize;     // 16
    float2 invViewportSize;  // 24 -> 32
    float  nearPlane;        // 32
    float  farPlane;        // 36
    float  exposure;         // 40
    uint   aoTextureIndex;   // 44
    float  aoIntensity;      // 48
    uint   evsmMode;         // 52
    uint   dsEnabled;        // 56
    uint   dsTileCountX;     // 60
    float  fogEndAndFlags_w; // 64 (env.fogEndAndFlags.w = iblEnabled)
    float  giIntensity;      // 68
    uint   giDebug;          // 72
    uint   giVolumesEnabled;  // 76
    uint   pad_[3];          // 80 -> 96 (16-byte aligned total)
};

// Moved below the Forward+ light structs so it can carry typed pointers
// into the deferred-shadow buffers (deferredshadow_types.hlsli above).
// ---- Debug visualizer parameters (paper 5.3 Light Culling / 5.4 Light Count).
// Standalone 32-byte payload (per-flight ring + zero slice on the CPU),
// reached through FrameUniforms::dsVizParams — deliberately NOT inlined in
// FrameUniforms: viz-gated #ifdefs inside GPU-pushed structs once made the
// CPU/GPU layout config-dependent. This struct is UNCONDITIONAL (C++ mirror:
// engine::DsVizParams, 32 bytes); only its fill (app) and read
// (DsVisualize in deferredshadow.hlsli) are DS_DEBUG_VIZ-gated.
struct DsVizParams
{
    vk::BufferPointer<DsTileLightMaskBuffer> tileMasks;  // phase-1 CPU masks (viz)
    vk::BufferPointer<DsTileLightMaskBuffer> tileGroupMasks; // phase-2 GPU masks (viz)
    uint mode;       // DS_VIZ_* (0 = off)
    uint light;      // selected light (sorted index)
    uint flags;      // bit0 = blend over lit
    uint countCeil;  // count-heat ceiling (lights)
};

// FrameUniforms::flags bits (unconditional; C++ mirror: kFrameFlag* in
// WorldRenderer.h). Lets shaders skip off-by-default payloads without a
// BDA chase. Scalar-constructor spelling (no integer-suffix reliance).
#define DS_FLAG_VIZ (uint64_t(1))
// JHFAA-style shading-normal validity correction (shading_normal_fix.hlsli;
// docs/jhfaa_normal_fix.md). bit1 = apply the correction (TOURMALINE_NORMAL_
// FIX pin, default off while under evaluation); bit2 = the red/orange
// violation overlay view (TOURMALINE_NORMAL_FIX_DEBUG — independent of bit1
// so the disease and the medicine can be inspected apart).
#define FRAME_FLAG_NORMAL_FIX (uint64_t(2))
#define FRAME_FLAG_NORMAL_FIX_DEBUG (uint64_t(4))

// World env tiles (Engine/WorldPartition/WorldEnvTiles.h — the runtime
// rework of the old OBB-volume zone system): the zone LOOK records (48 B
// each; the OBB fields are retired — the tile carries the shape) + the
// per-tile cells. records[0] is ALWAYS the base (the default environment
// block, the old zone-zero semantic). The cells ride a separate BDA; a
// cell = (lower/mid/upper record + splitY/midHiY + weights) — see
// EnvTileCell.
#define ENV_TILE_MAX 64
struct EnvTileRecord
{
    float4 sky;             // rgb skyTint, a ambientI
    float4 fog;             // rgb fogColor, a fogDensity
    float4 vol;             // x volumetrics ratio, y volumetricDensity, zw reserved
};
struct EnvTileRecordsBuffer
{
    EnvTileRecord records[ENV_TILE_MAX];
};
// One 64 m world tile (a subcell column): up to THREE records — the
// dominant record BELOW and ABOVE splitY (the vertical split carries
// underwater/interior half-spaces) plus an optional mid BAND bounded by
// [splitY, midHiY] (the enclosed room: base -> interior -> base).
// lower==upper + splitY=-FLT_MAX + midRecord=0xFFFFFFFF = no split.
struct EnvTileCell
{
    uint lowerRecord;       // 0
    uint upperRecord;       // 4
    float splitY;           // 8
    float lowerWeight;      // 12
    float upperWeight;      // 16
    uint midRecord;         // 20 (0xFFFFFFFF = no mid band)
    float midHiY;           // 24 (the band's high edge)
    float midWeight;        // 28 (32 B total)
};

// Offsets mirror the C++ FrameUniforms in App/world_renderer.h (asserted
// there): env at 512, cubemap block at 640, specular knobs at 656,
// deferred-shadow fields at 672, SG-GI fields at 704, evsmMode at 748,
// decal fields at 752, regular-volume fields at 784, convex-volume
// fields at 1040, frameHot at 1296, dsVizParams at 1304, flags at 1312
// (total 1328); the append-only tail adds giLevelLayers at 1320,
// giVolumeMeanLayers at 1640, envZones at 1672, lightmaps at 1888,
// probe pre-mix at 1952, worldFields at 1976 (total 2000).
// The DS visualizer inputs live in the standalone DsVizParams payload
// (NOT inlined here): viz-gated #ifdefs inside this struct once made the
// CPU/GPU layout config-dependent.
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
    // SSAO (DORMANT since the SSGI rework): the lighting pass no longer
    // samples AO — Main exports the AO-sensitive lighting through the
    // aoTerm MRT target and the SSGI compose applies it (ssgi.h +
    // ssao_compose.frag.hlsl). The fields stay in the layout (offsets
    // asserted vs the C++ mirror); always 0xffffffff / 0.
    uint aoTextureIndex; // dormant (was: full-res composed AO bindless)
    float aoIntensity;   // dormant (was: ambient composite strength)
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
    // asserted in App/world_renderer.h): index + sampler + intensity +
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
    // ---- SG-GI LEGACY PROBE-VOLUME frame fields (the .sggi octree —
    // RETIRED): the runtime zeroes this whole block forever (no loader,
    // no walker reads it); the FIELDS stay because the FrameUniforms
    // layout is a shared C++/HLSL ABI contract with static_asserts on
    // both sides — removing them would renumber every later field. The
    // live GI rides giVolumes*/lm*/reg* below + the sky IBL.
    vk::BufferPointer<SgGiProbesBuffer> giProbes;   // 704 probe array (96 B each)
    vk::BufferPointer<SgGiGrid> giGrid;            // 712 level descriptors
    vk::BufferPointer<SgGiCellsBuffer> giCells;    // 720 uint2 (start,count)/cell
    vk::BufferPointer<SgGiIndexBuffer> giIndices;   // 728 u32 probe index list
    uint giEnabled;                                 // 736 (always 0 — legacy)
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
#if 0 // Lightmaps deprecated - SG 2D lightmaps (STATIC geometry; Engine/GI/lightmap.h).
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
#endif
    // ---- SG regular 3D probe volumes (DYNAMIC objects; Ready-at-Dawn
    // grids, Engine/GI/regular_volume.h). Hand-placed artist OBBs with
    // density params, baked per volume; 7 RGBA16F 3D textures each,
    // hardware trilinear filtering between neighboring probes. ----
    vk::BufferPointer<SgRegularObbBuffer> regObbs;           // 784 OBB array
    uint regLayers[56];                                       // 792 8 volumes x 7 layer slots
    uint regCount;                                             // 1016 active volumes
    uint regEnabled;                                           // 1020 1 = grid branch active
    uint4 regPad;                                              // 1024 (16-align tail)
    // ---- GI volumes (Engine/GI/cubemap_volume.h; BO3-style
    // cubemap-reprojection bake, TPP/HZD-style stacked volumes). Up to
    // SG_GI_VOLUME_MAX volumes from gi/<scene>.givolumemanifest, each a
    // dense SG probe grid in 7 RGBA16F 3D textures (hardware trilinear)
    // plus an 80 B header (fade-expanded AABB broad-phase + packed
    // world->uvw transform + plane span) into a shared plane pool —
    // see sg_lightmap_types.hlsli. The app uploads them priority-sorted
    // (LOW first); the compositor in sg_gi_volume.hlsli walks them HIGH first
    // with transmittance factors (E = irr(V_h)*w_h + ... + base*Π(1-w)) —
    // a weight-1 volume zeroes the transmittance and the base gather is
    // skipped entirely; density levels never share interpolation
    // neighbors. ----
    vk::BufferPointer<GiVolumesBuffer> giVolumes;               // 1040 headers + packed plane pool
    uint giVolumeLayers[56];                                    // 1048 8 volumes x 7 layer slots
    uint giVolumeCount;                                         // 1272 active volumes
    uint giVolumesEnabled;                                      // 1276 1 = GI-volume branch active
    // ---- GI-volume tile masks: the tile cull's per-tile volume
    // bitmask (bit v = volume v reaches this 16x16 tile; conservative
    // screen-space test, NO depth-range cut — fog-march samples live in
    // front of the nearest opaque surface). Published under the decal
    // tile-list freshness contract (address + grid together, zeros
    // together): giVolumeTileCountX == 0 means no tile cull refreshed the
    // masks this view/frame — the shader then walks every volume (the
    // per-volume AABB reject in SgGiVolumeWeight keeps that path correct).
    // Same grid as the light/decal tiles (pc.fpTileSize/fpTileCountX). ----
    vk::BufferPointer<GiVolumeTileMaskBuffer> giVolumeTileMasks; // 1280 per-tile volume bitmask
    uint giVolumeTileCountX;                                     // 1288 tile grid width (0 = no mask)
    uint giVolumeTileSize;                                       // 1292 tile size (16)
    // ---- FramePSHot slab (FS-only hot struct, see FramePSHot below): the
    // pixel shader reaches the hot slab through FrameUniforms (FramePSHot
    // hot = frame.frameHot.Get()) instead of a pc member — a pc member
    // shifted every sub-192 pc field and broke the HLSL mirror structs
    // (the 10/1 device-loss crash; see Engine/Renderer/Renderer.h). Filled per
    // view slab by the app (UpdateFrameUniforms/BuildSecondaryFrameUniforms).
    vk::BufferPointer<FramePSHot> frameHot;                   // 1296 this view's hot slab
    // ---- Deferred-shadow visualizer params (paper 5.3/5.4): BDA of this
    // view's DsVizParams slice (standalone 32 B payload, per-flight ring +
    // zero slice — see deferredshadow.hlsli). Deliberately a BDA, not
    // inlined fields: viz-gated #ifdefs inside this struct once made the
    // CPU/GPU layout config-dependent. This field is UNCONDITIONAL (same
    // offset/size in every build); only its fill (app) and read
    // (DsVisualize) are DS_DEBUG_VIZ-gated. ----
    vk::BufferPointer<DsVizParams> dsVizParams;               // 1304 (zero slice = viz off)
    // ---- Per-frame feature flags (bitmask; upper bits reserved). Lets
    // shaders skip off-by-default payloads (like dsVizParams above)
    // without a BDA chase. Unconditional like every other field. ----
    uint64_t flags;                                              // 1312 kFrameFlag* bits (C++ mirror)
    // ---- Dense octree resample (the BC6H Texture3D runtime path): per
    // level 9 SG-lobe RGB layers (BC6H 3D) + 1 weight layer (R16F 3D) in
    // the shared volumeTextures bindless array; UINT32_MAX = absent level
    // (beyond the baked levelCount). The diffuse sampler trilinear-
    // filters these instead of walking probe lists per pixel (the probes
    // never move — baked once at load). Must match the C++ mirror +
    // static_asserts (giLevelLayers at 1320, 80 slots).
    uint giLevelLayers[80];                                      // 1320 8 levels x (9 amp + 1 weight)
    // ---- GI-volume isotropic-mean textures (the volumetric fog's
    // one-fetch-per-volume payload — PackGpuMeanLayer +
    // SgGiVolumeSampleMean, blended by SgGiVolumeRayComposite): 8 bindless
    // slots, UINT32_MAX =
    // none. APPEND-ONLY tail: every pinned offset above stays put. ----
    uint giVolumeMeanLayers[8];                                  // 1640 8 volumes x 1 mean slot
    // ---- World env tiles (Engine/WorldPartition/WorldEnvTiles.h — the
    // runtime rework of the OBB-volume zone system): envZones is the RAW
    // BDA of the EnvTileRecord array (records[0] = the base/default look);
    // envZoneCount = live records (0 = every consumer takes the plain env
    // path); envZoneFlags bit0 = tiles live, bit1 = ANY live record
    // authors volumetric fog (a positive absolute volumetricDensity — the
    // volumetric pass's zone-density early-out leg; packed CPU-side from
    // the same WorldEnvTiles scan the volOn gates read). The old reserved
    // block is
    // re-split IN PLACE into the tile-grid header + a shorter reserved —
    // every pinned offset below stays put (lmLayers@1888, probe@1952,
    // worldFields@1976). APPEND-ONLY tail. ----
    uint64_t envZones;                                           // 1672 EnvTileRecord array BDA (raw)
    uint envZoneCount;                                           // 1680 live records (incl. base)
    uint envZoneFlags;                                           // 1684 bit0 = tiles live, bit1 = zone fog authored
    float envTileMinX;                                            // 1688 tile-grid min corner X (m)
    float envTileMinZ;                                            // 1692 min corner Z
    uint envTileDimX;                                             // 1696 tiles per axis (64 m each)
    uint envTileDimZ;                                             // 1700
    uint64_t envTileCells;                                        // 1704 the EnvTileCell grid BDA (raw)
    uint envTileReserved[42];                                     // 1712 (keeps lmLayers@1888 pinned)
    // ---- SG 2D lightmaps (STATIC geometry): atlas layers ride the
    // bindless texture slots (mirror of the C++ APPEND-ONLY tail — every
    // pinned offset above stays put). lmLayerKind: 0 = 9 RGBA16F stream
    // layers, 1 = BC6H one-texture-per-lobe (texture l = lobe l).
    uint2 lmPad0_;                                                // 1880 (16-align to lmLayers@1888)
    uint lmLayers[12];                                            // 1888 atlas bindless slots
    uint lmEnabled;                                               // 1936 1 = lightmap branch active
    uint lmLayerKind;                                             // 1940 0 = RGBA16F stream, 1 = BC6H
    uint2 lmPad_;                                                 // 1944 (16-align; struct was 1952)
    // ---- GPU probe pre-mix (docs/probe_premix_plan.md; the Remember Me
    // architecture, GPU-only selection - mirror of the C++ append-only
    // tail). probePremixIndex = the BLENDED cubemap's bindless index
    // (0xffffffff = off: the per-pixel walk owns the specular env
    // unchanged). probePremixMips = the blended chain's mip count (the
    // roughness LOD clamp). probePremixSel = the weights pass's
    // selection buffer BDA - the single-fetch branch reads the DOMINANT
    // probe's packed parallax box straight from it (no CPU readback, no
    // staging, no classification anywhere; GPU-owned selection written
    // by the weights pass each frame it runs). ----
    uint probePremixIndex;                                         // 1952 blended cubemap (off = 0xffffffff)
    uint probePremixMips;                                          // 1956 blended chain mip count
    uint64_t probePremixSel;                                       // 1960 selection BDA (weights pass output)
    uint2 probePremixPad_;                                         // 1968 (16-align tail; struct was 1984)
    // ---- World voxel fields (Engine/WorldFields — the atlas-backed
    // virtual world volume system; sampler in world_field.hlsli, layout
    // contract in Engine/WorldFields/WorldFieldGpu.h). worldFields is
    // the BDA of this frame's field/page-table storage buffer (0 = the
    // subsystem is off — SampleWorldField returns zeros); the buffer and
    // its BDA are stable for the system's lifetime, the CONTENT is
    // re-published whenever residency moves. worldFieldCount = live
    // fields (field ids are registration order, fog first). APPEND-ONLY
    // tail: every pinned offset above stays put. ----
    uint64_t worldFields;                                          // 1976 field/page-table BDA (0 = off)
    uint worldFieldCount;                                          // 1984 live GPU fields
    uint worldFieldsPad_;                                          // 1988 (16-align tail; struct was 1992)
    // ---- GI-volume VISIBILITY layers (the through-wall rejection —
    // see sg_gi_volume.hlsli's SgGiVolumeSample): 2 RGBA8_UNORM 3D
    // textures per volume (visA = +x,-x,+y,-y obstacle distances;
    // visB = +z,-z, the forward-cell clean flag, spare — each distance
    // quantized unorm8 over [0, 15] SPACING UNITS). UINT32_MAX =
    // absent (the volume predates the visibility bake — the sample
    // takes the plain hardware trilerp, the pre-visibility behavior).
    // APPEND-ONLY tail: every pinned offset above stays put. ----
    uint giVolumeVisLayers[16];                                    // 1992 8 volumes x 2 vis layer slots
    // ---- The coarse VOLUMETRIC shadow tile (the fog god-ray pass):
    // the dedicated sun-oriented ortho + the tile's normalized atlas
    // rect (the identity (0,0,1,1) over its OWN dedicated atlas - kept
    // as a rect so the mapping stays source-agnostic). APPEND-ONLY
    // tail. enabled = 0 -> the fog sun leg falls back to the CSM tap
    // or fully lit. ----
    // The EXPLICIT pad is load-bearing: DXC's BufferPointer layout
    // does NOT implicitly 16-align a float4x4 following a uint array -
    // without this member the shader's matrix reads 8 bytes early (the
    // C++ pad + a shifted matrix: garbage clip.w, the coarse leg's
    // "empty tile" root cause). The C++ mirror is FrameUniforms::
    // fogShadowPad_ (WorldRenderer.h). ----
    float2 fogShadowPad_;                                           // 2056 (the mat4 16-align pad, explicit)
    float4x4 fogShadowVP;                                          // 2064
    float4 fogShadowRect;                                          // 2128 xy origin + zw size
    float fogShadowBias;                                           // 2144 NDC depth bias
    uint fogShadowEnabled;                                         // 2148
    // ---- Sun contact shadows (Engine/Rendergraph/contact_shadow.h,
    // the pre-Main depth raymarch): the visibility texture's bindless
    // index, sampled by the lit body's sun block
    // (default_lit_body.hlsli, min'd with the CSM/EVSM factor).
    // UINT32_MAX = the pass didn't run this frame (the publish gate
    // mirrors the emission gate exactly - the probe-premix lesson).
    // APPEND-ONLY into the old 8-byte tail pad: fields end 2156, the
    // remaining 4 pad to the unchanged 2160 total - NO size churn, no
    // later offset moves. ----
    uint contactShadowIndex;                                        // 2152 (bindless visibility)
};

// Deferred-shadow push constants + shared helpers (needs FrameUniforms
// and ForwardPlusLight above; includes deferredshadow_types.hlsli again,
// which is guarded and free-standing).
#include "deferredshadow.hlsli"

// Scene push-constant mirror lives in push_constants.hlsli (single source
// of truth: tools/push_constants_idl/push_constants.yaml; verify with
// python tools/push_constants_idl/codegen.py --check). Included here so
// legacy `#include "common.hlsli"` translation units keep working; new
// shaders may include push_constants.hlsli directly for fine-grained deps.
#include "push_constants.hlsli"

// Specialization constants (driver constant-folds these per permutation).
// REAL DXC spec-constant form: [[vk::constant_id(N)]] on a const with a
// default — the previous [[vk::spec_constant(...)]] was not a DXC
// attribute, so DXC silently ignored it and packed these into the
// auto-generated $Globals cbuffer (auto-bound at set 0 binding 2 — the
// validation "descriptor type mismatch / never updated" spam: the engine
// binds no Globals slot anymore). constant_id generates NO descriptor:
// unspecialized pipelines fold the defaults.
[[vk::constant_id(SPEC_GI_DEBUG_MODE)]] const uint giDebugMode = 0; // 0=off, 1=flat irradiance, 2=source palette
[[vk::constant_id(SPEC_EVSM_MODE)]]   const uint evsmMode = 0;     // 0=PCF depth, 1=EVSM moments
[[vk::constant_id(SPEC_SSR_ENABLED)]] const uint ssrEnabled = 0;   // 0=disable SSR, 1=enable SSR
[[vk::constant_id(SPEC_GI_INTENSITY)]] const float giIntensity = 1.0f; // intensity scale
[[vk::constant_id(SPEC_GI_VARIANT)]]  const uint giVariant = 0;     // 0=GI_NONE, [1 RETIRED — GI_OCTREE deleted with the .sggi runtime; the ID stays reserved so the shared specialization ABI never renumbers], 2=GI_REGULAR, 3=GI_VOLUMES
[[vk::constant_id(SPEC_LIT_SPECULAR)]] const uint litSpecular = 1;   // 0=Lambert-only far shading, 1=full GGX specular
[[vk::constant_id(SPEC_LIT_NORMALMAP)]] const uint litNormalMap = 1; // 0=geometric normal, 1=tangent-space normal fetch

#if !defined(TONEMAP_PASS) && !defined(BLOOM_PASS) && !defined(MOTIONBLUR_PASS) && !defined(MOTIONBLUR_TILES_PASS) && !defined(MOTIONBLUR_TILEDILATE_PASS) && !defined(SSAO_PASS) && !defined(GTAO_PASS) && !defined(SAO_PASS) && !defined(FORWARDPLUS_PASS) && !defined(UI_PASS) && !defined(GFXFILTER_PASS) && !defined(GFXFILTERED_PASS) && !defined(HZB_PASS) && !defined(GFXSEED_PASS) && !defined(EXPOSURE_PASS) && !defined(SMAA_PASS) && !defined(DS_PASS) && !defined(EVSM_BLUR_PASS) && !defined(EVSM_RESOLVE_PASS) && !defined(SDSM_STATS_PASS) && !defined(DOF_PASS) && !defined(SSR_PASS) && !defined(VOLUMETRIC_PASS) && !defined(VOLUMETRIC_COMPOSITE_PASS) && !defined(PARTICLES_PASS) && !defined(UVFLAT1_PASS) && !defined(DEFAULT_DIRECT_PASS) && !defined(SHADOW_DIRECT_PASS) && !defined(PLACEMENT_INSTANCED_PASS) && !defined(PROBEBALL_PASS) && !defined(SPD_PASS) && !defined(GFXMESH_PASS) && !defined(GFXBATCHCULL_PASS) && !defined(GFXDRAWCULL_PASS) && !defined(GFXSKINBOUNDS_PASS) && !defined(GFXSKINNING_PASS) && !defined(GFXCONEDEBUG_PASS) && !defined(TERRAIN_NOPC) && !defined(PROBEPREMIX_WEIGHTS_PASS) && !defined(PROBEPREMIX_MIX_PASS) && !defined(GFXSKINNING_PASS) && !defined(MATBALL_PASS) && !defined(SSGI_DIFFUSE_PASS) && !defined(GODRAYS_PASS) && !defined(CONTACT_SHADOW_PASS)
// ENGINE_HAS_PC: this translation unit sees the scene-wide `pc` push
// constants. Passes with their own push layout (FORWARDPLUS/HZB/DS/...)
// leave it undefined so headers can guard pc-dependent helpers (see
// shadows.hlsli FromPC wrappers).
#define ENGINE_HAS_PC
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;
#endif
