// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Chrome-ball visualization of the loaded cubemap probes — see
// Rendergraph/probeball.h for the design. Sphere build + the standalone
// pass (own push block, no scene pc).

#include <Engine/Rendergraph/probeball.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace smvk::probeball {

namespace {

// Unit UV sphere (24 slices x 16 stacks) in the terrain vertex format:
// pos = n (unit), normal = n; tangent/color are bound but unused (the
// shaders declare only POSITION + NORMAL — attributes shaders don't
// declare are simply not fetched).
bool BuildSphere(ResourceManager& ctx, ProbeBallVisual& v) {
	constexpr int kSlices = 24;
	constexpr int kStacks = 16;
	constexpr float kPi = 3.14159265359f;

	const uint32_t rows = uint32_t(kStacks) + 1u;
	const uint32_t cols = uint32_t(kSlices) + 1u;
	const uint32_t verts = rows * cols;
	const uint32_t tris = uint32_t(kStacks) * uint32_t(kSlices) * 2u;

	// (Plain-size locals: `std::vector<T> v(size_t(n))` parses as a
	// function declaration — the most vexing parse; same as terrain.cpp.)
	const size_t vertCountSz = size_t(verts);
	const size_t idxCountSz = size_t(tris) * 3u;
	std::vector<TerrainVertex> vertices(vertCountSz);
	std::vector<uint32_t> indices(idxCountSz);
	{
		TerrainVertex* out = vertices.data();
		for (int j = 0; j <= kStacks; ++j) {
			const float phi = kPi * float(j) / float(kStacks);
			const float sinPhi = std::sin(phi);
			const float cosPhi = std::cos(phi);
			for (int i = 0; i <= kSlices; ++i) {
				const float theta = 2.0f * kPi * float(i) / float(kSlices);
				const glm::vec3 n(sinPhi * std::cos(theta), cosPhi,
					sinPhi * std::sin(theta));
				TerrainVertexSrc src{};
				src.pos = n;
				src.normal = n;
				src.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
				src.color = glm::vec4(1.0f);
				PackTerrainVertex(src, *out);
				++out;
			}
		}
	}
	{
		uint32_t k = 0;
		for (int j = 0; j < kStacks; ++j) {
			for (int i = 0; i < kSlices; ++i) {
				const uint32_t a = uint32_t(j) * cols + uint32_t(i);
				const uint32_t b = a + 1u;
				const uint32_t c = a + cols;
				const uint32_t d = c + 1u;
				// Out-facing winding (+Y-up CCW front — the terrain/
				// placement grid convention), SPHERE-ADAPTED: the stack
				// axis runs DOWN (phi 0 = top, pi = bottom), so the grid's
				// (a, +row, +col) order winds INWARD here (cross(stack-,
				// slice-tangent) = -radial). The mirrored order (a, +col,
				// +row) keeps the geometric normal OUTWARD — the terrain
				// grid's axis (+Z) points AWAY from its up axis, the
				// sphere's stack axis points TOWARD it (-Y), which flips
				// the cross. Inward winding rendered the balls inside-out
				// (back-face culled to the inner surface, reflect sampling
				// mirrored — "the wall is on the wrong side").
				indices[k++] = a;
				indices[k++] = b;
				indices[k++] = c;
				indices[k++] = b;
				indices[k++] = d;
				indices[k++] = c;
			}
		}
	}

	v.sphereVB = ctx.CreateBuffer(
		sizeof(TerrainVertex) * size_t(verts),
		BufferType_Vertex, "probe ball sphere");
	v.sphereIB = ctx.CreateBuffer(
		sizeof(uint32_t) * size_t(tris) * 3u,
		BufferType_Index, "probe ball indices");
	if (v.sphereVB.empty() || v.sphereIB.empty()) {
		v.sphereVB = {};
		v.sphereIB = {};
		v.geometryValid = false;
		return false;
	}
	ctx.UploadDataToBuffer(v.sphereVB, vertices.data(),
		sizeof(TerrainVertex) * size_t(verts),
		PipelineStageBit_AllGraphics, AccessFlags::VertexAttributeRead, 0, 0);
	ctx.UploadDataToBuffer(v.sphereIB, indices.data(),
		sizeof(uint32_t) * size_t(tris) * 3u,
		PipelineStageBit_AllGraphics, AccessFlags::IndexRead, 0, 0);
	v.vertexCount = verts;
	v.indexCount = tris * 3u;
	v.geometryValid = true;
	return true;
}

} // namespace

bool ProbeBallVisual::Ensure(ResourceManager& ctx) {
	bool ok = true;
	if (!geometryValid)
		ok = BuildSphere(ctx, *this);
	try {
		if (vsModule.empty())
			vsModule = ctx.CreateShaderModule(
				readFile(kProbeBallVsPath), "probeBallVert");
		if (psModule.empty())
			psModule = ctx.CreateShaderModule(
				readFile(kProbeBallPsPath), "probeBallFrag");
	}
	catch (...) {
		ok = false;
	}
	if (vsModule.empty() || psModule.empty()) {
		if (ok)
			std::fprintf(stderr, "[probeball] shaders missing (no .spv?) "
				"- the chrome balls skip until they compile\n");
		return geometryValid; // geometry stands; the pass gates on shaders
	}
	if (!ShadersValid()) {
		Shader vs{ .module = vsModule, .entryPoint = "mainVS",
			.stage = ShaderStage::Vertex };
		Shader ps{ .module = psModule, .entryPoint = "mainPS",
			.stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
	}
	return ok;
}

void ProbeBallVisual::Destroy() {
	sphereVB = {};
	sphereIB = {};
	vertexCount = indexCount = 0;
	geometryValid = false;
	program = ShaderProgram{};
	vsModule = {};
	psModule = {};
}

bool ProbeBallVisual::AppendPass(RenderGraph& rg,
	LogicalImageViewHandle hdrView, LogicalImageViewHandle depthView,
	Extent2D extent, const glm::mat4& viewProj, const glm::vec3& eye,
	const ProbeBallRow* rows, uint32_t rowCount, float intensity,
	uint32_t samplerIndex, uint64_t frameUniformsAddress) {
	if (!geometryValid || !ShadersValid() || !rows || rowCount == 0
		|| hdrView.empty() || depthView.empty())
		return false;
	if (sphereVB.empty() || sphereIB.empty())
		return false;

	// Keep only drawable rows up front (invalid cubemap indices drop here,
	// so the pass is skipped entirely when nothing can draw).
	std::vector<ProbeBallRow> drawRows;
	drawRows.reserve(rowCount);
	for (uint32_t i = 0; i < rowCount; ++i)
		if (rows[i].cubemapIndex >= 0)
			drawRows.push_back(rows[i]);
	if (drawRows.empty())
		return false;

	// The pass: HDR load/store + the scene depth TEST-ONLY (load/store —
	// the attachment stores, the PIPELINE never writes: occluded balls hide
	// behind geometry, and the depth the post-Main consumers march stays
	// untouched by the debug visual).
	RenderPass& pass = rg.AddPass("ProbeBalls")
		.SetOffset(0, 0)
		.SetExtent(extent)
		.SetColor(
			{ hdrView },
			AttachmentLoadOp::Load,
			AttachmentStoreOp::Store,
			ClearColor(0, 0, 0, 1))
		.SetDepth(
			depthView,
			AttachmentLoadOp::Load,
			AttachmentStoreOp::Store,
			ClearDepthStencil(0, 0));
	// The recording captures `this` (the module lives on the WorldRenderer,
	// which outlives every frame's Execute) — Holders are single-owner, so
	// the buffers are read through the module at record time, never copied.
	pass.SetRecordingFunc([this, viewProj, eye, intensity, samplerIndex,
			frameUniformsAddress,
			drawRows = std::move(drawRows)](CommandBuffer& cmd) mutable {
		// TEST-ONLY depth (GreaterEqual reversed-Z): the balls occlude
		// against the scene depth but never write it.
		const DepthSettings depth{
			.depthFunc = CompareOp::GreaterEqual,
			.writeEnable = false,
			.depthTestEnable = true,
		};
		cmd.BindRenderPipeline(
			program,
			VertexFormat::TerrainVertexFormat,
			depth,
			StencilSettings{},
			BlendSettings::Opaque(),
			MSAASettings::Count1(),
			RasterSettings{} /* back-face cull, CCW front (sphere winding) */);
		cmd.BindVertexBuffer(sphereVB, 0, 0);
		cmd.BindIndexBuffer(sphereIB, 0);
		for (const ProbeBallRow& r : drawRows) {
			// Ball radius reads the influence at a glance (proportional,
			// clamped — a 10 m capture must not fill the frame; the old
			// editor chrome-ball rule).
			const float radius = std::clamp(r.influenceRadius * 0.15f,
				0.1f, 0.4f);
			ProbeBallPush push{};
			push.viewProj = viewProj;
			push.eye = glm::vec4(eye, 0.0f);
			push.xform = glm::vec4(r.center, radius);
			push.probe = glm::vec4(float(r.cubemapIndex), r.mipLevels,
				intensity, float(samplerIndex));
			push.frame = frameUniformsAddress;
			cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
			cmd.DrawIndexed(indexCount, 1, 0, 0, 0);
		}
	});
	return true;
}

} // namespace smvk::probeball
