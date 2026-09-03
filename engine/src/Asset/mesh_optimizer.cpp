#include <Engine/Asset/mesh_optimizer.h>

#include <vector>

#include <meshoptimizer.h>

namespace smvk {
namespace {

float ComputeAcmr(const uint32_t* indices, size_t indexCount, size_t vertexCount) {
	if (indexCount == 0 || vertexCount == 0)
		return 0.0f;
	const meshopt_VertexCacheStatistics stats =
		meshopt_analyzeVertexCache(indices, indexCount, vertexCount, 16, 32, 128);
	// acmr = transform invocations per triangle.
	return static_cast<float>(static_cast<double>(stats.vertices_transformed) /
		static_cast<double>(indexCount / 3));
}

float ComputeOverdraw(
	const uint32_t* indices, size_t indexCount, const StaticVertex* vertices, size_t vertexCount) {
	if (indexCount < 3 || vertexCount == 0)
		return 1.0f;
	const meshopt_OverdrawStatistics stats = meshopt_analyzeOverdraw(
		indices, indexCount, &vertices[0].pos.x, vertexCount, sizeof(StaticVertex));
	return stats.overdraw;
}

void RecomputeBounds(StaticMeshUpload& upload) {
	if (upload.vertices.empty()) {
		upload.aabbMin = glm::vec3(0.0f);
		upload.aabbMax = glm::vec3(0.0f);
		return;
	}
	glm::vec3 mn = upload.vertices[0].pos;
	glm::vec3 mx = upload.vertices[0].pos;
	for (const auto& v : upload.vertices) {
		mn = glm::min(mn, v.pos);
		mx = glm::max(mx, v.pos);
	}
	upload.aabbMin = mn;
	upload.aabbMax = mx;
}

} // namespace

bool OptimizeStaticMeshUpload(
	StaticMeshUpload& upload,
	const MeshOptConfig& config,
	uint32_t lodGroupId,
	MeshOptStats* outStats,
	LodInfo* outLod) {
	MeshOptStats stats;
	stats.lodGroupId = lodGroupId;

	const size_t vcount = upload.vertices.size();
	const size_t icount = upload.indices.size();
	stats.inVertices = static_cast<uint32_t>(vcount);
	stats.triangles = static_cast<uint32_t>(icount / 3);

	auto finish = [&](bool ok) {
		stats.outVertices = static_cast<uint32_t>(upload.vertices.size());
		if (outStats)
			*outStats = stats;
		if (outLod) {
			outLod->groupId = lodGroupId;
			outLod->lodIndex = 0;
			outLod->candidateCount = 0;
			outLod->simplifyError = 0.0f;
			outLod->screenThreshold = -1.0f;
			outLod->triCount = stats.triangles;
			outLod->acmr = stats.acmrAfter > 0.0f ? stats.acmrAfter : stats.acmrBefore;
		}
		return ok;
	};

	// Degenerate input: leave untouched, still report LOD marker fields.
	if (!config.enabled || vcount == 0 || icount < 3 || (icount % 3) != 0)
		return finish(false);

	// StaticVertex must be bit-comparable for the weld (no padding bytes).
	static_assert(sizeof(StaticVertex) == 60, "StaticVertex layout changed; update weld assumptions");
	static_assert(alignof(StaticVertex) <= 4, "StaticVertex alignment changed");

	stats.acmrBefore = ComputeAcmr(upload.indices.data(), icount, vcount);
	stats.overdrawBefore = ComputeOverdraw(upload.indices.data(), icount, upload.vertices.data(), vcount);

	std::vector<uint32_t> indices = upload.indices; // working copy

	// 1. Weld: dedup binary-identical vertices.
	if (config.weld) {
		std::vector<unsigned int> remap(vcount);
		const size_t uniqueCount = meshopt_generateVertexRemap(
			remap.data(),
			indices.data(), icount,
			upload.vertices.data(), vcount, sizeof(StaticVertex));
		if (uniqueCount > 0 && uniqueCount < vcount) {
			std::vector<StaticVertex> welded(uniqueCount);
			meshopt_remapVertexBuffer(
				welded.data(), upload.vertices.data(), vcount, sizeof(StaticVertex), remap.data());
			upload.vertices = std::move(welded);
		}
		std::vector<uint32_t> remapped(icount);
		meshopt_remapIndexBuffer(
			reinterpret_cast<unsigned int*>(remapped.data()),
			reinterpret_cast<const unsigned int*>(indices.data()), icount,
			remap.data());
		indices = std::move(remapped);
	}

	const size_t nvcount = upload.vertices.size();

	// 2. Vertex-cache optimization.
	if (config.vertexCache) {
		std::vector<uint32_t> optimized(icount);
		meshopt_optimizeVertexCache(
			reinterpret_cast<unsigned int*>(optimized.data()),
			reinterpret_cast<const unsigned int*>(indices.data()), icount, nvcount);
		indices = std::move(optimized);
	}

	// 3. Overdraw optimization. Needs float3 positions at offset 0, which
	// StaticVertex guarantees (pos is the first member).
	if (config.overdraw) {
		std::vector<uint32_t> optimized(icount);
		meshopt_optimizeOverdraw(
			reinterpret_cast<unsigned int*>(optimized.data()),
			reinterpret_cast<const unsigned int*>(indices.data()), icount,
			&upload.vertices[0].pos.x, nvcount, sizeof(StaticVertex),
			config.overdrawThreshold);
		indices = std::move(optimized);
	}

	// 4. Vertex-fetch optimization (reorders vertices; must run last).
	if (config.vertexFetch) {
		std::vector<unsigned int> remap(nvcount);
		meshopt_optimizeVertexFetchRemap(
			remap.data(), reinterpret_cast<const unsigned int*>(indices.data()), icount, nvcount);
		std::vector<StaticVertex> reordered(nvcount);
		meshopt_remapVertexBuffer(
			reordered.data(), upload.vertices.data(), nvcount, sizeof(StaticVertex), remap.data());
		std::vector<uint32_t> reorderedIdx(icount);
		meshopt_remapIndexBuffer(
			reinterpret_cast<unsigned int*>(reorderedIdx.data()),
			reinterpret_cast<const unsigned int*>(indices.data()), icount, remap.data());
		// Trim unreferenced tail vertices.
		size_t usedVerts = 0;
		for (uint32_t idx : reorderedIdx)
			usedVerts = (static_cast<size_t>(idx) + 1 > usedVerts) ? static_cast<size_t>(idx) + 1 : usedVerts;
		reordered.resize(usedVerts);
		upload.vertices = std::move(reordered);
		indices = std::move(reorderedIdx);
	}

	upload.indices = std::move(indices);
	RecomputeBounds(upload);

	stats.acmrAfter = ComputeAcmr(upload.indices.data(), upload.indices.size(), upload.vertices.size());
	stats.overdrawAfter =
		ComputeOverdraw(upload.indices.data(), upload.indices.size(),
			upload.vertices.data(), upload.vertices.size());
	return finish(true);
}

} // namespace smvk
