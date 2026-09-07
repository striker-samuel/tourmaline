#include <Engine/Asset/mesh_optimizer.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstddef>
#include <unordered_map>

#include <meshoptimizer.h>

#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>

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

bool BuildLodChainForUpload(
	const StaticMeshUpload& lod0,
	const MeshOptConfig& config,
	std::vector<CookedLod>& out) {
	out.clear();
	const size_t vcount = lod0.vertices.size();
	const size_t icount = lod0.indices.size();
	if (vcount == 0 || icount < 3 || (icount % 3) != 0)
		return false;
	if (!config.generateLods || config.lodRatios.empty())
		return true; // valid input, no candidates requested

	static_assert(offsetof(StaticVertex, pos) == 0, "pos must be first for meshopt");
	const float* positions = &lod0.vertices[0].pos.x;
	const float localScale = meshopt_simplifyScale(positions, vcount, sizeof(StaticVertex));
	if (!(localScale > 0.0f) || !std::isfinite(localScale))
		return true; // degenerate extents: nothing sensible to simplify

	const size_t baseTris = icount / 3;
	size_t lastKeptIndices = icount;
	const float targetError = std::clamp(config.lodTargetError, 0.0f, 1.0f);

	std::vector<unsigned int> dest(icount);
	for (float ratio : config.lodRatios) {
		if (!(ratio > 0.0f) || !(ratio < 1.0f))
			continue; // ignore nonsense ratios; keeps chain strictly coarser
		size_t targetTris = static_cast<size_t>(static_cast<double>(baseTris) * static_cast<double>(ratio));
		targetTris = std::max<size_t>(targetTris, 1);
		const size_t targetIndices = targetTris * 3;
		if (targetIndices >= lastKeptIndices)
			continue; // must be coarser than the last kept LOD

		float relativeError = 0.0f;
		const size_t resultCount = meshopt_simplify(
			dest.data(),
			lod0.indices.data(), icount,
			positions, vcount, sizeof(StaticVertex),
			targetIndices, targetError, config.lodSimplifyOptions,
			&relativeError);

		// --- Error-driven filter: drop broken candidates. ---
		if (resultCount < 3 || (resultCount % 3) != 0)
			continue;
		if (!std::isfinite(relativeError))
			continue;
		const size_t resultTris = resultCount / 3;
		if (resultTris < config.lodMinTriangles)
			continue; // tiny sliver: not worth a draw
		if (resultCount >= lastKeptIndices)
			continue; // simplifier got stuck: no reduction
		const double reduction =
			(static_cast<double>(lastKeptIndices) - static_cast<double>(resultCount)) /
			static_cast<double>(lastKeptIndices);
		if (reduction < static_cast<double>(config.lodMinReduction))
			continue; // not coarser enough to matter

		// Compact + clean the kept candidate so it uploads like LOD0:
		// vertex-cache order, then fetch remap with unreferenced trim.
		std::vector<uint32_t> indices(resultCount);
		for (size_t i = 0; i < resultCount; ++i)
			indices[i] = dest[i];
		{
			std::vector<uint32_t> optimized(resultCount);
			meshopt_optimizeVertexCache(
				reinterpret_cast<unsigned int*>(optimized.data()),
				reinterpret_cast<const unsigned int*>(indices.data()),
				resultCount, vcount);
			indices = std::move(optimized);
		}
		std::vector<unsigned int> remap(vcount);
		meshopt_optimizeVertexFetchRemap(
			remap.data(), reinterpret_cast<const unsigned int*>(indices.data()),
			resultCount, vcount);
		std::vector<StaticVertex> verts(vcount);
		meshopt_remapVertexBuffer(
			verts.data(), lod0.vertices.data(), vcount, sizeof(StaticVertex), remap.data());
		std::vector<uint32_t> remapped(resultCount);
		meshopt_remapIndexBuffer(
			reinterpret_cast<unsigned int*>(remapped.data()),
			reinterpret_cast<const unsigned int*>(indices.data()), resultCount,
			remap.data());
		size_t usedVerts = 0;
		for (uint32_t idx : remapped)
			usedVerts = std::max(usedVerts, static_cast<size_t>(idx) + 1);
		verts.resize(usedVerts);

		CookedLod kept;
		kept.upload.vertices = std::move(verts);
		kept.upload.indices = std::move(remapped);
		kept.upload.surfaces = lod0.surfaces; // single-surface layout preserved
		kept.relativeError = relativeError;
		kept.localError = relativeError * localScale;
		kept.triCount = static_cast<uint32_t>(resultTris);
		// Recompute bounds for the decimated positions.
		if (!kept.upload.vertices.empty()) {
			glm::vec3 mn = kept.upload.vertices[0].pos;
			glm::vec3 mx = kept.upload.vertices[0].pos;
			for (const auto& v : kept.upload.vertices) {
				mn = glm::min(mn, v.pos);
				mx = glm::max(mx, v.pos);
			}
			kept.upload.aabbMin = mn;
			kept.upload.aabbMax = mx;
		}
		else {
			kept.upload.aabbMin = lod0.aabbMin;
			kept.upload.aabbMax = lod0.aabbMax;
		}
		out.push_back(std::move(kept));
		lastKeptIndices = resultCount;
	}
	return true;
}

float MaxScaleOfMatrix(const glm::mat4& m) {
	// Max length of the three basis columns (translation ignored).
	const float sx = glm::length(glm::vec3(m[0]));
	const float sy = glm::length(glm::vec3(m[1]));
	const float sz = glm::length(glm::vec3(m[2]));
	float s = std::max(sx, std::max(sy, sz));
	return (std::isfinite(s) && s > 0.0f) ? s : 1.0f;
}

std::string LodSiblingUri(const std::string& baseUri, uint32_t lodIndex) {
	return baseUri + "/lod" + std::to_string(lodIndex);
}

std::string BaseMeshUriForLod(const std::string& uri) {
	// Strip a trailing "/lodN" (N >= 1 digits) if present.
	const size_t slash = uri.rfind("/lod");
	if (slash == std::string::npos)
		return uri;
	bool digits = false;
	for (size_t i = slash + 4; i < uri.size(); ++i) {
		if (uri[i] < '0' || uri[i] > '9')
			return uri;
		digits = true;
	}
	return (digits && slash > 0) ? uri.substr(0, slash) : uri;
}

void FinalizeLodWorldErrors(Cluster& cluster) {
	const auto lodEntries = cluster.ListMeshLods();
	if (lodEntries.empty())
		return;
	bool anyLod = false;
	for (const auto& [uri, lod] : lodEntries) {
		if (lod.lodIndex > 0) { anyLod = true; break; }
	}
	if (!anyLod)
		return;

	Scene& scene = cluster.scene;
	if (scene.globalTransform.size() != scene.hierarchy.size() || scene.hierarchy.empty())
		return;

	// Max world scale per base-mesh pool index over all instancing nodes.
	std::unordered_map<uint32_t, float> maxScaleByMesh;
	for (const auto& [node, meshIdx] : scene.meshForNode) {
		if (node >= scene.globalTransform.size())
			continue;
		const float s = MaxScaleOfMatrix(scene.globalTransform[node]);
		auto it = maxScaleByMesh.find(meshIdx);
		if (it == maxScaleByMesh.end() || s > it->second)
			maxScaleByMesh[meshIdx] = s;
	}
	// Base URI -> pool index (LOD0 handle) for scale lookup.
	std::unordered_map<std::string, uint32_t> poolByBaseUri;
	for (const auto& [node, meshIdx] : scene.meshForNode) {
		ClusterStaticMeshHandle h = cluster.staticMeshes.getHandle(meshIdx);
		if (!h.valid())
			continue;
		const std::string baseUri = cluster.GetMeshUri(h);
		if (!baseUri.empty())
			poolByBaseUri.emplace(baseUri, meshIdx);
	}

	for (const auto& [uri, lod] : lodEntries) {
		if (lod.lodIndex == 0)
			continue;
		const std::string baseUri = BaseMeshUriForLod(uri);
		auto bit = poolByBaseUri.find(baseUri);
		if (bit == poolByBaseUri.end())
			continue;
		auto sit = maxScaleByMesh.find(bit->second);
		const float nodeScale = (sit != maxScaleByMesh.end()) ? sit->second : 1.0f;
		LodInfo scaled = lod;
		scaled.simplifyError = lod.simplifyError * nodeScale;
		cluster.RegisterMeshLod(uri, scaled);
	}
}

} // namespace smvk
