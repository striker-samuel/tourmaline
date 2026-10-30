// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Forward+ tiled light culling builder.
//
// Ports the classic Forward+ stages into the engine builder pattern used by
// BloomChain / CascadedShadowAtlas / HdrTarget:
//   depth prepass (geometry VS only, writes scene depth) ->
//   compute tile cull (depth min/max + frustum vs point/spot volumes) ->
//   forward opaque samples per-tile lists.
//
// Directional lights + CSM stay on their existing path; only point/spot
// lights are tiled. Buffers are addressed via buffer-device-address push
// constants (no new descriptor types); depth is sampled bindless.
//
// Usage (app/src/main.cpp):
//   smvk::ForwardPlus fp;
//   fp.Ensure(ctx, rg, swapExtent, desc, depthViewHandle);
//   // per frame:
//   fp.Ensure(ctx, rg, swapExtent, desc, depthViewHandle); // resize-safe
//   auto& pre = fp.AddDepthPrepass(rg, depthLogical, swapExtent);
//   pre.SetRecordingFunc(... draw opaque casters with depthProgram ...);
//   auto& cull = fp.AddCullPass(rg, depthLogical, lightsLogical,
//       tileCountsLogical, tileIndicesLogical);
//   cull.SetRecordingFunc(... bind cullProgram, push ForwardPlusCullPush,
//       Dispatch(tileCountX, tileCountY, 1) ...);
//   // main opaque: Load depth, sample tile lists via extended PushConstants.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>

namespace smvk {

// ---------------------------------------------------------------------------
// GPU light (must match HLSL ForwardPlusLight in common.hlsli / forwardplus)
// 96 bytes, 16-byte aligned.
//
// Shadow fields (spots AND points):
//  shadowTile      – SPOTS: tile index into the local-light shadow atlas
//                    descriptor buffer. POINTS: the FIRST of SIX consecutive
//                    cube-face tiles (the sampler adds the dominant-axis
//                    face number, see shadows.hlsli GetPointShadowFactorTile).
//                    -1 when this light casts no shadow this frame.
//  shadowMapIndex  – bindless index of the atlas the light samples: the D32
//                    tile atlas for points (always) and spots (PCF mode);
//                    the blurred EVSM moments for spots in EVSM mode. -1
//                    when unshadowed.
//  shadowBias      – depth bias applied to the receiver depth (perspective).
// `flags` bit kForwardPlusFlag_CastShadow is the *request* flag: the app sets
// it on lights allowed to cast shadows; per-frame selection then assigns
// shadowTile/MapIndex only to the top-N casters (see spotshadow.h - spots
// take 1 tile each, points take 6).
//
// Light cookie (spot only): cookieIndex is the bindless texture index of a
// gobo projected through the spot cone (cone-UV convention matches the
// spot-shadow projection, see light.hlsli SpotConeUV). -1 = no cookie.
//
// Light function: per-light procedural intensity modulation evaluated per
// pixel per light (Unreal-style light functions, but analytic instead of a
// material). functionType selects the curve, functionSpeed/functionScale
// parameterize it (see kForwardPlusFunction_*).
// ---------------------------------------------------------------------------
struct alignas(16) ForwardPlusLight {
	glm::vec4 color = glm::vec4(1.0f);      // 0  linear RGB + pad
	glm::vec3 position = { 0, 0, 0 };       // 16 world pos
	float range = 1.0f;                     // 28 cutoff distance (m)
	glm::vec3 direction = { 0, -1, 0 };     // 32 normalized, spot axis
	float intensity = 0.0f;                 // 44 candela (lm/sr)
	float innerAngle = 0.0f;                // 48 radians (spot)
	float outerAngle = 0.0f;                // 52 radians (spot)
	int32_t type = 0;                       // 56 0=point, 1=spot
	int32_t flags = 0;                      // 60 bit0 = shadow request
	int32_t shadowTile = -1;                // 64 tile in spot atlas, -1 = none
	int32_t shadowMapIndex = -1;            // 68 bindless atlas index, -1 = none
	float shadowBias = 0.0f;                // 72 receiver-depth bias
	float pad2_ = 0.0f;                     // 76
	int32_t cookieIndex = -1;               // 80 bindless gobo texture, -1 = none
	int32_t functionType = 0;               // 84 kForwardPlusFunction_*
	float functionSpeed = 1.0f;             // 88 animation speed multiplier
	float functionScale = 1.0f;              // 92 modulation depth / pattern scale
};
static_assert(sizeof(ForwardPlusLight) == 96);
static_assert(sizeof(ForwardPlusLight) % 16 == 0);

static constexpr uint32_t kForwardPlusPoint = 0;
static constexpr uint32_t kForwardPlusSpot = 1;
// Reflection probe (specular IBL bucketing): a probe rides the light
// buffer + tile lists as a third entry type instead of a parallel
// bucketing system. Packing into the 96-byte light (must match the HLSL
// comment on ForwardPlusLight in common.hlsli):
//   position/direction — position.xyz = probe center (world);
//                        direction = (fadeRadius, cubemapIndex, mipLevels)
//                        with cubemapIndex/mipLevels bit-exact in float
//                        (bindless < 2^24, mips small).
//   range              — influenceRadius + fadeRadius (the tile cull's
//                        conservative outer sphere, tested generically).
//   color.rgb          — the DEPTH-FITTED box's MINUS-plane distances
//                        (center - boxMin, per axis): one scalar per cube
//                        wall. The intensity/innerAngle/outerAngle float
//                        triple carries the PLUS-plane distances
//                        (boxMax - center). Zero (a probe packed without
//                        a box) means "no fitted box" — the shader falls
//                        back to the symmetric influence sphere.
//   type               — kForwardPlusProbe. angles/intensity are
//                        reinterpreted above, no shadow/cookie/function
//                        (all -1/0), so a misread contributes nothing.
// Probes sort LAST in the upload (lights keep their slots AND their
// indices: spot selection, DS lists, debug UI and every `li >= nlights`
// guard keep working untouched with nlights = lights-only). The cull
// push carries lights+probes; lit/DS pushes carry lights-only.
static constexpr int32_t kForwardPlusProbe = 2;
// Reserve of the 1024 light-buffer slots for probe entries (see
// FP_PROBE_RESERVE in the app): lights are capped early so appended
// probes can never truncate a selected shadow spot. Per-PIXEL cost is
// NOT this number: the tile lists bucket at most MAX_FP_PROBE_TILE_SLOTS
// (4) probes per tile (common.hlsli) and the specular walk blends the
// top 2 of those - 64 resident probes is a density/streaming budget,
// not a shading cost.
static constexpr uint32_t kForwardPlusMaxProbes = 64;
// Dedicated per-tile probe slots in the tile index buffer (shader
// MAX_FP_PROBE_TILE_SLOTS): stride per tile is maxLightsPerTile + this,
// lights pack [0..maxLightsPerTile), probes the tail slots — light
// saturation can never pop probes from a tile. 8 (was 4): dense
// fitted-pocket placement puts >4 covering probes on one 16 px tile near
// geometry, and the first-come overflow changed the blend weight set
// across tile boundaries (16 px tiles in the raw-cubemap debug view).
// Must stay in sync with common.hlsli.
static constexpr uint32_t kForwardPlusProbeTileSlots = 8;

// Relevance-sort capacity of the cull's tile lists (shader
// MAX_FP_TILE_SORT_SLOTS in common.hlsli): the surface light-list and
// fog-list row strides clamp to it in the shader, so a desc configured
// beyond it would allocate C++ rows the GPU never addresses — Create()
// rejects instead (the shader-side FpTileLightSlots clamp stays as
// defense in depth). Must stay in sync with common.hlsli.
static constexpr uint32_t kForwardPlusMaxTileSortSlots = 32;
// Per-tile decal-list WRITE gate (shader MAX_FP_DECALS_PER_TILE): the
// cull writes at most this many entries per tile row; the row STRIDE is
// the desc's maxDecalsPerTile on both the producer and the consumer.
// Must stay in sync with common.hlsli.
static constexpr uint32_t kForwardPlusMaxDecalsPerTile = 16;

// ---------------------------------------------------------------------------
// Hierarchical tile-light culling (Final Fantasy XVI "Shadow Techniques",
// sections 2.1-2.3): the shared PHASE-1 artifact format. The classic
// forward+ cull scans every light per tile; the hierarchical path first
// builds, once per frame:
//   - zBins: kFpZBinCount view-distance bins over [near, far]; each bin
//     carries {minIdx, maxIdx, rangedMin, rangedMax} over the SORTED
//     light buffer (DsSortLightsByNearDistance order — nearest first).
//     A tile's depth interval resolves to a conservative sorted-index
//     window with TWO lookups via the ranged variants (figure 2b).
//   - coarse tile masks ("Light Mask Entry", figure 4): per
//     kFpTileGroupPx screen TileGroup, 1 uint of 32-light group bits +
//     kFpMaxLightGroups uints of 32-light bitmasks — 132 bytes per group,
//     1024 light bits. Conservative: a set bit is a NECESSARY condition
//     for the light to reach any pixel of the group (near-plane crossers
//     fall back to the whole grid), never a sufficient one.
// The tile cull then intersects the coarse mask with the zBin window at
// the 32-bit word level (DsWindowMask) and runs the EXACT per-light
// tests (side planes + depth slab) only on surviving bits. The zBin
// windows are conservative supersets of the exact depth cuts; each
// coarse-mask reject (group plane or projected rect) proves zero
// contribution to the group's pixels, so no light that reaches any
// pixel of a tile is ever dropped — the lists differ from the linear
// scan's only by that scan's own zero-contribution false positives.
// The CPU builders live in deferredshadow.h (DsBuildZBins /
// DsBuildTileMasks — pure math, differential-tested in
// test_deferredshadow.cpp); the per-flight GPU buffers are owned HERE so
// the DEFAULT forward+ path consumes them without the deferred-shadow
// pipeline active. deferredshadow.h aliases these as kDs* (single source
// of truth — the two consumers must never disagree on the format).
// ---------------------------------------------------------------------------
static constexpr uint32_t kFpZBinCount = 1024;
static constexpr uint32_t kFpMaxLightGroups = 32; // MAX_FP_LIGHTS / 32
static constexpr uint32_t kFpTileGroupPx = 64;    // coarse mask grid pitch
static constexpr size_t kFpZBinBytes = sizeof(uint32_t) * 4 * kFpZBinCount;
static constexpr uint32_t kFpTileMaskWords = 1 + kFpMaxLightGroups;

// Builds a probe entry for the upload tail (upload order stays
// widest-influence-first so tile overflow keeps the widest probes).
// boxMin/boxMax = the depth-fitted proxy volume (manifest "box" group);
// pass equal vectors to pack "no fitted box" (the shader then uses the
// symmetric influence sphere).
inline ForwardPlusLight MakeProbeLightEntry(const glm::vec3& center,
	float influenceRadius, float fadeRadius, uint32_t cubemapBindless,
	uint32_t mipLevels, const glm::vec3& boxMin, const glm::vec3& boxMax) {
	ForwardPlusLight L{};
	L.position = center;
	L.range = influenceRadius + fadeRadius;
	L.direction = glm::vec3(fadeRadius,
		static_cast<float>(cubemapBindless), static_cast<float>(mipLevels));
	// Per-plane wall scalars (center-relative): color.rgb = the minus
	// walls, the intensity/inner/outer triple = the plus walls. The
	// all-zero packing means "no box" (legacy symmetric fallback).
	const glm::vec3 dNeg = glm::max(center - boxMin, 0.0f);
	const glm::vec3 dPos = glm::max(boxMax - center, 0.0f);
	L.color = glm::vec4(dNeg, 1.0f);
	L.intensity = dPos.x;
	L.innerAngle = dPos.y;
	L.outerAngle = dPos.z;
	L.type = kForwardPlusProbe;
	L.shadowTile = -1;
	L.shadowMapIndex = -1;
	L.cookieIndex = -1;
	return L;
}
// Per-light controllable flag: light *may* cast a spot shadow. The frame
// selection (upper limit + screenspace/distance heuristic) decides which
// requesting spots actually get a tile (shadowTile >= 0).
static constexpr int32_t kForwardPlusFlag_CastShadow = 1 << 0;

// Light function types (must match light.hlsli EvaluateLightFunction).
// Flicker = stepped candle-like noise over time; Pulse = sine breathing;
// Bands = moving diagonal bands across the spot cone (spatial; spots only,
// points fall back to Pulse). Speed scales the time axis, Scale how deep the
// modulation dips (1 = down to black, 0 = disabled).
static constexpr int32_t kForwardPlusFunction_None = 0;
static constexpr int32_t kForwardPlusFunction_Flicker = 1;
static constexpr int32_t kForwardPlusFunction_Pulse = 2;
static constexpr int32_t kForwardPlusFunction_Bands = 3;

// ---------------------------------------------------------------------------
// GPU decal (must match HLSL ForwardPlusDecal in common.hlsli).
// 128 bytes, 16-byte aligned.
//
// A decal is an oriented box projected along its local +Z: the projection
// axis (xform2's xyz direction) points from the decal center TOWARD the
// receiving surface (like a camera looking down -Z). A surface receives the
// decal when the pixel's world position lands inside the unit cube
// (|local| <= 1, local = xformRows . worldPos) AND the geometric surface
// normal faces back toward the projector (dot(N, axisZ) < 0).
// UV = local.xy * 0.5 + 0.5 (V along the decal's local +Y).
//
// Tile culling tests the bounding sphere (bounds.xyz / bounds.w) against
// the tile frustum + depth range, exactly like point lights.
//
// material (int4): x = bindless albedo texture (RGBA: rgb color, a =
//   coverage mask), y = bindless normal map (tangent-space, xy in [0,1],
//   Z reconstructed on the unit sphere like engine normal maps), z = flags
//   (kDecalFlag_*), w = reserved (-1).
// color: rgb = linear tint (multiplies the albedo texture), a = opacity.
// params: x = normal strength, y = roughness target, z = metallic target,
//   w = depth fade start (opacity fades to 0 as |local.z| approaches 1;
//   keeps offset/curved surfaces from popping at the box faces).
// emissive: rgb = emissive add color, a = strength (0 = off; additive after
//   the material emissive so decals can double as glow projections).
// ---------------------------------------------------------------------------
struct alignas(16) ForwardPlusDecal {
	glm::vec4 bounds = glm::vec4(0.0f);   // 0   xyz world center, w bounding radius
	glm::vec4 xform0 = glm::vec4(0.0f);   // 16  world->unit-cube row 0 (axisX/halfX, -c*axisX/halfX)
	glm::vec4 xform1 = glm::vec4(0.0f);   // 32  row 1
	glm::vec4 xform2 = glm::vec4(0.0f);   // 48  row 2 (projection axis in xyz)
	glm::ivec4 material = glm::ivec4(-1); // 64  albedoTex, normalTex, flags, -1
	glm::vec4 color = glm::vec4(1.0f);    // 80  rgb tint, a opacity
	glm::vec4 params = glm::vec4(1.0f);   // 96  normalStrength, roughTarget, metalTarget, depthFadeStart
	glm::vec4 emissive = glm::vec4(0.0f); // 112 rgb emissive add, a strength
};
static_assert(sizeof(ForwardPlusDecal) == 128);
static_assert(sizeof(ForwardPlusDecal) % 16 == 0);

// Channel flags (ForwardPlusDecal::material.z; must match common.hlsli).
static constexpr int32_t kDecalFlag_Albedo = 1 << 0;
static constexpr int32_t kDecalFlag_Normal = 1 << 1;
static constexpr int32_t kDecalFlag_Material = 1 << 2; // rough/metal targets

// Builds a decal from a placement frame: center + projection axis (points
// from the decal toward the surface) + up hint (texture V direction),
// half extents (x, y, depth) and the material knobs (see ForwardPlusDecal).
// yawRadians spins the decal in its own plane (splat rotation). The up hint
// is orthogonalized against axisZ with a safe fallback for near-parallel
// inputs (floor splats pass world up with axisZ = -Y).
inline ForwardPlusDecal MakeDecal(const glm::vec3& center, glm::vec3 axisZ,
	const glm::vec3& upHint, const glm::vec3& halfExtents,
	int32_t albedoTex, int32_t normalTex, int32_t flags, float opacity,
	const glm::vec3& tint, float emissiveStrength, const glm::vec3& emissiveColor,
	float normalStrength, float roughnessTarget, float metalTarget,
	float depthFadeStart, float yawRadians)
{
	axisZ = glm::normalize(axisZ);
	glm::vec3 up = upHint - axisZ * glm::dot(upHint, axisZ);
	if (glm::length(up) < 1e-4f) {
		up = glm::vec3(0.0f, 1.0f, 0.0f);
		up -= axisZ * glm::dot(up, axisZ);
		if (glm::length(up) < 1e-4f) {
			up = glm::vec3(1.0f, 0.0f, 0.0f);
			up -= axisZ * glm::dot(up, axisZ);
		}
	}
	glm::vec3 axisX = glm::normalize(glm::cross(up, axisZ));
	glm::vec3 axisY = glm::normalize(glm::cross(axisZ, axisX));
	// In-plane yaw (rotate X/Y around the projection axis).
	const float s = std::sin(yawRadians);
	const float c = std::cos(yawRadians);
	const glm::vec3 rx = axisX * c + axisY * s;
	const glm::vec3 ry = axisY * c - axisX * s;
	const float hx = std::max(halfExtents.x, 1e-4f);
	const float hy = std::max(halfExtents.y, 1e-4f);
	const float hz = std::max(halfExtents.z, 1e-4f);

	ForwardPlusDecal d{};
	d.bounds = glm::vec4(center, glm::length(halfExtents));
	const glm::vec3 ex = rx / hx;
	const glm::vec3 ey = ry / hy;
	const glm::vec3 ez = axisZ / hz;
	d.xform0 = glm::vec4(ex, -glm::dot(center, ex));
	d.xform1 = glm::vec4(ey, -glm::dot(center, ey));
	d.xform2 = glm::vec4(ez, -glm::dot(center, ez));
	d.material = glm::ivec4(albedoTex, normalTex, flags, -1);
	d.color = glm::vec4(tint, opacity);
	d.params = glm::vec4(normalStrength, roughnessTarget, metalTarget, depthFadeStart);
	d.emissive = glm::vec4(emissiveColor, emissiveStrength);
	return d;
}

struct ForwardPlusDesc {
	uint32_t tileSize = 16;
	uint32_t maxLights = 1024;
	// Surface list: up to maxLightsPerTile SLOTS per tile, relevance-
	// sorted by the cull (nearest reach first; spots pinned ahead of
	// points — see forwardplus.cs.hlsl). Overflow pops the FARTHEST
	// candidates, so the pop is a graceful distance fade, not the
	// first-come hard edge that showed as tile seams. Capped at
	// kForwardPlusMaxTileSortSlots (the shader's sort capacity) —
	// Create() rejects anything beyond.
	uint32_t maxLightsPerTile = 16;
	// Per-tile FOG list cap (separate lists for the volumetric march —
	// ForwardPlusCullPush::fogTile*; march-range candidacy, no slab cuts).
	// Capped at kForwardPlusMaxTileSortSlots like the surface list
	// (same sorted-insertion capacity) — Create() rejects beyond.
	uint32_t maxFogPerTile = 16;
	// Decals share the light tiling (same tile size / tile counts): the
	// cull pass fills parallel per-tile decal index lists in the same
	// dispatch. maxDecalsPerTile is the ROW STRIDE (the write gate is
	// the shader's MAX_FP_DECALS_PER_TILE); capped at
	// kForwardPlusMaxDecalsPerTile — Create() rejects beyond.
	uint32_t maxDecals = 256;
	uint32_t maxDecalsPerTile = 16;
	const char* name = "forwardplus";
};

// Must match forwardplus.cs.hlsl ForwardPlusCullPush (120 bytes).
struct ForwardPlusCullPush {
	uint64_t frameUniforms = 0;
	uint64_t lights = 0;
	uint64_t tileCounts = 0;
	uint64_t tileIndices = 0;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t lightCount = 0;
	uint32_t tileSize = 16;
	uint32_t maxLightsPerTile = 128;
	// Per-tile convex-volume bitmask output (replaces the old pad0/pad1
	// tail — every other field offset is unchanged).
	uint64_t volumeMasks = 0;
	// ---- Per-tile FOG lists (separate from the surface lists) ----
	// The volumetric march samples OUTSIDE the surface depth slab (in
	// front of the nearest opaque surface), so the surface lists' slab
	// cuts would drop lights the fog needs (the fog split on the tile
	// grid at depth discontinuities - the GI-volume mask leg's no-cut
	// rationale, applied to a dedicated list so the surface lists stay
	// tight). Candidates: side planes + a marching-range far cut at
	// fogMaxDistance (no slab cuts); packed up to desc.maxFogPerTile
	// slots per tile, relevance-sorted (see the shader). Everything is
	// zeroed on the same freshness contract as the mask leg.
	uint64_t fogTileCounts = 0;
	uint64_t fogTileIndices = 0;
	// March-range far cut (the volumetricDesc.maxDistance the pass will
	// actually run with).
	float fogMaxDistance = 80.0f;
	// Fog row stride (desc.maxFogPerTile — must match the volumetric
	// push's fogMaxPerTile so both sides address the same rows).
	uint32_t maxFogPerTile = 16;
	// 0 = skip the fog track entirely (the volumetric pass is OFF this
	// frame — no consumer of the lists; the per-light fog candidacy +
	// sorted insertion per tile is real work, not free dead stores).
	// 1 = build the fog lists (the volumetric pass will run).
	uint32_t fogListsWanted = 0;
	// ---- Hierarchical culling phase-1 artifacts (FFXVI 2.1-2.3) ----
	// Both zero = no phase-1 upload this frame (deferred shadows owns the
	// light lists, the toggle is off, or a resize frame raced the upload)
	// -> the cull falls back to the linear scan over all lights. The
	// fallback is also the A/B reference (TOURMALINE_FP_MASKS=0 pins it
	// headless): the mask/zBin gates never drop a light that reaches any
	// pixel of the tile (each coarse-mask reject is a proof of zero
	// contribution), so the mask path's image matches the reference —
	// except in SATURATED tiles, where it keeps a SUPERSET of the
	// reference's true contributors (the reference's sorted insertion
	// can waste slots on zero-contribution false positives).
	uint64_t zbins = 0;      // DsZBinBuffer flight slice
	uint64_t tileMasks = 0;  // DsTileLightMaskBuffer flight slice
	// Lights-only count: the phase-1 builders ran over the light vector
	// BEFORE the probe tail was appended, so the masks/zBins cover
	// [0, maskLightCount) and the cull walks the probe tail
	// [maskLightCount, lightCount) linearly (probes are <= 64 entries —
	// the tail scan is cheaper than extending the phase-1 format for
	// them).
	uint32_t maskLightCount = 0;
	// Coarse-mask grid width in groups (ceil(extent / kFpTileGroupPx));
	// the shader needs it for the groupIdx = gy * countX + gx layout.
	uint32_t maskGroupCountX = 0;
};
static_assert(sizeof(ForwardPlusCullPush) == 120);

inline constexpr const char* kForwardPlusCsPath = "content/shaders/forwardplus.cs.hlsl.spv";
inline constexpr const char* kForwardPlusDepthVsPath = "content/shaders/default.vert.hlsl.spv";

struct ForwardPlus {
	ForwardPlusDesc desc{};
	Extent2D extent{ 0, 0 };
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t tileCount = 0;

	// Tile buffers (physical, owned). Single copy reused across frames;
	// GPU serializes via rendergraph barriers (compute write -> frag read).
	Holder<BufferHandle> tileCountsBuffer;
	Holder<BufferHandle> tileIndicesBuffer;
	LogicalBufferHandle tileCountsLogical{};
	LogicalBufferHandle tileIndicesLogical{};
	// Decal tile lists (same tiling as the light lists; the cull pass
	// fills both in one dispatch). Stride is desc.maxDecalsPerTile.
	Holder<BufferHandle> decalTileCountsBuffer;
	Holder<BufferHandle> decalTileIndicesBuffer;
	LogicalBufferHandle decalTileCountsLogical{};
	LogicalBufferHandle decalTileIndicesLogical{};
	// Per-tile FOG lists (volumetric march candidates — see
	// ForwardPlusCullPush::fogTile*). Separate buffers so the surface
	// lists' stride/probe layout (and every surface consumer) stays
	// untouched. Stride is desc.maxFogPerTile.
	Holder<BufferHandle> fogTileCountsBuffer;
	Holder<BufferHandle> fogTileIndicesBuffer;
	LogicalBufferHandle fogTileCountsLogical{};
	LogicalBufferHandle fogTileIndicesLogical{};
	// GI-volume tile masks (same tiling; the cull's volume leg
	// packs a <=8-bit survivor bitmask per tile — see sg_gi_volume.hlsli).
	Holder<BufferHandle> volumeTileMasksBuffer;
	LogicalBufferHandle volumeTileMasksLogical{};
	// ---- Hierarchical phase-1 rings (zbins + coarse tile masks) ----
	// PER-FLIGHT, unlike the tile buffers above: these are CPU-uploaded
	// at record time (UpdateForwardPlusLights runs before Execute), so a
	// single shared buffer would let frame N+1's upload clobber frame N's
	// still-executing cull — the same ringing rationale as
	// DeferredShadows. Sizing: zBins is FIXED (kFpZBinBytes); the mask
	// buffer is dynamic (groupCount * kFpTileMaskWords uints — no
	// kDsMaxTileGroups-style cap; the 32768-tile gate above bounds the
	// extent harder).
	Holder<BufferHandle> zBins[smvk::limits::MaxFramesInFlight]{};
	Holder<BufferHandle> tileMasks[smvk::limits::MaxFramesInFlight]{};
	LogicalBufferHandle zBinsLogical[smvk::limits::MaxFramesInFlight]{};
	LogicalBufferHandle tileMasksLogical[smvk::limits::MaxFramesInFlight]{};
	// Coarse-mask grid (ceil(extent / kFpTileGroupPx)); the CPU builder
	// (DsBuildTileMasks) derives the identical grid from the same extent.
	uint32_t maskGroupCountX = 0;
	uint32_t maskGroupCountY = 0;

	[[nodiscard]] bool MasksValid() const {
		for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
			if (zBins[f].empty() || tileMasks[f].empty())
				return false;
		}
		return maskGroupCountX != 0 && maskGroupCountY != 0;
	}

	// Depth bindless (sampled D32 as float in compute). Rebuilt when the
	// depth view handle changes (swapchain recreate).
	Holder<BindlessImageHandle> depthBindless;
	uint32_t depthBindlessIndex = UINT32_MAX;
	ImageViewHandle depthViewHandle{};

	// Programs (owned). Cull CS + depth-prepass VS-only.
	Holder<ShaderModuleHandle> cullModule;
	ShaderProgram cullProgram{};
	Holder<ShaderModuleHandle> depthVsModule;
	ShaderProgram depthProgram{};

	[[nodiscard]] bool Valid() const {
		return tileCount != 0 && !tileCountsBuffer.empty() && !tileIndicesBuffer.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return cullProgram.shaderCount != 0 && depthProgram.shaderCount != 0 &&
			!cullModule.empty() && !depthVsModule.empty();
	}

	static uint32_t TileCountFor(uint32_t dim, uint32_t tileSize) {
		tileSize = tileSize == 0 ? 16 : tileSize;
		return (dim + tileSize - 1) / tileSize;
	}
	// Coarse-mask grid pitch (kFpTileGroupPx): the same ceil-div the CPU
	// mask builder (DsBuildTileMasks) applies to the same extent.
	static uint32_t TileGroupCountFor(uint32_t dim) {
		return (dim + kFpTileGroupPx - 1) / kFpTileGroupPx;
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* csPath = kForwardPlusCsPath,
		const char* depthVsPath = kForwardPlusDepthVsPath) {
		if (ShadersValid())
			return true;
		try {
			if (cullModule.empty())
				cullModule = ctx.CreateShaderModule(
					readFile(csPath ? csPath : kForwardPlusCsPath), "forwardplusCullCS");
			if (depthVsModule.empty())
				depthVsModule = ctx.CreateShaderModule(
					readFile(depthVsPath ? depthVsPath : kForwardPlusDepthVsPath), "forwardplusDepthVS");
		} catch (...) {
			return false;
		}
		if (cullModule.empty() || depthVsModule.empty())
			return false;
		Shader cs{ .module = cullModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		cullProgram = { .shaders = { cs }, .shaderCount = 1 };
		Shader vs{ .module = depthVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		depthProgram = { .shaders = { vs }, .shaderCount = 1 };
		return true;
	}

	void DestroyShaders() {
		cullProgram = ShaderProgram();
		depthProgram = ShaderProgram();
		cullModule = {};
		depthVsModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent,
		const ForwardPlusDesc& d, ImageViewHandle depthView,
		const char* csPath = kForwardPlusCsPath,
		const char* depthVsPath = kForwardPlusDepthVsPath) {
		if (swapExtent.width == 0 || swapExtent.height == 0)
			return false;
		if (d.tileSize == 0 || d.maxLights == 0 || d.maxLightsPerTile == 0)
			return false;
		// Fog row stride 0 would allocate a zero-size buffer and an
		// invalid BDA for the cull's fog stores — reject like the others.
		if (d.maxFogPerTile == 0)
			return false;
		// Shader-capacity contract: the cull's sorted insertion tracks at
		// most kForwardPlusMaxTileSortSlots entries per tile and the decal
		// write gate is kForwardPlusMaxDecalsPerTile. A desc beyond either
		// would clamp in the shader while the C++ allocation kept the desc
		// rows — producer and consumer would address different rows of the
		// same buffer. Reject here; the shader-side clamps (FpTileLightSlots
		// + the decal min) stay as defense in depth.
		if (d.maxLightsPerTile > kForwardPlusMaxTileSortSlots)
			return false;
		if (d.maxFogPerTile > kForwardPlusMaxTileSortSlots)
			return false;
		if (d.maxDecalsPerTile == 0 || d.maxDecalsPerTile > kForwardPlusMaxDecalsPerTile)
			return false;
		Destroy(rg);

		const uint32_t tcx = TileCountFor(swapExtent.width, d.tileSize);
		const uint32_t tcy = TileCountFor(swapExtent.height, d.tileSize);
		const uint32_t tiles = tcx * tcy;
		if (tiles == 0 || tiles > 32768)
			return false;

		Holder<BufferHandle> counts = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles, BufferType_Storage, "forwardplus tileCounts");
		if (counts.empty())
			return false;
		Holder<BufferHandle> indices = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles
				* (d.maxLightsPerTile + kForwardPlusProbeTileSlots),
			BufferType_Storage,
			"forwardplus tileIndices");
		if (indices.empty())
			return false;
		Holder<BufferHandle> decalCounts = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles, BufferType_Storage, "forwardplus decalTileCounts");
		Holder<BufferHandle> decalIndices = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles * d.maxDecalsPerTile, BufferType_Storage,
			"forwardplus decalTileIndices");
		if (decalCounts.empty() || decalIndices.empty())
			return false;
		// Per-tile FOG lists: one count + one index entry per tile/slot
		// (small buffers - the fog slots are few by design).
		Holder<BufferHandle> fogCounts = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles, BufferType_Storage, "forwardplus fogTileCounts");
		Holder<BufferHandle> fogIndices = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles * d.maxFogPerTile, BufferType_Storage,
			"forwardplus fogTileIndices");
		if (fogCounts.empty() || fogIndices.empty())
			return false;
		// Convex-volume tile masks (one bitmask uint per tile; the volume
		// leg of the cull fills them unconditionally every dispatch).
		Holder<BufferHandle> volumeMasks = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles, BufferType_Storage,
			"forwardplus volumeTileMasks");
		if (volumeMasks.empty())
			return false;

		LogicalBufferHandle countsLogical = rg.AddExternalBuffer(counts, "forwardplus tileCounts");
		if (countsLogical.empty())
			return false;
		LogicalBufferHandle indicesLogical = rg.AddExternalBuffer(indices, "forwardplus tileIndices");
		if (indicesLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			return false;
		}
		LogicalBufferHandle decalCountsLogical = rg.AddExternalBuffer(decalCounts, "forwardplus decalTileCounts");
		LogicalBufferHandle decalIndicesLogical = rg.AddExternalBuffer(decalIndices, "forwardplus decalTileIndices");
		if (decalCountsLogical.empty() || decalIndicesLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			rg.FreeBuffer(indicesLogical);
			if (!decalCountsLogical.empty())
				rg.FreeBuffer(decalCountsLogical);
			if (!decalIndicesLogical.empty())
				rg.FreeBuffer(decalIndicesLogical);
			return false;
		}
		LogicalBufferHandle fogCountsLogical = rg.AddExternalBuffer(fogCounts, "forwardplus fogTileCounts");
		LogicalBufferHandle fogIndicesLogical = rg.AddExternalBuffer(fogIndices, "forwardplus fogTileIndices");
		if (fogCountsLogical.empty() || fogIndicesLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			rg.FreeBuffer(indicesLogical);
			rg.FreeBuffer(decalCountsLogical);
			rg.FreeBuffer(decalIndicesLogical);
			if (!fogCountsLogical.empty())
				rg.FreeBuffer(fogCountsLogical);
			if (!fogIndicesLogical.empty())
				rg.FreeBuffer(fogIndicesLogical);
			return false;
		}
		LogicalBufferHandle volumeMasksLogical = rg.AddExternalBuffer(volumeMasks, "forwardplus volumeTileMasks");
		if (volumeMasksLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			rg.FreeBuffer(indicesLogical);
			rg.FreeBuffer(decalCountsLogical);
			rg.FreeBuffer(decalIndicesLogical);
			rg.FreeBuffer(fogCountsLogical);
			rg.FreeBuffer(fogIndicesLogical);
			return false;
		}

		// ---- Hierarchical phase-1 rings (zbins + coarse tile masks, one
		// pair per flight). Allocated LAST and assigned straight to the
		// members: Create opened with Destroy(rg) so the members are
		// empty, which makes the failure path below a plain Destroy(rg)
		// — it frees exactly the partial ring set and re-zeroes, no
		// hand-rolled cascade for 2 * MaxFramesInFlight more handles. ----
		const uint32_t mgx = TileGroupCountFor(swapExtent.width);
		const uint32_t mgy = TileGroupCountFor(swapExtent.height);
		const uint32_t groups = mgx * mgy;
		const size_t maskBytes =
			sizeof(uint32_t) * size_t(groups) * kFpTileMaskWords;
		bool ringsOk = groups != 0;
		for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight && ringsOk; ++f) {
			zBins[f] = ctx.CreateBuffer(kFpZBinBytes, BufferType_Storage,
				"forwardplus zbins");
			tileMasks[f] = ctx.CreateBuffer(maskBytes, BufferType_Storage,
				"forwardplus tileLightMasks");
			zBinsLogical[f] = rg.AddExternalBuffer(zBins[f], "forwardplus zbins");
			tileMasksLogical[f] =
				rg.AddExternalBuffer(tileMasks[f], "forwardplus tileLightMasks");
			ringsOk = !zBins[f].empty() && !tileMasks[f].empty()
				&& !zBinsLogical[f].empty() && !tileMasksLogical[f].empty();
		}
		if (!ringsOk) {
			Destroy(rg);
			return false;
		}
		maskGroupCountX = mgx;
		maskGroupCountY = mgy;

		desc = d;
		extent = swapExtent;
		tileCountX = tcx;
		tileCountY = tcy;
		tileCount = tiles;
		tileCountsBuffer = std::move(counts);
		tileIndicesBuffer = std::move(indices);
		tileCountsLogical = countsLogical;
		tileIndicesLogical = indicesLogical;
		decalTileCountsBuffer = std::move(decalCounts);
		decalTileIndicesBuffer = std::move(decalIndices);
		decalTileCountsLogical = decalCountsLogical;
		decalTileIndicesLogical = decalIndicesLogical;
		fogTileCountsBuffer = std::move(fogCounts);
		fogTileIndicesBuffer = std::move(fogIndices);
		fogTileCountsLogical = fogCountsLogical;
		fogTileIndicesLogical = fogIndicesLogical;
		volumeTileMasksBuffer = std::move(volumeMasks);
		volumeTileMasksLogical = volumeMasksLogical;

		if (!depthView.empty()) {
			depthViewHandle = depthView;
			// Must match the post-barrier layout: the cull hint requests
			// ShaderReadOnlyOptimal and the graph promotes depth formats to
			// DepthStencilReadOnlyOptimal (see CollectPassUsages fixup).
			depthBindless = ctx.CreateBindlessImage(depthView, ImageLayout::DepthStencilReadOnlyOptimal);
			depthBindlessIndex = depthBindless.empty()
				? UINT32_MAX : ctx.GetBindlessIndex(depthBindless);
		}
		return EnsureShaders(ctx, csPath, depthVsPath);
	}

	void Destroy(RenderGraph& rg) {
		if (!tileCountsLogical.empty())
			rg.FreeBuffer(tileCountsLogical);
		if (!tileIndicesLogical.empty())
			rg.FreeBuffer(tileIndicesLogical);
		if (!decalTileCountsLogical.empty())
			rg.FreeBuffer(decalTileCountsLogical);
		if (!decalTileIndicesLogical.empty())
			rg.FreeBuffer(decalTileIndicesLogical);
		if (!fogTileCountsLogical.empty())
			rg.FreeBuffer(fogTileCountsLogical);
		if (!fogTileIndicesLogical.empty())
			rg.FreeBuffer(fogTileIndicesLogical);
		if (!volumeTileMasksLogical.empty())
			rg.FreeBuffer(volumeTileMasksLogical);
		for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
			if (!zBinsLogical[f].empty())
				rg.FreeBuffer(zBinsLogical[f]);
			if (!tileMasksLogical[f].empty())
				rg.FreeBuffer(tileMasksLogical[f]);
			zBinsLogical[f] = {};
			tileMasksLogical[f] = {};
			zBins[f] = {};
			tileMasks[f] = {};
		}
		maskGroupCountX = maskGroupCountY = 0;
		tileCountsLogical = {};
		tileIndicesLogical = {};
		decalTileCountsLogical = {};
		decalTileIndicesLogical = {};
		fogTileCountsLogical = {};
		fogTileIndicesLogical = {};
		volumeTileMasksLogical = {};
		depthBindlessIndex = UINT32_MAX;
		depthBindless = {};
		depthViewHandle = {};
		tileCountsBuffer = {};
		tileIndicesBuffer = {};
		decalTileCountsBuffer = {};
		decalTileIndicesBuffer = {};
		fogTileCountsBuffer = {};
		fogTileIndicesBuffer = {};
		volumeTileMasksBuffer = {};
		extent = { 0, 0 };
		tileCountX = tileCountY = tileCount = 0;
	}

	// Resize-safe: recreates tile buffers only when extent/tile/limits drift.
	// Refreshes the depth bindless when the depth view handle changes.
	// Also ensures compute + depth programs. Returns true when valid after.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent,
		const ForwardPlusDesc& d, ImageViewHandle depthView,
		const char* csPath = kForwardPlusCsPath,
		const char* depthVsPath = kForwardPlusDepthVsPath) {
		const uint32_t wantX = TileCountFor(swapExtent.width, d.tileSize);
		const uint32_t wantY = TileCountFor(swapExtent.height, d.tileSize);
		const bool shapeOk = Valid() && MasksValid() &&
			extent.width == swapExtent.width && extent.height == swapExtent.height &&
			desc.tileSize == d.tileSize && desc.maxLights == d.maxLights &&
			desc.maxLightsPerTile == d.maxLightsPerTile &&
			desc.maxFogPerTile == d.maxFogPerTile &&
			desc.maxDecals == d.maxDecals && desc.maxDecalsPerTile == d.maxDecalsPerTile &&
			tileCountX == wantX && tileCountY == wantY &&
			maskGroupCountX == TileGroupCountFor(swapExtent.width) &&
			maskGroupCountY == TileGroupCountFor(swapExtent.height);
		if (shapeOk) {
			desc = d;
			if (depthView != depthViewHandle && !depthView.empty()) {
				depthViewHandle = depthView;
				depthBindless = ctx.CreateBindlessImage(depthView, ImageLayout::DepthStencilReadOnlyOptimal);
				depthBindlessIndex = depthBindless.empty()
					? UINT32_MAX : ctx.GetBindlessIndex(depthBindless);
			}
			return EnsureShaders(ctx, csPath, depthVsPath);
		}
		return Create(ctx, rg, swapExtent, d, depthView, csPath, depthVsPath);
	}

	uint64_t TileCountsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(tileCountsBuffer);
	}
	uint64_t TileIndicesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(tileIndicesBuffer);
	}
	uint64_t DecalTileCountsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(decalTileCountsBuffer);
	}
	uint64_t DecalTileIndicesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(decalTileIndicesBuffer);
	}
	uint64_t FogTileCountsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(fogTileCountsBuffer);
	}
	uint64_t FogTileIndicesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(fogTileIndicesBuffer);
	}
	// ---- Phase-1 ring addresses (per flight). Zero when the rings are
	// absent — the app publishes exactly these into the cull push and the
	// shader treats 0 as "no masks this frame, linear scan". ----
	uint64_t ZBinsAddress(ResourceManager& ctx, uint32_t f) const {
		return zBins[f].empty() ? 0 : ctx.GetBufferDeviceAddress(zBins[f]);
	}
	uint64_t TileMasksAddress(ResourceManager& ctx, uint32_t f) const {
		return tileMasks[f].empty()
			? 0 : ctx.GetBufferDeviceAddress(tileMasks[f]);
	}
	uint64_t VolumeTileMasksAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(volumeTileMasksBuffer);
	}

	// ---- Pass shells (caller sets the recording func) ----
	// Depth prepass: depth (Clear|Load)->Store over the full extent, plus
	// the optional SSSR-style thin G-buffer color attachment (oct normal
	// + roughness + metal, RGBA8 — see gbuffer.frag.hlsl) and the F0-
	// chroma layer 2 (RGB = the metal F0 tint, A = materialId/255 — the
	// same writer's SV_Target1). Load when the GfxRenderer seeded depth
	// first (T3 temporal seed); Clear otherwise. gbufferLoad: on
	// temporal-split frames (fallbackOnly prepass) Load keeps last
	// frame's values for the seed-drawn majority (static normals never
	// change); Clear on full-prepass frames (fresh coverage). Empty
	// gbufferLogical = no color attachment (legacy VS-only prepass —
	// the G-buffer off). gbuffer2Logical rides the SAME gate (empty =
	// not attached): the layer 2 is only meaningful where the G-buffer
	// leg writes; its clear (1,1,1,0) = white chroma + material 0 (the
	// flat-F0 fallback the trace takes when the layer is absent).
	RenderPass& AddDepthPrepass(RenderGraph& rg,
		LogicalImageViewHandle depthLogical, Extent2D swapExtent,
		const char* name = "ForwardPlusDepth",
		AttachmentLoadOp depthLoad = AttachmentLoadOp::Clear,
		LogicalImageViewHandle gbufferLogical = LogicalImageViewHandle{},
		AttachmentLoadOp gbufferLoad = AttachmentLoadOp::Clear,
		LogicalImageViewHandle gbuffer2Logical = LogicalImageViewHandle{},
		AttachmentLoadOp gbuffer2Load = AttachmentLoadOp::Clear) const {
		assert(Valid());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(swapExtent)
			.SetDepth(depthLogical,
				depthLoad,
				AttachmentStoreOp::Store,
				ClearDepthStencil(0.0f, 0));
		if (!gbufferLogical.empty()) {
			// Clear (0.5, 0.5, 1, 0): oct +Z normal, matte, non-metal —
			// "no data" reads as a safe non-reflector.
			pass.SetColor({ gbufferLogical },
				gbufferLoad,
				AttachmentStoreOp::Store,
				ClearColor(0.5f, 0.5f, 1.0f, 0.0f));
		}
		if (!gbuffer2Logical.empty() && !gbufferLogical.empty()) {
			// The layer 2 rides the G-buffer leg (it is only meaningful
			// where the writer FS pair binds): both attachments attach
			// together — SetColor REPLACES the list, so this re-binds
			// the layer 1 WITH the layer 2 in one MRT pair (never call
			// SetColor twice — the second call would drop the first).
			// The layer 2's clear (1,1,1,0): white chroma + material 0
			// — the "no data" decode is the flat-F0 behavior.
			pass.SetColorAttachments({
				{ gbufferLogical,
					gbufferLoad,
					AttachmentStoreOp::Store,
					ClearColor(0.5f, 0.5f, 1.0f, 0.0f) },
				{ gbuffer2Logical,
					gbuffer2Load,
					AttachmentStoreOp::Store,
					ClearColor(1.0f, 1.0f, 1.0f, 0.0f) },
			});
		}
		return pass;
	}

	// Cull: compute, reads depth + light buffer, writes tile buffers.
	// Caller binds cullProgram, pushes ForwardPlusCullPush, dispatches
	// (tileCountX, tileCountY, 1). decalsLogical (optional): the per-frame
	// decal upload buffer read by the decal leg of the cull (pass an empty
	// handle when the frame has no decal data buffer). flight: the
	// frame-in-flight index selecting this frame's phase-1 zBin/mask ring
	// slice (read hints only — the upload happened outside the graph with
	// its own transfer->compute barrier declared at upload time, the DS
	// phase-2 pattern).
	RenderPass& AddCullPass(RenderGraph& rg,
		LogicalImageViewHandle depthLogical,
		LogicalBufferHandle lightsLogical,
		const char* name = "ForwardPlusCull",
		LogicalBufferHandle decalsLogical = LogicalBufferHandle{},
		uint32_t flight = 0) const {
		assert(Valid());
		RenderPass& pass = rg.AddComputePass(name);
		// Depth sampled in compute.
		ImageSyncRequest depthReq{};
		depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		depthReq.layout = ImageLayout::ShaderReadOnlyOptimal;
		pass.AddImageHint(depthLogical, depthReq);
		// Lights read in compute.
		BufferSyncRequest lightsReq{};
		lightsReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		lightsReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(lightsLogical, lightsReq);
		// Decal data (uploaded this frame) read in compute.
		if (!decalsLogical.empty()) {
			BufferSyncRequest decalsReq{};
			decalsReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			decalsReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(decalsLogical, decalsReq);
		}
		// Phase-1 zBins + coarse tile masks (CPU upload -> compute read;
		// the rings exist whenever the builder is Valid — a frame that
		// skipped the upload still gets harmless ordering-only hints).
		if (flight < smvk::limits::MaxFramesInFlight) {
			BufferSyncRequest phase1Read{};
			phase1Read.barrierMasks.accessMask = AccessFlags::ShaderRead;
			phase1Read.barrierMasks.stageMask = PipelineStageBit_Compute;
			if (!zBinsLogical[flight].empty())
				pass.AddBufferHint(zBinsLogical[flight], phase1Read);
			if (!tileMasksLogical[flight].empty())
				pass.AddBufferHint(tileMasksLogical[flight], phase1Read);
		}
		// Tile buffers write in compute, read in fragment.
		BufferSyncRequest tileWrite{};
		tileWrite.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		tileWrite.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Fragment;
		pass.AddBufferHint(tileCountsLogical, tileWrite);
		pass.AddBufferHint(tileIndicesLogical, tileWrite);
		pass.AddBufferHint(decalTileCountsLogical, tileWrite);
		pass.AddBufferHint(decalTileIndicesLogical, tileWrite);
		pass.AddBufferHint(fogTileCountsLogical, tileWrite);
		pass.AddBufferHint(fogTileIndicesLogical, tileWrite);
		pass.AddBufferHint(volumeTileMasksLogical, tileWrite);
		return pass;
	}

	// Read-side barrier helper for the opaque forward pass: tile buffers
	// sampled in fragment. Call before/while building the main pass.
	static void AddTileReadHints(RenderPass& mainPass,
		LogicalBufferHandle countsLogical, LogicalBufferHandle indicesLogical) {
		BufferSyncRequest tileRead{};
		tileRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		tileRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
		mainPass.AddBufferHint(countsLogical, tileRead);
		mainPass.AddBufferHint(indicesLogical, tileRead);
	}

	// Fog-list sibling of AddTileReadHints: the volumetric pass reads the
	// dedicated fog tile lists (march-range candidates — see
	// ForwardPlusCullPush::fogTile*). Same fragment-stage read request.
	static void AddFogTileReadHints(RenderPass& pass,
		LogicalBufferHandle fogCountsLogical, LogicalBufferHandle fogIndicesLogical) {
		BufferSyncRequest fogRead{};
		fogRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		fogRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
		pass.AddBufferHint(fogCountsLogical, fogRead);
		pass.AddBufferHint(fogIndicesLogical, fogRead);
	}

	// Decal-list sibling of AddTileReadHints (Main samples the decal tile
	// counts + indices even when the light lists come from the
	// deferred-shadow pipeline).
	static void AddDecalTileReadHints(RenderPass& mainPass,
		LogicalBufferHandle countsLogical, LogicalBufferHandle indicesLogical) {
		AddTileReadHints(mainPass, countsLogical, indicesLogical);
	}

	// Convex-volume tile-mask sibling (single buffer): the lit Main pass
	// AND the volumetric fog pass sample the per-tile volume bitmask the
	// tile cull wrote this frame — this hint orders the compute ->
	// fragment edge for whichever pass reads it.
	static void AddVolumeMaskReadHints(RenderPass& mainPass,
		LogicalBufferHandle masksLogical) {
		BufferSyncRequest maskRead{};
		maskRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		maskRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
		mainPass.AddBufferHint(masksLogical, maskRead);
	}
};

} // namespace smvk
