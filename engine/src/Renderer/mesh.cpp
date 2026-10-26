// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/smvk.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Memory/alloc_config.h> // mem::config::Runtime() (the heap budgets)
#include <Engine/Renderer/static_vertex_heap.h>

#include <Engine/Renderer/mesh.h>
#include <Engine/App/cluster.h>

#include <algorithm>

namespace smvk {

	MaterialPipelineState MakePipelineStateForMaterial(
		AlphaMode alphaMode,
		ShadingModel shadingModel)
	{
		MaterialPipelineState out{};

		// Raster: always back-face cull (two-sided geometry is no longer
		// supported — the cooked Material::doubleSided flag is ignored).
		// NOTE: main view uses reversed infinite-Z with Y-flipped proj (proj[1][1] *= -1).
		// Position conversion (-x,-z) preserves winding (+1), Y-flip inverts it (-1).
		// Net winding inversion would suggest Clockwise, but glTF import + Vulkan viewport
		// handling keeps NDC winding as CounterClockwise for front faces in this engine
		// (verified: Clockwise caused global N inversion). Keep CounterClockwise per user report.
		out.raster.cullMode = CullMode::Back;
		out.raster.polygonMode = PolygonMode::Fill;
		out.raster.frontFace = FrontFace::CounterClockwise;

		// Depth: opaque/masked write depth; blend does not write depth
		// (prevents transparent sorting artefacts, caller may override).
		// Main view uses reversed infinite-Z (GreaterEqual, clear 0) – see main.cpp.
		if (alphaMode == AlphaMode::Blend) {
			out.depth = DepthSettings::Disabled();
			// Keep depth test but no write for typical transparent.
			out.depth.depthTestEnable = true;
			out.depth.depthFunc = CompareOp::GreaterEqual;
			out.depth.writeEnable = false;
		} else {
			out.depth = DepthSettings::DepthTest();
			out.depth.depthFunc = CompareOp::GreaterEqual;
			out.depth.writeEnable = true;
			out.depth.depthTestEnable = true;
		}

		// Blend: opaque/masked = no blend; Blend = alpha blend.
		// Masked shading still opaque blend + alpha test in shader via alphaCutoff.
		if (alphaMode == AlphaMode::Blend) {
			out.blend = BlendSettings::AlphaBlend();
		} else {
			out.blend = BlendSettings::Opaque();
		}

		// Future: shadingModel selects shader permutation.
		// e.g. Unlit -> different PipelineKey::program; kept here so material
		// remains single source of truth.
		(void)shadingModel;

		return out;
	}

// GetVertexFormatDescription lives in vertex_format.cpp and this file's
// static-heap allocation is pure facade: BOTH compile into the Vulkan
// engine AND engine_d3d12 (the whole mesh/static-heap layer is
// backend-neutral; the CommandBuffer-side bind lives per-backend).

void AllocateStaticVertexFmt(smvk::ResourceManager& ctx)
{
	// Idempotent: App and Editor both need the heap; a second call
	// must not leak buffers/blocks (editor previously crashed in
	// vmaVirtualAllocate on a null block because it never called this).
	if (staticVertexFmtHeap.m_staticVertexBlock.Valid()
		&& staticVertexFmtHeap.m_staticIndexBlock.Valid()
		&& staticVertexFmtHeap.m_staticVertexPosBuffer.valid()
		&& staticVertexFmtHeap.m_staticVertexShadingBuffer.valid()
		&& staticVertexFmtHeap.m_staticIndexBuffer.valid()) {
		return;
	}
	// The budgets come from mem::config::Runtime() (the compile-time
	// defaults + the TOURMALINE_MEM_STATIC_VERTEX_MB /
	// TOURMALINE_MEM_STATIC_INDEX_MB program-entry overrides) — read AT
	// ALLOCATION TIME. The old code parsed the env overrides into the
	// runtime config but still allocated the hardcoded 512 MiB values, so
	// the overrides silently did nothing.
	const uint64_t vertexHeapBytes =
		mem::config::Runtime().staticVertexHeap;
	const uint64_t indexHeapBytes =
		mem::config::Runtime().staticIndexHeap;
	const uint64_t maxVertices = std::min(
		vertexHeapBytes / sizeof(smvk::VertexPos),
		vertexHeapBytes / sizeof(smvk::VertexShading)
	);
	staticVertexFmtHeap.m_staticVertexPosBuffer =
		ctx.CreateBuffer(vertexHeapBytes,
		                 smvk::BufferType_Vertex, "static vertex pos")
			.release();
	staticVertexFmtHeap.m_staticVertexShadingBuffer =
		ctx.CreateBuffer(vertexHeapBytes,
		                 smvk::BufferType_Vertex, "static vertex shading")
			.release();
	staticVertexFmtHeap.m_staticIndexBuffer =
		ctx.CreateBuffer(indexHeapBytes,
		                 smvk::BufferType_Storage, "static index")
			.release();

	// ---------------------------------------------------------------------
	// Vertex virtual allocator — VERTEX units (both streams allocate the
	// same vertex count at the same vertexBase).
	// ---------------------------------------------------------------------
	staticVertexFmtHeap.m_staticVertexBlock.Initialize(maxVertices);

	// ---------------------------------------------------------------------
	// Index virtual allocator — BYTES.
	// ---------------------------------------------------------------------
	staticVertexFmtHeap.m_staticIndexBlock.Initialize(
		indexHeapBytes);
}
};