// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Tiled terrain heightfield, baked from authored H-tiles — see
// Rendergraph/terrain.h. CPU bake: per-block LOD grids from the H-field
// (terrain_blocks.h) into one vertex buffer + one shared index buffer per
// LOD. Draw: 6-plane frustum cull + distance LOD, grouped into three
// pipeline phases (one per LOD topology).

#include <Engine/Rendergraph/terrain.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/Rendergraph/terrain_blocks.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace smvk::terrain {

bool TerrainField::EnsureShaders(ResourceManager& ctx,
	const char* vsPath, const char* psPath, const char* farVsPath) {
	bool ok = true;
	try {
		if (vsModule.empty())
			vsModule = ctx.CreateShaderModule(
				readFile(vsPath ? vsPath : kTerrainVsPath), "terrainVert");
		if (psModule.empty())
			psModule = ctx.CreateShaderModule(
				readFile(psPath ? psPath : kTerrainPsPath), "terrainFrag");
		if (farVsModule.empty())
			farVsModule = ctx.CreateShaderModule(
				readFile(farVsPath ? farVsPath : kTerrainFarVsPath),
				"terrainFarVert");
	}
	catch (...) {
		ok = false;
	}
	if (vsModule.empty() || psModule.empty())
		ok = false;
	if (ok && !ShadersValid()) {
		Shader vs{ .module = vsModule, .entryPoint = "mainVS",
			.stage = ShaderStage::Vertex };
		Shader ps{ .module = psModule, .entryPoint = "mainPS",
			.stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
	}
	if (farVsModule.empty() || psModule.empty())
		ok = false;
	if (ok && !FarShadersValid()) {
		Shader vs{ .module = farVsModule, .entryPoint = "mainVS",
			.stage = ShaderStage::Vertex };
		Shader ps{ .module = psModule, .entryPoint = "mainPS",
			.stage = ShaderStage::Fragment };
		farProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
	}
	return ok;
}

void TerrainField::DestroyShaders() {
	program = ShaderProgram{};
	farProgram = ShaderProgram{};
	vsModule = {};
	psModule = {};
	farVsModule = {};
}

bool TerrainField::BuildFromSource(ResourceManager& ctx,
	const TerrainSource& src, const TerrainDesc& d) {
	if (!src.hasTerrain || !src.hfield.valid() || src.masks.size() != 256) {
		std::fprintf(stderr,
			"[terrain] no valid terrain source (empty or broken harvest)\n");
		return false;
	}
	const TerrainHField& hf = src.hfield;

	Destroy();
	desc = d;
	hfield = hf; // retained for queries (collision / T3 CPU)
	blocks.resize(kTerrainBlockCount);

	MaskSet masks;
	masks.tiles = &src.masks;
	masks.tilesPerAxis = 16;
	masks.res = kMaskTileRes;

	// ---- per-LOD shared topologies ------------------------------------
	// Winding references: real baked positions (L0 block (0,0) for the
	// near pattern; a 16-grid height sample for the far pattern — same
	// ring layout, resolution-independent check).
	std::vector<std::vector<uint32_t>> lodIndices(3);
	{
		GBlockMesh ref;
		BakeGBlockVerts(hf, masks, 0, 0, kBlockLods[0], kSkirtDepth, ref);
		std::vector<glm::vec3> refPos;
		refPos.reserve(ref.verts.size());
		for (const auto& v : ref.verts)
			refPos.push_back(v.pos);
		BuildBlockIndices(kBlockLods[0], lodIndices[0], refPos);
		BuildBlockIndices(kBlockLods[1], lodIndices[1], {});
		BuildBlockIndices(kBlockLods[2], lodIndices[2], {});
	}
	for (int l = 0; l < 3; ++l)
		indicesPerLod[size_t(l)] = uint32_t(lodIndices[size_t(l)].size());

	// ---- per-block verts (L0 AABB first, then all LOD ranges) ----------
	std::vector<TerrainVertex> verts;
	verts.reserve(size_t(1600000));
	std::vector<TerrainFarVertex> farVerts;
	farVerts.reserve(size_t(100000));
	float globalMinY = 1e30f;
	float globalMaxY = -1e30f;
	GBlockMesh scratch;
	std::vector<TerrainFarVertex> farScratch;
	for (uint32_t gz = 0; gz < kTerrainBlocksPerAxis; ++gz) {
		for (uint32_t gx = 0; gx < kTerrainBlocksPerAxis; ++gx) {
			TerrainBlockInfo& info =
				blocks[size_t(gz) * size_t(kTerrainBlocksPerAxis) + gx];
			for (int l = 0; l < 3; ++l) {
				BakeGBlockVerts(hf, masks, gx, gz, kBlockLods[l],
					kSkirtDepth, scratch);
				info.vertexBase[size_t(l)] =
					int32_t(verts.size());
				verts.insert(verts.end(), scratch.verts.begin(),
					scratch.verts.end());
				if (l == 0) {
					info.min = scratch.bmin;
					info.max = scratch.bmax;
					globalMinY = std::min(globalMinY, scratch.bmin.y);
					globalMaxY = std::max(globalMaxY, scratch.bmax.y);
				}
			}
			BakeFarBlockVerts(hf, masks, gx, gz, farScratch);
			info.farVertexBase = int32_t(farVerts.size());
			farVerts.insert(farVerts.end(), farScratch.begin(),
				farScratch.end());
		}
	}
	vertexCount = uint32_t(verts.size());
	farVertexCount = uint32_t(farVerts.size());
	// Shared far topology, wound from real 16-grid positions.
	std::vector<uint32_t> farIndices;
	{
		const TerrainMeta& meta = hf.meta;
		const float step = kBlockSize / float(kFarQuads);
		std::vector<glm::vec3> farPos;
		farPos.reserve(size_t(kFarQuads + 1) * size_t(kFarQuads + 1));
		for (uint32_t j = 0; j <= kFarQuads; ++j)
			for (uint32_t i = 0; i <= kFarQuads; ++i)
				farPos.emplace_back(meta.originX + float(i) * step,
					SampleHeight(hf, meta.originX + float(i) * step,
						meta.originZ + float(j) * step),
					meta.originZ + float(j) * step);
		// Skirt ring positions (dropped; only the horizontal facing
		// matters for the winding check).
		for (uint32_t k = 0; k < 4 * (kFarQuads + 1); ++k)
			farPos.emplace_back(0.0f, -1000.0f, 0.0f);
		BuildBlockIndices(kFarQuads, farIndices, farPos);
	}
	farIndexCount = uint32_t(farIndices.size());
	boundsMin = glm::vec3(hf.meta.originX, globalMinY, hf.meta.originZ);
	boundsMax = glm::vec3(hf.meta.originX + hf.meta.worldSize, globalMaxY,
		hf.meta.originZ + hf.meta.worldSize);

	// ---- GPU upload ----------------------------------------------------
	vertexBuffer = ctx.CreateBuffer(
		sizeof(TerrainVertex) * verts.size(),
		BufferType_Vertex, "terrain blocks vertices");
	if (vertexBuffer.empty()) {
		smvk::OnResourceExhaustion("terrain/blocks vertices");
		Destroy();
		return false;
	}
	for (int l = 0; l < 3; ++l) {
		indexBuffers[l] = ctx.CreateBuffer(
			sizeof(uint32_t) * lodIndices[size_t(l)].size(),
			BufferType_Index, "terrain blocks indices");
		if (indexBuffers[l].empty()) {
			smvk::OnResourceExhaustion("terrain/blocks indices");
			Destroy();
			return false;
		}
	}
	farVertexBuffer = ctx.CreateBuffer(
		sizeof(TerrainFarVertex) * farVerts.size(),
		BufferType_Vertex, "terrain far vertices");
	if (farVertexBuffer.empty()) {
		smvk::OnResourceExhaustion("terrain/far vertices");
		Destroy();
		return false;
	}
	farIndexBuffer = ctx.CreateBuffer(
		sizeof(uint32_t) * farIndices.size(),
		BufferType_Index, "terrain far indices");
	if (farIndexBuffer.empty()) {
		smvk::OnResourceExhaustion("terrain/far indices");
		Destroy();
		return false;
	}
	// Height atlas: 16 H-tiles merged into ONE 4096 R16_UNORM image.
	// RG16 bits copy verbatim (R=MSB, G=LSB -> uint16 = R*256+G), so the
	// .r sample IS h01 and hardware linear filtering interpolates TRUE
	// heights (packed-channel filtering would corrupt transitions).
	std::vector<uint16_t> atlasPixels;
	atlasPixels.resize(size_t(4096) * size_t(4096));
	for (uint32_t tz = 0; tz < 4; ++tz) {
		for (uint32_t tx = 0; tx < 4; ++tx) {
			const auto& px = hf.tiles[tz * 4 + tx];
			for (uint32_t py = 0; py < 1024; ++py) {
				for (uint32_t x = 0; x < 1024; ++x) {
					const size_t s = (size_t(py) * 1024 + x) * 4;
					atlasPixels[(size_t(tz) * 1024 + py) * 4096
						+ size_t(tx) * 1024 + x] =
						PackHeightR16(px[s], px[s + 1]);
				}
			}
		}
	}
	{
		ImageCreateInfo ci;
		ci.type = ImageType::Image2D;
		ci.extent = Extent3D{ 4096, 4096, 1 };
		ci.format = Format::R16_UNorm;
		ci.mipLevels = 1;
		ci.name = "terrain/height-atlas";
		heightAtlas = ctx.CreateTexture(ci, atlasPixels.data());
		if (heightAtlas.empty()) {
			smvk::OnResourceExhaustion("terrain/height-atlas");
			Destroy();
			return false;
		}
		atlasSlot = ctx.GetTextureBindlessIndex(heightAtlas);
	}
	ctx.UploadDataToBuffer(vertexBuffer, verts.data(),
		sizeof(TerrainVertex) * verts.size(),
		PipelineStageBit_AllGraphics, AccessFlags::VertexAttributeRead, 0, 0);
	for (int l = 0; l < 3; ++l) {
		ctx.UploadDataToBuffer(indexBuffers[size_t(l)],
			lodIndices[size_t(l)].data(),
			sizeof(uint32_t) * lodIndices[size_t(l)].size(),
			PipelineStageBit_AllGraphics, AccessFlags::IndexRead, 0, 0);
	}
	ctx.UploadDataToBuffer(farVertexBuffer, farVerts.data(),
		sizeof(TerrainFarVertex) * farVerts.size(),
		PipelineStageBit_AllGraphics, AccessFlags::VertexAttributeRead, 0, 0);
	ctx.UploadDataToBuffer(farIndexBuffer, farIndices.data(),
		sizeof(uint32_t) * farIndices.size(),
		PipelineStageBit_AllGraphics, AccessFlags::IndexRead, 0, 0);
	valid = true;

	if (!EnsureShaders(ctx)) {
		std::fprintf(stderr,
			"[terrain] shader compile failed - the field stays built but "
			"draws are skipped until the .spv files exist\n");
	}
	std::fprintf(stderr, "[terrain] built: %u blocks x 4 LODs "
		"(%u near verts, %u far verts, idx %u/%u/%u/%u, atlas slot %u) "
		"heights [%.1f, %.1f]\n",
		kTerrainBlockCount, vertexCount, farVertexCount, indicesPerLod[0],
		indicesPerLod[1], indicesPerLod[2], farIndexCount, atlasSlot,
		globalMinY, globalMaxY);
	return true;
}

void TerrainField::Destroy() {
	vertexBuffer = {};
	for (auto& ib : indexBuffers)
		ib = {};
	farVertexBuffer = {};
	farIndexBuffer = {};
	heightAtlas = {};
	atlasSlot = 0xFFFFFFFFu;
	hfield = TerrainHField{};
	valid = false;
	blocks.clear();
	indicesPerLod = { 0, 0, 0 };
	farIndexCount = 0;
	vertexCount = 0;
	farVertexCount = 0;
	boundsMin = glm::vec3(0.0f);
	boundsMax = glm::vec3(0.0f);
}

namespace {

// Frustum planes from a (post Y-flip, reversed-Z ok) viewProj: the 6
// row-pair planes, Gribb/Hartmann extraction. Planes face INWARD (a point
// inside satisfies dot(p, n) + d >= 0 for all 6).
inline void ExtractFrustumPlanes(const glm::mat4& m, glm::vec4 out[6]) {
	// Rows of m (column-major glm: m[i] is column i; row j = (m[0][j],
	// m[1][j], m[2][j], m[3][j])).
	const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
	const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
	const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
	const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
	// left, right, bottom, top (w + the respective row); reversed-Z:
	// near/far flip to r3-r2 / r3+r2, which still bound the same volume.
	out[0] = r3 + r0;
	out[1] = r3 - r0;
	out[2] = r3 + r1;
	out[3] = r3 - r1;
	out[4] = r3 - r2;
	out[5] = r3 + r2;
	for (int i = 0; i < 6; ++i) {
		const float l = glm::length(glm::vec3(out[i]));
		if (l > 1e-12f)
			out[i] /= l;
	}
}

// AABB vs 6 inward planes: outside when fully behind any one plane.
inline bool AabbInFrustum(const glm::vec3& mn, const glm::vec3& mx,
	const glm::vec4 planes[6]) {
	for (int i = 0; i < 6; ++i) {
		const glm::vec3 p(
			planes[i].x > 0.0f ? mx.x : mn.x,
			planes[i].y > 0.0f ? mx.y : mn.y,
			planes[i].z > 0.0f ? mx.z : mn.z);
		if (glm::dot(p, glm::vec3(planes[i])) + planes[i].w < 0.0f)
			return false;
	}
	return true;
}

} // namespace

uint32_t TerrainField::RecordBlocks(CommandBuffer& cmd,
	const glm::mat4& viewProj, const glm::vec3& eye,
	const engine::IRenderer::PushConstants& push, const DepthSettings& depth,
	const StencilSettings& stencil, const BlendSettings& blend,
	const MSAASettings& msaa, int hfShadow, int hfSteps) {
	if (!valid || blocks.empty() || vertexBuffer.empty())
		return 0;
	if (!ShadersValid())
		return 0; // built but shaders missing: nothing to draw

	glm::vec4 planes[6];
	ExtractFrustumPlanes(viewProj, planes);

	// Cull + LOD split per block. Distance = closest approach from the
	// eye to the block AABB (squared, vs the desc splits).
	std::vector<uint32_t> lodBlocks[4];
	const float near2 = desc.lodNearFull * desc.lodNearFull;
	const float mid2 = desc.lodMid * desc.lodMid;
	const float far2 = desc.lodFar * desc.lodFar;
	const float maxD2 = desc.maxDrawDistance * desc.maxDrawDistance;
	for (uint32_t bi = 0; bi < blocks.size(); ++bi) {
		const TerrainBlockInfo& b = blocks[bi];
		if (!AabbInFrustum(b.min, b.max, planes))
			continue;
		const glm::vec3 d = glm::clamp(eye, b.min, b.max) - eye;
		const float d2 = glm::dot(d, d);
		if (d2 > maxD2)
			continue;
		lodBlocks[d2 > far2 ? 3 : (d2 > mid2 ? 2 : (d2 > near2 ? 1 : 0))]
			.push_back(bi);
	}
	if (lodBlocks[0].empty() && lodBlocks[1].empty()
		&& lodBlocks[2].empty() && lodBlocks[3].empty())
		return 0;

	// Shared tail words (atlas + world mapping + shadow mode).
	TerrainBlockPush tail{};
	tail.atlasSlot = atlasSlot;
	tail.originX = hfield.meta.originX;
	tail.originZ = hfield.meta.originZ;
	tail.invWorldSize = 1.0f / hfield.meta.worldSize;
	tail.baseY = hfield.meta.baseY;
	tail.range = hfield.meta.range;
	tail.hfShadow = hfShadow;
	tail.hfSteps = hfSteps;

	uint32_t drawn = 0;
	// The scene pc rides every bind: pushed AFTER each pipeline bind so
	// the draws never depend on a prior push in the pass. The terrain
	// tail overlays bytes 192..224 after it (placement precedent).
	const auto pushPcTail = [&cmd, &push, &tail]() {
		cmd.PushConstants(0, sizeof(engine::IRenderer::PushConstants), &push,
			ShaderStage::All);
		cmd.PushConstants(kTerrainPushOffset, sizeof(TerrainBlockPush),
			&tail, ShaderStage::All);
	};
	// Near phases: baked verts, one tail push per LOD (world-uniform).
	cmd.BindVertexBuffer(vertexBuffer, 0, 0);
	for (int l = 0; l < 3; ++l) {
		if (lodBlocks[l].empty() || indexBuffers[l].empty())
			continue;
		cmd.BindIndexBuffer(indexBuffers[l], 0);
		cmd.BindRenderPipeline(program,
			VertexFormat::TerrainVertexFormat, depth, stencil, blend, msaa,
			RasterSettings{} /* back-face cull, CCW front (block winding) */);
		pushPcTail();
		for (uint32_t bi : lodBlocks[l]) {
			cmd.DrawIndexed(indicesPerLod[size_t(l)], 1, 0,
				blocks[bi].vertexBase[size_t(l)], 0);
			++drawn;
		}
	}
	// Far phase: VS-fetch verts, one tail push per block (identical words
	// except hfShadow forced FULL on) + block id via firstInstance.
	if (!lodBlocks[3].empty() && !farVertexBuffer.empty()
		&& !farIndexBuffer.empty() && FarShadersValid()) {
		cmd.BindVertexBuffer(farVertexBuffer, 0, 0);
		cmd.BindIndexBuffer(farIndexBuffer, 0);
		cmd.BindRenderPipeline(farProgram,
			VertexFormat::TerrainFarVertexFormat, depth, stencil, blend,
			msaa, RasterSettings{});
		pushPcTail();
		TerrainBlockPush farTail = tail;
		farTail.hfShadow = 2;
		for (uint32_t bi : lodBlocks[3]) {
			cmd.PushConstants(0,
				sizeof(engine::IRenderer::PushConstants), &push,
				ShaderStage::All);
			cmd.PushConstants(kTerrainPushOffset, sizeof(TerrainBlockPush),
				&farTail, ShaderStage::All);
			cmd.DrawIndexed(farIndexCount, 1, 0,
				blocks[bi].farVertexBase, bi);
			++drawn;
		}
	}
	return drawn;
}

} // namespace smvk::terrain
