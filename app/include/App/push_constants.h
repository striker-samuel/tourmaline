#pragma once
// Scene push constants shared by main.cpp and frame renderers.
// Must match HLSL PushConstants in engine/shaders/common.hlsli
// (field order + size). Was previously a main.cpp-local struct.

#include <cstddef>
#include <cstdint>

namespace app {

// Must fit the engine push budget (checked below).
struct alignas(16) PushConstants {
	float model[16];
	uint64_t frameUniforms;
	uint64_t frameHot;
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
	float prevModel[16]; // motion vector pass: last frame's model (192..255)
};
static_assert(sizeof(PushConstants) <= 256, "PushConstants must fit in 256 bytes (engine limit)");

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
static_assert(offsetof(PushConstants, prevModel) == 200, "prevModel must start at byte 200");
static_assert(offsetof(PushConstants, model) == 0, "model must start at byte 0");
static_assert(offsetof(PushConstants, frameUniforms) == 64, "frameUniforms mirror drift");
static_assert(offsetof(PushConstants, frameHot) == 72, "frameHot mirror drift");
static_assert(offsetof(PushConstants, renderViews) == 184, "renderViews mirror drift");
static_assert(offsetof(PushConstants, mainRenderView) == 192, "mainRenderView mirror drift");
static_assert(offsetof(PushConstants, renderViewIndex) == 196, "renderViewIndex mirror drift");

} // namespace app
