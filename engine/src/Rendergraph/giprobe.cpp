// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GI probe visualization — see Rendergraph/giprobe.h for the design.
// Icosphere build + the standalone instanced pass (own push block, no
// scene pc).

#include <Engine/Rendergraph/giprobe.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace smvk::giprobe {

namespace {

// Hardcoded single-subdivision icosphere: the 12-vertex icosahedron
// (golden-ratio lattice) with every face split once at its edge
// midpoints projected to the unit sphere — 42 vertices (12 + 30 edge
// midpoints), 80 triangles (20 * 4), all edges uniform. TerrainVertex
// format (pos = normal = the unit direction — the VS needs no other
// attribute; tangent/color are bound but undeclared, exactly like
// probeball's sphere).
bool BuildIcosphere(ResourceManager& ctx, GiProbeVisual& v) {
	// The icosahedron: (0, ±1, ±t) cyclic, t = golden ratio, normalized.
	const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
	std::vector<glm::vec3> dirs = {
		glm::normalize(glm::vec3(-1.0f,  t, 0.0f)),
		glm::normalize(glm::vec3( 1.0f,  t, 0.0f)),
		glm::normalize(glm::vec3(-1.0f, -t, 0.0f)),
		glm::normalize(glm::vec3( 1.0f, -t, 0.0f)),
		glm::normalize(glm::vec3(0.0f, -1.0f,  t)),
		glm::normalize(glm::vec3(0.0f,  1.0f,  t)),
		glm::normalize(glm::vec3(0.0f, -1.0f, -t)),
		glm::normalize(glm::vec3(0.0f,  1.0f, -t)),
		glm::normalize(glm::vec3( t, 0.0f, -1.0f)),
		glm::normalize(glm::vec3( t, 0.0f,  1.0f)),
		glm::normalize(glm::vec3(-t, 0.0f, -1.0f)),
		glm::normalize(glm::vec3(-t, 0.0f,  1.0f)),
	};
	// CCW-outward faces (the classic icosahedron table).
	const uint32_t kFaces[20][3] = {
		{ 0, 11,  5 }, { 0,  5,  1 }, { 0,  1,  7 }, { 0,  7, 10 }, { 0, 10, 11 },
		{ 1,  5,  9 }, { 5, 11,  4 }, { 11, 10, 2 }, { 10,  7, 6 }, { 7,  1,  8 },
		{ 3,  9,  4 }, { 3,  4,  2 }, { 3,  2,  6 }, { 3,  6,  8 }, { 3,  8,  9 },
		{ 4,  9,  5 }, { 2,  4, 11 }, { 6,  2, 10 }, { 8,  6,  7 }, { 9,  8,  1 },
	};
	// One subdivision: midpoints normalized to the sphere (the standard
	// icosphere; the midpoint cache keys on the undirected edge).
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> mid;
	const auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
		const auto key = std::minmax(a, b);
		auto it = mid.find(key);
		if (it != mid.end())
			return it->second;
		const uint32_t idx = uint32_t(dirs.size());
		dirs.push_back(glm::normalize(
			0.5f * (dirs[a] + dirs[b])));
		mid.emplace(key, idx);
		return idx;
	};
	std::vector<uint32_t> indices;
	indices.reserve(240);
	for (const auto& f : kFaces) {
		const uint32_t a = f[0], b = f[1], c = f[2];
		const uint32_t ab = midpoint(a, b);
		const uint32_t bc = midpoint(b, c);
		const uint32_t ca = midpoint(c, a);
		// Same winding as the parent face — the split preserves CCW.
		const uint32_t sub[4][3] = {
			{ a, ab, ca }, { b, bc, ab }, { c, ca, bc }, { ab, bc, ca },
		};
		for (const auto& s : sub) {
			indices.push_back(s[0]);
			indices.push_back(s[1]);
			indices.push_back(s[2]);
		}
	}
	// The hardcoded contract: 42 vertices, 80 triangles.
	if (dirs.size() != 42 || indices.size() != 240) {
		std::fprintf(stderr, "[giprobe] icosphere build bug: %zu verts / "
			"%zu indices (expected 42 / 240)\n",
			dirs.size(), indices.size());
		return false;
	}

	std::vector<TerrainVertex> vertices(42);
	for (size_t i = 0; i < dirs.size(); ++i) {
		TerrainVertexSrc src{};
		src.pos = dirs[i];
		src.normal = dirs[i];
		src.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
		src.color = glm::vec4(1.0f);
		PackTerrainVertex(src, vertices[i]);
	}

	v.sphereVB = ctx.CreateBuffer(sizeof(TerrainVertex) * 42,
		BufferType_Vertex, "gi probe icosphere");
	v.sphereIB = ctx.CreateBuffer(sizeof(uint32_t) * 240,
		BufferType_Index, "gi probe indices");
	if (v.sphereVB.empty() || v.sphereIB.empty()) {
		v.sphereVB = {};
		v.sphereIB = {};
		v.geometryValid = false;
		return false;
	}
	ctx.UploadDataToBuffer(v.sphereVB, vertices.data(),
		sizeof(TerrainVertex) * 42,
		PipelineStageBit_AllGraphics, AccessFlags::VertexAttributeRead, 0, 0);
	ctx.UploadDataToBuffer(v.sphereIB, indices.data(),
		sizeof(uint32_t) * 240,
		PipelineStageBit_AllGraphics, AccessFlags::IndexRead, 0, 0);
	v.vertexCount = 42;
	v.indexCount = 240;
	v.geometryValid = true;
	return true;
}

} // namespace

bool GiProbeVisual::Ensure(ResourceManager& ctx) {
	bool ok = true;
	if (!geometryValid)
		ok = BuildIcosphere(ctx, *this);
	try {
		if (vsModule.empty())
			vsModule = ctx.CreateShaderModule(
				readFile(kGiProbeVsPath), "giProbeVert");
		if (psModule.empty())
			psModule = ctx.CreateShaderModule(
				readFile(kGiProbePsPath), "giProbeFrag");
	}
	catch (...) {
		ok = false;
	}
	if (vsModule.empty() || psModule.empty()) {
		if (ok)
			std::fprintf(stderr, "[giprobe] shaders missing (no .spv?) "
				"- the probe spheres skip until they compile\n");
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

void GiProbeVisual::Destroy() {
	sphereVB = {};
	sphereIB = {};
	vertexCount = indexCount = 0;
	geometryValid = false;
	program = ShaderProgram();
	vsModule = {};
	psModule = {};
}

bool GiProbeVisual::AppendPass(RenderGraph& rg,
	LogicalImageViewHandle hdrView, LogicalImageViewHandle depthView,
	Extent2D extent, const glm::mat4& viewProj, const glm::vec3& eye,
	float maxDist, const GiProbeVolumeRow* rows, uint32_t rowCount,
	uint64_t frameUniformsAddress, uint32_t colorMode, float markerGain,
	uint32_t offsetX, uint32_t offsetY) {
	if (!geometryValid || !ShadersValid() || !rows || rowCount == 0
		|| hdrView.empty() || depthView.empty())
		return false;
	if (sphereVB.empty() || sphereIB.empty())
		return false;

	// Keep only drawable rows up front (empty buffers / zero counts
	// drop here, so the pass is skipped entirely when nothing can draw).
	std::vector<GiProbeVolumeRow> drawRows;
	drawRows.reserve(rowCount);
	for (uint32_t i = 0; i < rowCount; ++i)
		if (!rows[i].instances.empty() && rows[i].instancesBda != 0
			&& rows[i].instanceCount > 0)
			drawRows.push_back(rows[i]);
	if (drawRows.empty())
		return false;

	// The pass: HDR load/store + the scene depth TEST-ONLY — the
	// probeball slot contract (occluded probes hide behind geometry; the
	// depth the post-Main consumers march stays untouched).
	// Sub-rect support (the editor's per-viewport rects; full-frame
	// callers keep 0,0 - the App/probeball convention).
	RenderPass& pass = rg.AddPass("GiProbes")
		.SetOffset(offsetX, offsetY)
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
	// The recording captures `this` (the module lives on the
	// WorldRenderer, which outlives every frame's Execute) + the row
	// copies (plain values — the caller's tables may mutate after).
	pass.SetRecordingFunc([this, viewProj, eye, maxDist,
			frameUniformsAddress, colorMode, markerGain,
			drawRows = std::move(drawRows)](CommandBuffer& cmd) mutable {
		// Depth test AND write (GreaterEqual reversed-Z): the test hides
		// probes behind scene geometry like probeball's TEST-ONLY — but
		// the WRITE is load-bearing here: 1120+ overlapping spheres in
		// one instanced draw need sphere-vs-sphere occlusion, and without
		// writes the draw order decides (the see-through-probes bug — a
		// far sphere painted over a near one). Safe at this slot: nothing
		// AFTER the HDR tail (exposure / bloom / tonemap / capture / SMAA
		// / UI) reads depth — the depth-marching consumers (godrays,
		// volumetrics) all run BEFORE it.
		const DepthSettings depth{
			.depthFunc = CompareOp::GreaterEqual,
			.writeEnable = true,
			.depthTestEnable = true,
		};
		cmd.BindRenderPipeline(
			program,
			VertexLayoutId::TerrainFull,
			depth,
			StencilSettings{},
			BlendSettings::Opaque(),
			MSAASettings::Count1(),
			RasterSettings{} /* back-face cull, CCW front (sphere winding) */);
		cmd.BindVertexBuffer(sphereVB, 0, 0);
		cmd.BindIndexBuffer(sphereIB, 0);
		for (const GiProbeVolumeRow& r : drawRows) {
			GiProbePush push{};
			push.viewProj = viewProj;
			push.eyeMaxDist = glm::vec4(eye, maxDist);
			push.frame = frameUniformsAddress;
			push.instances = r.instancesBda;
			push.volumeIndex = r.volumeIndex;
			push.mode = colorMode;
			push.markerGain = markerGain;
			cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
			// ONE instanced call per volume: the shared icosphere over
			// this volume's instance rows (SV_InstanceID-indexed).
			cmd.DrawIndexed(indexCount,
				std::min(r.instanceCount, kGiProbeMaxInstances), 0, 0, 0);
		}
	});
	return true;
}

} // namespace tour::giprobe
