#pragma once
// Scene push constants shared by main.cpp and frame renderers.
// Must match HLSL PushConstants in engine/shaders/common.hlsli
// (field order + size). Was previously a main.cpp-local struct.

#include <cstddef>
#include <cstdint>

namespace engine {

// Must fit the engine push budget (checked below).
// LAYOUT INVARIANT (broke 10/1, fixed 10/2): real shared fields MUST stay
// within bytes 0..192 — every HLSL "mirror" struct that re-declares the
// scene pc (DefaultVsPush in default.vert.hlsl, UvFlatVsPush, the seed /
// shadow / placement / filtered-motion tails) hard-codes the sub-192
// offsets, and bytes 192..256 are the per-draw overlay scratch (filtered
// VS tail 192..224, direct chart tail 192..208, per-draw prevModel push
// 192..240). The 10/1 rework inserted `frameHot` at 72 which shifted every
// later field past what the mirrors declare — default.vert read spotIndex
// bytes as the renderViews BDA and dereferenced it (async device loss, no
// fault records). Keep this invariant: FS-only extras go in FramePSHot
// (carried via FrameUniforms::frameHot), never in here below byte 192.
struct alignas(16) PushConstants {
	float model[16];
	uint64_t frameUniforms;
	uint64_t lightsBuffer;
	uint64_t materialsBuffer;
	uint64_t cascadeBuffer;
	uint32_t materialId;
	uint32_t comparisonSamplerId;
	uint32_t numCascades;
	uint32_t cascadeIndex;
	uint64_t fpLights;
	uint64_t fpTileCounts;
	uint64_t fpTileIndices;
	uint32_t fpTileCountX;
	uint32_t fpLightCount;
	uint32_t fpTileSize;
	uint32_t fpMaxPerTile;
	uint64_t spotShadowBuffer;
	uint32_t spotShadowMapIndex;
	uint32_t spotShadowCount;
	uint32_t spotIndex;
	// Transparency technique for AlphaMode::Blend materials (must match
	// HLSL PushConstants::pad_pc in engine/shaders/common.hlsli):
	// 0 = dithered screen-door (default, opaque state), 1 = alpha blend.
	uint32_t pad_pc;
	// RenderView system (see smvk::RenderViewSet): the per-frame
	// virtual-camera slab + the main camera index. renderViewIndex is
	// the view the current pass renders from (UINT32_MAX = none).
	uint64_t renderViews;
	uint32_t mainRenderView;
	uint32_t renderViewIndex;
	// Motion vector pass: last frame's model as float3x4 (last row 0,0,0,1 implied).
	// Starts at byte 192, ends at byte 240 (12 floats = 48 bytes; the
	// SPV packs it RowMajor stride 12, consumed via v4 x M, so the blob
	// holds the matrix COLUMNS — glm columns xyz + translation, i.e.
	// source floats {0,1,2, 4,5,6, 8,9,10, 12,13,14}, see the pack loop
	// in world_renderer.cpp drawMotionNode).
	float prevModel[12];
	uint32_t padEnd_[4];
};
static_assert(sizeof(PushConstants) == 256, "PushConstants must be exactly 256 bytes (engine limit; mirrors common.hlsli)");

// Filtered-VS tail push lives in the prevModel scratch area (bytes
// 192..224: 24B prefix + chart-transform BDA, see
// smvk::gfx::kFilteredVsPushOffset); the fragment shader never reads
// prevModel in passes that use the filtered VS. The motion VS reuses the
// 24B prefix and pushes its FrameUniforms BDA at byte 216.
//
// The direct-path vertex shaders (default.vert DefaultVsPush,
// uvflat1 UvFlatVsPush) mirror bytes 0..192 field-for-field in HLSL with
// a chart tail at 192 (see smvk::gfx::kUvFlatVsPushOffset) — the offsets
// below pin the C++ side of that mirror.
static_assert(offsetof(PushConstants, prevModel) == 192, "prevModel must start at byte 192");
static_assert(sizeof(PushConstants::prevModel) == 48, "prevModel must be 12 floats (float3x4)");
static_assert(offsetof(PushConstants, model) == 0, "model must start at byte 0");
static_assert(offsetof(PushConstants, frameUniforms) == 64, "frameUniforms mirror drift");
static_assert(offsetof(PushConstants, lightsBuffer) == 72, "lightsBuffer mirror drift (sub-192 invariant)");
static_assert(offsetof(PushConstants, renderViews) == 176, "renderViews mirror drift");
static_assert(offsetof(PushConstants, mainRenderView) == 184, "mainRenderView mirror drift");
static_assert(offsetof(PushConstants, renderViewIndex) == 188, "renderViewIndex mirror drift");

} // namespace engine
