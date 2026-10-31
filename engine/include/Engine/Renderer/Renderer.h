// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Renderer interface: owns the scene push-constant layout.
//
// Migration complete (10/2): was `engine::PushConstants`
// (Engine/Rendergraph/PushConstants.h, deleted) — an engine-wide
// definition every frame renderer included. The layout is
// really a renderer contract: it must match the HLSL PushConstants in
// engine/shaders/common.hlsli field-for-field, and only a renderer that
// binds those shaders can own it. Helpers that record draws against it
// (GfxRenderer seed/shadow lists, ForwardPlus prepass, terrain/probeball
// passes, the editor live view) consume `IRenderer::PushConstants` — they
// never define their own copy.
//
// LAYOUT INVARIANT (broke 10/1, fixed 10/2): real shared fields MUST stay
// within bytes 0..192 — every HLSL "mirror" struct that re-declares the
// scene pc (DefaultVsPush in default.vert.hlsl, UvFlatVsPush, the seed /
// shadow / placement / filtered-motion tails) hard-codes the sub-192
// offsets, and bytes 192..256 are the per-draw overlay scratch (filtered
// VS tail 192..224, direct chart tail 192..208, per-draw prevModel push
// 192..240). Keep this invariant: FS-only extras go in FramePSHot
// (carried via FrameUniforms::frameHot), never in here below byte 192.

#include <cstddef>
#include <cstdint>

namespace engine {

class IRenderer {
public:
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
		uint32_t pad_pc;
		// RenderView system (see smvk::RenderViewSet): the per-frame
		// virtual-camera slab + the main camera index. renderViewIndex is
		// the view the current pass renders from (UINT32_MAX = none).
		uint64_t renderViews;
		uint32_t mainRenderView;
		uint32_t renderViewIndex;
		// Motion vector pass: last frame's model as float3x4 (last row
		// 0,0,0,1 implied). Starts at byte 192, ends at byte 240
		// (12 floats = 48 bytes; the SPV packs it RowMajor stride 12,
		// consumed via v4 x M, so the blob holds the matrix COLUMNS —
		// glm columns xyz + translation, i.e. source floats
		// {0,1,2, 4,5,6, 8,9,10, 12,13,14}, see the pack loop in
		// world_renderer.cpp drawMotionNode).
		float prevModel[12];
		// padEnd_[0]: the DIRECT legs' per-draw dither-on-closeup flag
		// (Scene::ditherCloseup — pushed per draw, 0/1, by the direct
		// prepass / Main / shadow legs; default.vert / shadow.vert
		// forward it flat at VS location 8 so the FS family reads ONE
		// channel on both paths — the filtered legs deliver the gfx Draw
		// ring's DrawFlagBit_DitherCloseup there).
		// padEnd_[1]: the DIRECT legs' per-draw RESOLVED material
		// userdata (docs/shader_library_design.md Phase A — pushed per
		// LIT draw by the direct Main legs + the editor's direct legs;
		// default.vert forwards it flat at VS location 9, the unified
		// second per-draw channel; the filtered legs deliver the Draw
		// ring's userData there. The filtered motion pass overlays the
		// whole 240..256 word with its own tail before its draws, so
		// the two scratch users never conflict). [2..3] stay scratch.
		uint32_t padEnd_[4];
	};

	virtual ~IRenderer() = default;
	virtual const char* RendererName() const = 0;

	static constexpr size_t kPushSize = sizeof(PushConstants);
};

// Must fit the engine push budget (checked below).
static_assert(sizeof(IRenderer::PushConstants) == 256,
	"IRenderer::PushConstants must be exactly 256 bytes (engine limit; mirrors common.hlsli)");

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
static_assert(offsetof(IRenderer::PushConstants, prevModel) == 192,
	"prevModel must start at byte 192");
static_assert(sizeof(IRenderer::PushConstants::prevModel) == 48,
	"prevModel must be 12 floats (float3x4)");
static_assert(offsetof(IRenderer::PushConstants, model) == 0,
	"model must start at byte 0");
static_assert(offsetof(IRenderer::PushConstants, frameUniforms) == 64,
	"frameUniforms mirror drift");
static_assert(offsetof(IRenderer::PushConstants, lightsBuffer) == 72,
	"lightsBuffer mirror drift (sub-192 invariant)");
static_assert(offsetof(IRenderer::PushConstants, materialsBuffer) == 80,
	"materialsBuffer mirror drift");
static_assert(offsetof(IRenderer::PushConstants, cascadeBuffer) == 88,
	"cascadeBuffer mirror drift");
static_assert(offsetof(IRenderer::PushConstants, materialId) == 96,
	"materialId mirror drift");
static_assert(offsetof(IRenderer::PushConstants, comparisonSamplerId) == 100,
	"comparisonSamplerId mirror drift");
static_assert(offsetof(IRenderer::PushConstants, numCascades) == 104,
	"numCascades mirror drift");
static_assert(offsetof(IRenderer::PushConstants, cascadeIndex) == 108,
	"cascadeIndex mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpLights) == 112,
	"fpLights mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpTileCounts) == 120,
	"fpTileCounts mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpTileIndices) == 128,
	"fpTileIndices mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpTileCountX) == 136,
	"fpTileCountX mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpLightCount) == 140,
	"fpLightCount mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpTileSize) == 144,
	"fpTileSize mirror drift");
static_assert(offsetof(IRenderer::PushConstants, fpMaxPerTile) == 148,
	"fpMaxPerTile mirror drift");
static_assert(offsetof(IRenderer::PushConstants, spotShadowBuffer) == 152,
	"spotShadowBuffer mirror drift");
static_assert(offsetof(IRenderer::PushConstants, spotShadowMapIndex) == 160,
	"spotShadowMapIndex mirror drift");
static_assert(offsetof(IRenderer::PushConstants, spotShadowCount) == 164,
	"spotShadowCount mirror drift");
static_assert(offsetof(IRenderer::PushConstants, spotIndex) == 168,
	"spotIndex mirror drift");
static_assert(offsetof(IRenderer::PushConstants, pad_pc) == 172,
	"pad_pc mirror drift (transparency technique word)");
static_assert(offsetof(IRenderer::PushConstants, renderViews) == 176,
	"renderViews mirror drift");
static_assert(offsetof(IRenderer::PushConstants, mainRenderView) == 184,
	"mainRenderView mirror drift");
static_assert(offsetof(IRenderer::PushConstants, renderViewIndex) == 188,
	"renderViewIndex mirror drift");
static_assert(offsetof(IRenderer::PushConstants, padEnd_) == 240,
	"padEnd_ mirror drift");

} // namespace engine
