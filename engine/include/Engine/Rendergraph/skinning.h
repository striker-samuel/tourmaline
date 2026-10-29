// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU SKINNING pass module (the Northlight compute box; see
// gfxskinning.cs.hlsl for the full pipeline contract). One pass per
// frame, appended EARLY (the Northlight order: bone eval -> skinning ->
// [the bounds/cone update - slice 3] -> the gfxfilter culling -> the
// draw passes): everything downstream (the mesh-shader's deformed fetch,
// the culling) reads the deformed buffer written here.
//
// The palette: ONE skinned cluster per frame for now (the knight demo
// has exactly one; a per-draw palette table is the multi-cluster
// generalization - deferred with the slice-3 culling work).

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/smvk.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace smvk::skinning {

inline constexpr const char* kSkinningCsPath =
	"content/shaders/gfxskinning.cs.hlsl.spv";

// The joint-table cap (mesh.h: the RGBA8 palette indexing).
inline constexpr uint32_t kSkinPaletteMax = 256;

// Mirrors gfxskinning.cs.hlsl's SkinningPush exactly (layout-pinned:
// the shader's comment carries the offsets).
struct SkinPush {
	uint64_t palette = 0;   // 0  float4x4[256] (per frame)
	uint64_t skin = 0;      // 8  PackedSkinInfluences pairs (static)
	uint64_t ranges = 0;    // 16 (heapBase, count, skinBase, pad) rows
	uint64_t deformed = 0;  // 24 [pos3, normalW, tangentW] output
	uint64_t posHeap = 0;   // 32 bind-pose positions (3 words/v)
	uint64_t shadeHeap = 0; // 40 bind-pose shading (6 words/v)
	uint32_t deformedBase = 0; // 48 the skinned span's heap-id base
	uint32_t paletteCount = 0; // 52
	uint32_t rangeCount = 0;   // 56
	uint32_t pad0 = 0;         // 60
};
static_assert(sizeof(SkinPush) == 64);

struct SkinVisual {
	Holder<ShaderModuleHandle> csModule;
	ShaderProgram program{};
	// Per-flight palette buffers (kSkinPaletteMax mat4s = 16KB each):
	// the frame's single palette upload lands in the flight's buffer so
	// the next frame's overwrite can never race this frame's reads (the
	// meshParamsBuffers pattern).
	Holder<BufferHandle> paletteBuffers[smvk::limits::MaxFramesInFlight] = {};
	bool valid = false;

	// Compiles the compute shader + creates the per-flight palette
	// buffers. Non-fatal on shader failure (AppendPass checks valid).
	bool Ensure(ResourceManager& ctx);
	void Destroy();

	// Appends the skinning compute pass: uploads the palette (the CPU
	// bone-eval box's output - the joint matrices the cluster's
	// SampleAnimation built this frame) into this flight's buffer +
	// dispatches one workgroup per range (the skinned-heap BDAs + the
	// ranges table from the cluster's EnsureSkinnedGpuData). Returns
	// false when nothing could run (no shader/no data/empty palette).
	bool AppendPass(ResourceManager& ctx, RenderGraph& rg,
		uint32_t frameIndex, const SkinPush& push,
		const std::vector<glm::mat4>& palette);
};

} // namespace smvk::skinning
