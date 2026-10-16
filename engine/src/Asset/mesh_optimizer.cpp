// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/mesh_optimizer.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include <meshoptimizer.h>

#include <Engine/App/cluster.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/GI/triangle_bvh.h>
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

void ComputeSmoothNormals(StaticMeshUpload& upload) {
	const size_t vcount = upload.vertices.size();
	if (vcount == 0) return;
	for (auto& v : upload.vertices) v.normal = glm::vec3(0.0f);
	// Dominant (largest-area) face normal per vertex, for the inversion
	// repair below. Squared-area weighting matches the accumulation, so
	// the dominant face is the accumulation's biggest contributor.
	std::vector<glm::vec3> dominant(vcount, glm::vec3(0.0f));
	std::vector<float> domArea2(vcount, 0.0f);
	auto accumTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
		if (i0 >= vcount || i1 >= vcount || i2 >= vcount) return;
		const glm::vec3& p0 = upload.vertices[i0].pos;
		const glm::vec3& p1 = upload.vertices[i1].pos;
		const glm::vec3& p2 = upload.vertices[i2].pos;
		const glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
		const float a2 = glm::dot(fn, fn);
		if (a2 <= 1e-24f) return; // degenerate
		upload.vertices[i0].normal += fn;
		upload.vertices[i1].normal += fn;
		upload.vertices[i2].normal += fn;
		const uint32_t ids[3] = { i0, i1, i2 };
		for (int k = 0; k < 3; ++k) {
			if (a2 > domArea2[ids[k]]) {
				domArea2[ids[k]] = a2;
				dominant[ids[k]] = fn;
			}
		}
	};
	if (!upload.indices.empty()) {
		for (size_t t = 0; t + 2 < upload.indices.size() + 1; t += 3)
			accumTri(upload.indices[t], upload.indices[t + 1], upload.indices[t + 2]);
	}
	else {
		for (uint32_t t = 0; t + 2 < vcount + 1; t += 3)
			accumTri(t, t + 1, t + 2);
	}
	size_t repaired = 0;
	for (size_t i = 0; i < vcount; ++i) {
		glm::vec3& n = upload.vertices[i].normal;
		const float l2 = glm::dot(n, n);
		if (!(l2 > 1e-24f)) {
			// No contributing faces (unreferenced vertex), or exact
			// cancellation across opposing faces: start from the old
			// up-fallback and fall through to the repair below, which
			// replaces it with the dominant face normal whenever one
			// exists (unreferenced vertices have none, so they keep the
			// fallback exactly as before).
			n = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		else {
			n = glm::normalize(n);
		}
		// Inversion repair: a smoothed normal that disagrees with its
		// biggest contributor (dot <= 0) comes from cancellation across
		// opposing faces — thin double-sided cards merged by decimation
		// (front/back quads sharing vertices average to ~0/garbage, and
		// the normalize then points anywhere, including backwards). Such
		// normals shade black where they interpolate backwards (NdotL <=
		// 0 kills the sun, the GI SH samples the dark lobe) — the
		// distant-LOD-sibling black-band repro (sponza atrium foliage).
		// Fall back to the dominant face normal, which is consistent
		// with at least one real triangle by construction. The gate is
		// strict (only true inversions): clean manifold meshes agree
		// with every adjacent face, so they never repair.
		if (domArea2[i] > 1e-24f
			&& glm::dot(n, dominant[i]) <= 0.0f) {
			n = glm::normalize(dominant[i]);
			++repaired;
		}
	}
	if (repaired > 0)
		std::cout << "  [MeshOpt] ComputeSmoothNormals: repaired "
			<< repaired << "/" << vcount << " inverted normals.\n" << std::flush;
}

void GenerateTangents(StaticMeshUpload& upload) {
	const size_t vcount = upload.vertices.size();
	if (vcount == 0) return;
	std::vector<glm::vec3> tan1(vcount, glm::vec3(0.0f));
	std::vector<glm::vec3> tan2(vcount, glm::vec3(0.0f));
	auto accumTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
		if (i0 >= vcount || i1 >= vcount || i2 >= vcount) return;
		const glm::vec3& p0 = upload.vertices[i0].pos;
		const glm::vec3& p1 = upload.vertices[i1].pos;
		const glm::vec3& p2 = upload.vertices[i2].pos;
		const glm::vec2& w0 = upload.vertices[i0].texCoord;
		const glm::vec2& w1 = upload.vertices[i1].texCoord;
		const glm::vec2& w2 = upload.vertices[i2].texCoord;
		const glm::vec3 x1 = p1 - p0;
		const glm::vec3 x2 = p2 - p0;
		const glm::vec2 s1 = w1 - w0;
		const glm::vec2 s2 = w2 - w0;
		const float denom = s1.x * s2.y - s2.x * s1.y;
		if (fabsf(denom) <= 1e-12f) return; // degenerate UVs: no tangent info
		const float r = 1.0f / denom;
		const glm::vec3 sdir = (x1 * s2.y - x2 * s1.y) * r;
		const glm::vec3 tdir = (x2 * s1.x - x1 * s2.x) * r;
		tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
		tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
	};
	if (!upload.indices.empty()) {
		for (size_t t = 0; t + 2 < upload.indices.size() + 1; t += 3)
			accumTri(upload.indices[t], upload.indices[t + 1], upload.indices[t + 2]);
	}
	else {
		for (uint32_t t = 0; t + 2 < vcount + 1; t += 3)
			accumTri(t, t + 1, t + 2);
	}
	for (size_t i = 0; i < vcount; ++i) {
		const glm::vec3& n = upload.vertices[i].normal;
		// Gram-Schmidt: make accumulated tangent orthogonal to N.
		glm::vec3 t = tan1[i] - n * glm::dot(n, tan1[i]);
		if (glm::dot(t, t) > 1e-24f) {
			t = glm::normalize(t);
		}
		else {
			// No usable UV gradient: build an arbitrary frame orthogonal to N.
			const glm::vec3 axis =
				(fabsf(n.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			t = glm::normalize(glm::cross(axis, n));
			if (glm::dot(t, t) <= 1e-24f) t = glm::vec3(1.0f, 0.0f, 0.0f);
		}
		const float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;
		upload.vertices[i].tangent = glm::vec4(t, w);
	}
}

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
	// chartId rides the weld key: seam twins (identical but for chart)
	// never merge — the split the stamp created survives optimization.
	static_assert(sizeof(StaticVertex) == 76, "StaticVertex layout changed; update weld assumptions");
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
	std::vector<CookedLod>& out,
	bool preserveCoverage,
	const uint32_t* lmVertexChart) {
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

	// Cutout foliage: pack UVs as a dense attribute buffer so the
	// attribute-aware simplifier can penalize UV distortion (which is what
	// the alpha test samples). Weights are in UV units; ~0.5 keeps UV
	// fidelity important without fully vetoing decimation.
	std::vector<float> cutoutAttrs;
	const float* cutoutAttrPtr = nullptr;
	float cutoutWeights[2] = { 0.5f, 0.5f };
	if (preserveCoverage) {
		cutoutAttrs.resize(vcount * 2);
		for (size_t i = 0; i < vcount; ++i) {
			cutoutAttrs[i * 2 + 0] = lod0.vertices[i].texCoord.x;
			cutoutAttrs[i * 2 + 1] = lod0.vertices[i].texCoord.y;
		}
		cutoutAttrPtr = cutoutAttrs.data();
	}

	// Default-path albedo-UV awareness (see header): when neither the
	// cutout nor the lightmap-chart path applies, texCoord still rides the
	// error metric, protecting UV-seam topology and biasing collapses
	// toward UV-preserving ones. Same weights as the cutout path but
	// WITHOUT LockBorder (general meshes must simplify borders).
	// Non-finite UVs fall back to positions-only; all-zero UVs (untextured
	// meshes) leave the attribute quadrics empty and behave positions-only.
	std::vector<float> uvAttrs;
	bool useUvAttrs = false;
	if (!preserveCoverage && !lmVertexChart && config.lodUvAware) {
		uvAttrs.resize(vcount * 2);
		bool finite = true;
		for (size_t i = 0; i < vcount; ++i) {
			const float u = lod0.vertices[i].texCoord.x;
			const float v = lod0.vertices[i].texCoord.y;
			if (!std::isfinite(u) || !std::isfinite(v)) {
				finite = false;
				break;
			}
			uvAttrs[i * 2 + 0] = u;
			uvAttrs[i * 2 + 1] = v;
		}
		useUvAttrs = finite;
		if (!useUvAttrs)
			uvAttrs.clear();
	}

	// Lightmap-aware attributes (stamped mesh + chart table): chart id
	// (+1 so 0 = uncharted) + chart-local uv. The chart term is the ONLY
	// rejector for the seam-twin collapse — seam corners are position-
	// identical duplicates, so that collapse has zero position error —
	// and weight 1.0 puts a one-chart delta far over the squared budget
	// (default 0.02^2). The uv terms (0.1, chart-local units) mildly
	// penalize parameterization distortion inside a chart: a whole-chart
	// sweep (~0.25 chart units) lands right at the default budget, while
	// ordinary collapses stay well under. See LodSeamLock for the Hard
	// alternative (vertex_lock on seam pairs).
	std::vector<float> lmAttrs;
	std::vector<unsigned char> lmLock;
	const unsigned char* lmLockPtr = nullptr;
	if (lmVertexChart && !preserveCoverage) {
		lmAttrs.resize(vcount * 3);
		for (size_t i = 0; i < vcount; ++i) {
			const uint32_t chart = lmVertexChart[i];
			lmAttrs[i * 3 + 0] = chart == 0xFFFFFFFFu
				? 0.0f : float(chart) + 1.0f;
			lmAttrs[i * 3 + 1] = lod0.vertices[i].texCoord2.x;
			lmAttrs[i * 3 + 2] = lod0.vertices[i].texCoord2.y;
		}
		if (config.lodSeamLock == LodSeamLock::Hard) {
			// Lock every member of a position run whose charts disagree
			// (seam duplicates are bit-identical position copies, so raw
			// memcmp grouping is exact). Collapses across the seam then
			// become impossible; the cost is locked vertices.
			lmLock.assign(vcount, 0);
			std::vector<uint32_t> byPos(vcount);
			for (uint32_t i = 0; i < vcount; ++i)
				byPos[i] = i;
			const StaticVertex* verts = lod0.vertices.data();
			std::sort(byPos.begin(), byPos.end(),
				[verts](uint32_t a, uint32_t b) {
					return std::memcmp(&verts[a].pos, &verts[b].pos, 12) < 0;
				});
			size_t runStart = 0;
			while (runStart < vcount) {
				size_t runEnd = runStart + 1;
				while (runEnd < vcount
					&& std::memcmp(&verts[byPos[runEnd]].pos,
						&verts[byPos[runStart]].pos, 12) == 0)
					++runEnd;
				if (runEnd - runStart > 1) {
					bool chartsDiffer = false;
					for (size_t r = runStart + 1; r < runEnd; ++r) {
						if (lmVertexChart[byPos[r]]
							!= lmVertexChart[byPos[runStart]]) {
							chartsDiffer = true;
							break;
						}
					}
					if (chartsDiffer) {
						for (size_t r = runStart; r < runEnd; ++r)
							lmLock[byPos[r]] = meshopt_SimplifyVertex_Lock;
					}
				}
				runStart = runEnd;
			}
			for (const unsigned char l : lmLock) {
				if (l) {
					lmLockPtr = lmLock.data();
					break;
				}
			}
		}
	}

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
		size_t resultCount = 0;
		if (preserveCoverage) {
			const unsigned options =
				config.lodSimplifyOptions | meshopt_SimplifyLockBorder;
			resultCount = meshopt_simplifyWithAttributes(
				dest.data(),
				lod0.indices.data(), icount,
				positions, vcount, sizeof(StaticVertex),
				cutoutAttrPtr, sizeof(float) * 2,
				cutoutWeights, 2, nullptr,
				targetIndices, targetError, options,
				&relativeError);
		}
		else if (!lmAttrs.empty()) {
			// Chart-aware: chart id + uv2 as error terms (Hard mode
			// additionally vertex-locks seam pairs; see block above).
			static const float kLmAttrWeights[3] = { 1.0f, 0.1f, 0.1f };
			resultCount = meshopt_simplifyWithAttributes(
				dest.data(),
				lod0.indices.data(), icount,
				positions, vcount, sizeof(StaticVertex),
				lmAttrs.data(), sizeof(float) * 3,
				kLmAttrWeights, 3, lmLockPtr,
				targetIndices, targetError, config.lodSimplifyOptions,
				&relativeError);
		}
		else if (useUvAttrs) {
			// Default: albedo-UV-aware positions+UV simplify (no border
			// lock — general meshes must simplify borders). The reported
			// error is the combined position+UV distortion in relative
			// units, converted to local units below like every path.
			static const float kUvAttrWeights[2] = { 0.5f, 0.5f };
			resultCount = meshopt_simplifyWithAttributes(
				dest.data(),
				lod0.indices.data(), icount,
				positions, vcount, sizeof(StaticVertex),
				uvAttrs.data(), sizeof(float) * 2,
				kUvAttrWeights, 2, nullptr,
				targetIndices, targetError, config.lodSimplifyOptions,
				&relativeError);
		}
		else {
			resultCount = meshopt_simplify(
				dest.data(),
				lod0.indices.data(), icount,
				positions, vcount, sizeof(StaticVertex),
				targetIndices, targetError, config.lodSimplifyOptions,
				&relativeError);
		}

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
		// The simplifier only tracks positions: surviving vertices still
		// carry LOD0's normals/tangents, which no longer match the decimated
		// topology and shade black where they interpolate backwards. Re-derive
		// the frame from this LOD's own connectivity (creases survive because
		// crease vertices are duplicated).
		ComputeSmoothNormals(kept.upload);
		GenerateTangents(kept.upload);
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

StringViewT LodSiblingUri(StringViewT baseUri, uint32_t lodIndex, Arena& scratch) {
	return scratch.FormatString("%.*s/lod%u",
		static_cast<int>(baseUri.size()), baseUri.data(), lodIndex);
}

StringViewT BaseMeshUriForLod(StringViewT uri) {
	// Strip a trailing "/lodN" (N >= 1 digits) if present (pure view slice).
	const size_t slash = uri.rfind('/');
	if (slash == StringViewT::npos || slash == 0)
		return uri;
	const StringViewT tail = uri.substr(slash + 1);
	if (!tail.starts_with("lod"))
		return uri;
	bool digits = false;
	for (size_t i = 3; i < tail.size(); ++i) {
		if (tail[i] < '0' || tail[i] > '9')
			return uri;
		digits = true;
	}
	return digits ? uri.substr(0, slash) : uri;
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
	// Current globals are load-bearing here (the max world scale below):
	// cook pipelines that skip the lightmap block (which recalculates as
	// a side effect) otherwise finalize against stale (identity-scale)
	// transforms, leaving LOCAL-unit errors — 125x too big for Sponza's
	// 0.008 root — and the runtime LOD selection pins at LOD0 at any
	// sane budget. Marking roots covers the whole scene; the recalculate
	// is incremental (a no-op when current).
	for (size_t n = 0; n < scene.hierarchy.size(); ++n)
		if (scene.hierarchy[n].parent < 0)
			::markAsChanged(scene, (int)n);
	::recalculateGlobalTransforms(scene);

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
	// Base URI -> pool index (LOD0 handle) for scale lookup. Keys are
	// cluster-arena views (GetMeshUri) — stable for the cluster lifetime,
	// so the UriMap stores them directly.
	UriMap<uint32_t> poolByBaseUri;
	for (const auto& [node, meshIdx] : scene.meshForNode) {
		ClusterStaticMeshHandle h = cluster.staticMeshes.getHandle(meshIdx);
		if (!h.valid())
			continue;
		const StringViewT baseUri = BaseMeshUriForLod(cluster.GetMeshUri(h));
		if (!baseUri.empty())
			poolByBaseUri.Insert(baseUri, meshIdx);
	}

	for (const auto& [uri, lod] : lodEntries) {
		if (lod.lodIndex == 0)
			continue;
		const StringViewT baseUri = BaseMeshUriForLod(uri);
		const uint32_t* poolIdx = poolByBaseUri.Find(baseUri);
		if (!poolIdx)
			continue;
		auto sit = maxScaleByMesh.find(*poolIdx);
		const float nodeScale = (sit != maxScaleByMesh.end()) ? sit->second : 1.0f;
		// Idempotent re-scale: sibling errors are stored world-scaled, so
		// divide out the scale a previous run baked in before applying
		// the current one. Without this, cook pipelines that finalize
		// twice (frostbite pre-bake sibling cook + post-bake sibling pass)
		// compound the node scale — for Sponza's 0.008 root that shrinks
		// every error 125x per extra run and the renderer drops to the
		// coarsest LOD centimeters from the camera.
		const float prevScale = cluster.GetLodFinalizedScale(uri);
		LodInfo scaled = lod;
		scaled.simplifyError = lod.simplifyError / prevScale * nodeScale;
		cluster.RegisterMeshLod(uri, scaled);
		cluster.SetLodFinalizedScale(uri, nodeScale);
	}
}

// ---------------------------------------------------------------------------
// Lightmap-safety audit (see mesh_optimizer.h).
// ---------------------------------------------------------------------------
bool AuditLodLightmap(const StaticMeshUpload& lod0,
	const StaticMeshUpload& sibling, const glm::mat4& world,
	const std::vector<glm::vec4>& chartXforms,
	uint32_t atlasWidth, uint32_t atlasHeight,
	float thresholdTexels, float searchDist, uint32_t maxTris,
	LodLightmapAuditReport& out) {
	out = LodLightmapAuditReport{};
	const size_t lod0Tris = lod0.indices.size() / 3;
	const size_t sibTris = sibling.indices.size() / 3;
	if (lod0Tris == 0 || sibTris == 0 || !atlasWidth || !atlasHeight)
		return false;
	if (lod0.vertices.empty() || sibling.vertices.empty())
		return false;

	// LOD0 projection target in world space (siblings share LOD0's node
	// transform, so both uploads land in the same space through `world`).
	std::vector<glm::vec3> worldPos(lod0.vertices.size());
	for (size_t i = 0; i < lod0.vertices.size(); ++i)
		worldPos[i] = glm::vec3(world * glm::vec4(lod0.vertices[i].pos, 1.0f));
	gi::TriangleBvh bvh;
	bvh.Build(std::span<const glm::vec3>(worldPos),
		std::span<const uint32_t>(lod0.indices));
	if (bvh.Empty())
		return false;

	// Chart-local -> atlas-space (same math as the VS unpack).
	auto resolveAtlas = [&chartXforms](const StaticVertex& v) {
		if (v.chartId != 0xFFFFFFFFu && v.chartId < chartXforms.size()) {
			const glm::vec4& xf = chartXforms[v.chartId];
			return v.texCoord2 * glm::vec2(xf.x, xf.y)
				+ glm::vec2(xf.z, xf.w);
		}
		return v.texCoord2;
	};
	// Cross-chart sweep over EVERY triangle (cheap, no projection): a
	// triangle whose corners disagree on chart id — or whose resolved
	// atlas corners sweep more than one chart rect — smears the whole
	// atlas between them. No texel number can describe that.
	auto uv2Span = [&sibling, &resolveAtlas](size_t t) {
		const StaticVertex& va = sibling.vertices[sibling.indices[t * 3 + 0]];
		const StaticVertex& vb = sibling.vertices[sibling.indices[t * 3 + 1]];
		const StaticVertex& vc = sibling.vertices[sibling.indices[t * 3 + 2]];
		if (va.chartId != vb.chartId || va.chartId != vc.chartId)
			return 1e30f; // chart-id mismatch: cross-chart by construction
		const glm::vec2 a = resolveAtlas(va);
		const glm::vec2 b = resolveAtlas(vb);
		const glm::vec2 c = resolveAtlas(vc);
		const float ab = std::abs(a.x - b.x) + std::abs(a.y - b.y);
		const float ac = std::abs(a.x - c.x) + std::abs(a.y - c.y);
		const float bc = std::abs(b.x - c.x) + std::abs(b.y - c.y);
		return std::max({ ab, ac, bc });
	};
	out.trisTotal = uint32_t(sibTris);
	for (size_t t = 0; t < sibTris; ++t)
		if (uv2Span(t) > kLodCrossChartUvSpan)
			++out.crossChartTris;

	// Projection audit over a deterministic triangle subsample.
	const size_t stride = std::max<size_t>(1, (sibTris + maxTris - 1) / maxTris);
	static const glm::vec3 kBary[3] = {
		glm::vec3(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f),
		glm::vec3(0.7f, 0.15f, 0.15f),
		glm::vec3(0.15f, 0.7f, 0.15f),
	};
	const glm::vec2 texelScale{ float(atlasWidth), float(atlasHeight) };
	const float eps = std::max(1e-5f, searchDist * 1e-3f);
	std::vector<float> errs;
	double errSum = 0.0;
	for (size_t t = 0; t < sibTris; t += stride) {
		++out.trisAudited;
		glm::vec3 wp[3];
		glm::vec2 uv[3];
		for (int k = 0; k < 3; ++k) {
			const StaticVertex& v = sibling.vertices[sibling.indices[t * 3 + k]];
			wp[k] = glm::vec3(world * glm::vec4(v.pos, 1.0f));
			uv[k] = resolveAtlas(v);
		}
		const glm::vec3 fn = glm::cross(wp[1] - wp[0], wp[2] - wp[0]);
		const float fnLen = glm::length(fn);
		if (!(fnLen > 1e-20f))
			continue; // degenerate triangle
		const glm::vec3 N = fn / fnLen;
		for (const glm::vec3& b : kBary) {
			const glm::vec3 P = b.x * wp[0] + b.y * wp[1] + b.z * wp[2];
			const glm::vec2 cand = b.x * uv[0] + b.y * uv[1] + b.z * uv[2];
			// Projection: normal rays both directions (scored by
			// distance + normal alignment, wrong-side folds rejected),
			// then the exact closest-point fallback (the first hit
			// along P -> closestPoint is the closest triangle: anything
			// nearer would contradict the closest distance).
			bool have = false;
			gi::TriangleBvh::Hit hit{};
			float bestScore = 0.0f;
			for (int s = 0; s < 2; ++s) {
				const glm::vec3 dir = s ? N : -N;
				gi::TriangleBvh::Hit h{};
				if (!bvh.Raycast(P - dir * eps, dir, searchDist, h))
					continue;
				const glm::vec3 hn = bvh.TriangleNormal(h.tri);
				const float align = std::abs(glm::dot(N, hn));
				if (align < 0.25f)
					continue;
				const float score = h.t * h.t + 0.05f * (1.0f - align);
				if (!have || score < bestScore) {
					have = true;
					bestScore = score;
					hit = h;
				}
			}
			if (!have) {
				glm::vec3 cp(0.0f);
				const float d = bvh.ClosestDistance(P, &cp);
				if (d < 1e30f && d > 1e-6f) {
					const glm::vec3 dir = (cp - P) / d;
					if (bvh.Raycast(P + dir * eps, dir, d * 1.001f + eps, hit))
						have = true;
				}
			}
			if (!have) {
				++out.unresolved;
				continue;
			}
			// True uv from the hit's barycentrics: baryUV.x weights V1,
			// baryUV.y weights V2, (1-x-y) weights V0 (IntersectTriangle
			// convention; Hit.tri is an original-soup triangle id).
			const uint32_t j0 = lod0.indices[hit.tri * 3 + 0];
			const uint32_t j1 = lod0.indices[hit.tri * 3 + 1];
			const uint32_t j2 = lod0.indices[hit.tri * 3 + 2];
		const float w0 = 1.0f - hit.baryUV.x - hit.baryUV.y;
		const glm::vec2 trueUv =
			w0 * resolveAtlas(lod0.vertices[j0])
			+ hit.baryUV.x * resolveAtlas(lod0.vertices[j1])
			+ hit.baryUV.y * resolveAtlas(lod0.vertices[j2]);
			const glm::vec2 dTex = (cand - trueUv) * texelScale;
			const float e = std::sqrt(glm::dot(dTex, dTex));
			++out.samples;
			errSum += e;
			errs.push_back(e);
			if (e > thresholdTexels)
				++out.overThreshold;
			out.maxTexels = std::max(out.maxTexels, e);
		}
	}
	if (out.samples) {
		out.meanTexels = float(errSum / double(out.samples));
		std::sort(errs.begin(), errs.end());
		out.p95Texels = errs[std::min<size_t>(errs.size() - 1,
			size_t(float(errs.size()) * 0.95f))];
	}
	return true;
}

// ---------------------------------------------------------------------------
// Post-bake LOD sibling cooking (see mesh_optimizer.h).
// ---------------------------------------------------------------------------
bool CookLodSiblings(Cluster& cluster, const MeshOptConfig& config,
	const MeshletConfig& meshletConfig,
	const CookLodSiblingsOptions& options) {
	if (!config.generateLods)
		return true;
	Scene& scene = cluster.scene;

	// Mesh pool indices in node usage (instanced meshes share one pool
	// entry; the chain is per MESH — node transforms only matter for the
	// audit). Sorted for deterministic registration/pool order.
	std::unordered_map<uint32_t, uint32_t> nodeOfMesh;
	for (const auto& [node, meshIdx] : scene.meshForNode) {
		if (!nodeOfMesh.count(meshIdx))
			nodeOfMesh[meshIdx] = node;
	}
	std::vector<uint32_t> meshOrder;
	meshOrder.reserve(nodeOfMesh.size());
	for (const auto& [meshIdx, node] : nodeOfMesh)
		meshOrder.push_back(meshIdx);
	std::sort(meshOrder.begin(), meshOrder.end());

	for (const uint32_t meshIdx : meshOrder) {
		const uint32_t node = nodeOfMesh[meshIdx];
		const ClusterStaticMeshHandle handle =
			cluster.staticMeshes.getHandle(meshIdx);
		if (!handle.valid())
			continue;
		const StringViewT uri = cluster.GetMeshUri(handle);
		if (uri.empty())
			continue;
		if (BaseMeshUriForLod(uri) != uri)
			continue; // a sibling itself: chains grow from base meshes only
		const StaticMeshUpload* upload = cluster.GetRetainedUpload(handle);
		if (!upload || upload->vertices.empty() || upload->indices.empty())
			continue;
		// Idempotence: already-cooked chains stay untouched.
		if (auto hit = cluster.GetMeshLod(LodSiblingUri(uri, 1));
			hit && hit->lodIndex == 1)
			continue;
		// LOD0 marker (serializer fallback semantics when unregistered).
		LodInfo lod0;
		if (auto hit = cluster.GetMeshLod(uri))
			lod0 = *hit;
		else {
			lod0.groupId = handle.index();
			lod0.triCount = uint32_t(upload->indices.size() / 3);
		}
		// Lightmap provenance: stamped chart uvs + per-vertex chart ids +
		// the lobe count and chart transforms the sibling shares (it
		// samples LOD0's atlas rects through the same chart list).
		const std::vector<uint32_t>* charts = nullptr;
		uint32_t lod0LmChunk = 0xFFFFFFFFu;
		std::vector<glm::vec4> lod0Xforms;
		if (const StaticMesh* m0 = cluster.staticMeshes.get(handle)) {
			lod0LmChunk = m0->lmChunk;
			lod0Xforms = m0->lmChartXforms;
		}
		if (options.vertexCharts && lod0LmChunk != 0xFFFFFFFFu) {
			const auto it = options.vertexCharts->find(meshIdx);
			if (it != options.vertexCharts->end()
				&& it->second.size() == upload->vertices.size())
				charts = &it->second;
		}
		// Coverage-preserving simplify for alpha cutout (mirror the
		// importer's material gate; Blend/MASK meshes are never
		// lightmapped, so the two paths never mix).
		bool preserveCoverage = false;
		{
			const auto matIt = scene.materialForNode.find(node);
			if (matIt != scene.materialForNode.end()) {
				const ClusterMaterialHandle mh =
					cluster.materials.getHandle(matIt->second);
				if (mh.valid()) {
					if (const Material* mat = cluster.materials.get(mh))
						preserveCoverage = (mat->alphaMode == AlphaMode::Mask
							|| mat->alphaMode == AlphaMode::Blend);
				}
			}
		}
		std::vector<CookedLod> chain;
		if (!BuildLodChainForUpload(*upload, config, chain, preserveCoverage,
				charts ? charts->data() : nullptr))
			continue;

		// Lightmap-safety audit per level (log-only unless gate on). A
		// failing level fails the chain: coarser levels only interpolate
		// more, so the gate truncates from the first failure.
		size_t keptLevels = chain.size();
		if (charts && options.audit && options.atlasWidth && options.atlasHeight) {
			const glm::mat4 world = node < scene.globalTransform.size()
				? scene.globalTransform[node] : glm::mat4(1.0f);
			const float worldScale = MaxScaleOfMatrix(world);
			const float diag = worldScale
				* glm::length(upload->aabbMax - upload->aabbMin);
			for (size_t li = 0; li < chain.size(); ++li) {
				// Ray reach: 4x the level's world error, with extent and
				// absolute floors (the closest-point fallback is exact
				// regardless, so this only widens the cheap path).
				const float worldErr = chain[li].localError * worldScale;
				const float searchDist = std::max(
					{ worldErr * 4.0f, diag * 0.01f, 1e-4f });
			LodLightmapAuditReport rep;
			const bool ran = AuditLodLightmap(*upload, chain[li].upload,
				world, lod0Xforms, options.atlasWidth, options.atlasHeight,
				options.auditThresholdTexels, searchDist,
				options.auditMaxTris, rep);
				const bool fail = ran && (rep.crossChartTris > 0
					|| (rep.samples > 0
						&& rep.overThreshold * 20u > rep.samples)); // >5%
				std::cout << "  [LM-LOD] " << uri.Str() << "/lod" << (li + 1)
					<< ": audit tris " << rep.trisAudited << "/" << rep.trisTotal
					<< " samples " << rep.samples
					<< " unresolved " << rep.unresolved
					<< " texels max " << rep.maxTexels
					<< " mean " << rep.meanTexels
					<< " p95 " << rep.p95Texels
					<< " over " << options.auditThresholdTexels << "tx "
					<< rep.overThreshold
					<< " cross-chart " << rep.crossChartTris
					<< (fail ? " FAIL" : "") << "\n" << std::flush;
				if (fail && options.auditGate) {
					keptLevels = li;
					break;
				}
			}
			if (keptLevels < chain.size())
				std::cout << "  [LM-LOD] " << uri.Str()
					<< ": gate dropped lod" << (keptLevels + 1) << "+\n"
					<< std::flush;
			chain.resize(keptLevels);
		}

		// Upload + register siblings (mirrors the importer's sibling
		// registration so World::RebuildLodChains resolves them the same
		// way); each sibling gets its own meshlets so the GPU filter can
		// register every level.
		uint32_t registered = 0;
		for (size_t li = 0; li < chain.size(); ++li) {
			const CookedLod& kept = chain[li];
			const auto lh = cluster.UploadStaticMesh(kept.upload);
			if (!lh.valid())
				continue;
			const StringViewT lodUri = LodSiblingUri(uri, uint32_t(li + 1));
			if (meshletConfig.enabled && !kept.upload.vertices.empty()
				&& !kept.upload.indices.empty()) {
				CookedMeshlets lodCooked;
				MeshletStats lodStats;
				std::string lodErr;
				if (BuildMeshletsForUpload(kept.upload, meshletConfig,
						&lodCooked, &lodStats, &lodErr)
					&& !lodCooked.empty())
					cluster.RegisterMeshMeshlets(lodUri, lodCooked);
			}
			cluster.RegisterMeshUri(lodUri, lh);
			LodInfo llod;
			llod.groupId = lod0.groupId;
			llod.lodIndex = uint32_t(li + 1);
			llod.candidateCount = uint32_t(1 + chain.size());
			llod.simplifyError = kept.localError; // scaled to world below
			llod.screenThreshold = -1.0f;
			llod.triCount = kept.triCount;
			llod.acmr = 0.0f;
			cluster.RegisterMeshLod(lodUri, llod);
		// Lightmap chunk (lobe count) + chart transforms: the sibling
		// shares LOD0's atlas rects, so it carries the same count on
		// its draws and the same transform list for the shader unpack
		// (simplified vertices are a remapped subset — chart ids stay
		// valid indices into LOD0's chart list).
		if (charts) {
			if (StaticMesh* m = cluster.staticMeshes.get(lh))
				m->lmChunk = lod0LmChunk;
		}
		if (!lod0Xforms.empty()) {
			if (StaticMesh* m = cluster.staticMeshes.get(lh))
				m->lmChartXforms = lod0Xforms;
		}
		++registered;
		}
		lod0.candidateCount = uint32_t(1 + registered);
		cluster.RegisterMeshLod(uri, lod0);
		std::cout << "LOD " << uri.Str() << ": " << registered
			<< " sibling(es)" << (charts ? " (lightmapped)" : "") << "\n";
	}
	FinalizeLodWorldErrors(cluster);
	return true;
}

} // namespace smvk
