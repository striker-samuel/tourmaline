// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/meshlet.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <meshoptimizer.h>

#include <Engine/Renderer/mesh.h>

namespace smvk {

size_t CookedMeshlets::byteSize() const
{
	return meshlets.size() * sizeof(Meshlet) +
		vertices.size() * sizeof(uint32_t) +
		triangles.size();
}

namespace {

// Degenerate-cone split depth: 6 (the knight plateau: 44 -> 10 zero-axis
// cones at 1999 -> 2000 meshlets; depth 5 leaves 12, depth 7 is identical).
// Each split produces subsets of the parent, so the 64v/64t caps hold.
inline constexpr unsigned int kSplitDepth = 6;

// Degenerate-cone repair for meshlets whose normal cones cancel (the
// double-layer-cloth fix, extended to round patches). meshopt's growth mixes
// opposing skins into ONE meshlet two ways: getNeighborTriangle's
// vertex-budget priority always prefers an extra==0 triangle (an
// opposite-skin triangle through a shared rim vertex) over a same-skin
// triangle that would add vertices - the cone score only breaks ties WITHIN
// a priority class, so cone_weight cannot stop the crossing - and the kd-tree
// fallback (adjacency ran dry mid-meshlet) takes the spatially CLOSEST
// triangle irrespective of facing, which on double-layer cloth (front + back
// skins ~1mm apart) is the other skin. The skins' normals then cancel,
// meshopt_computeMeshletBounds degenerates the cone (axis 0, trivial accept),
// and the GPU backface/cone cull goes dead for the whole region (observed on
// the knight: 1867/1913 meshlets carried zero-axis cones; the radii were fine
// - the meshlets were spatially tight but facing-mixed; cone_weight 0 / 0.5 /
// 1.0 all left ~97% degenerate, and an edge-component pre-split either
// shattered corrugated cloth into micro-components (cut at 90deg) or kept the
// layer welds alive (cut at 134deg)).
// The repair is a bounded POST-split with two partition modes:
//   - an opposing face-normal pair exists -> cut by the sign of
//     dot(faceNormal, endpointNormal) (separates the two skins);
//   - no opposing pair (the cone wraps because the patch itself is round: a
//     ring, a crumpled fold) -> cut by POSITION at the centroid median along
//     the widest covariance axis (a ring becomes arcs, an arc smaller arcs).
// Children re-bound on their own compacted windows; both stay under the
// 64v/64t engine caps (they are subsets). A child that still degenerates at
// the depth cap (a genuinely wrap-around or zero-normal patch) keeps its
// conservative keep-cone - degenerate cones never cull wrong, they just fail
// to cull.

// Radius containment repair (conservative direction): the HZB occlusion +
// cone tests treat (boundCenter, boundRadius) as CONTAINING all meshlet
// triangles. A non-containing sphere yields wrong footprints -> false HZB
// culls of visible meshlets (observed once in the wild: sponza prim.49
// meshlet 136, sliver triangle outside radius by ~2x). Expands the radius
// on violation. `meshletVerts` is a LOCAL window (local ids into it), not
// the global vertex window the meshopt call takes.
void RepairBoundRadius(meshopt_Bounds& b,
	const unsigned int* meshletVerts, const unsigned char* meshletTris,
	unsigned int triCount, const StaticMeshUpload& upload)
{
	const size_t vcount = upload.vertices.size();
	float maxD2 = 0.0f;
	for (unsigned int t = 0; t < triCount; ++t) {
		for (unsigned int k = 0; k < 3; ++k) {
			const unsigned int local = meshletTris[t * 3 + k];
			if (local >= kMeshletMaxVertices)
				continue;
			const unsigned int vid = meshletVerts[local];
			if ((size_t)vid >= vcount)
				continue;
			const glm::vec3& p = upload.vertices[vid].pos;
			const float dx = p.x - b.center[0];
			const float dy = p.y - b.center[1];
			const float dz = p.z - b.center[2];
			maxD2 = std::max(maxD2, dx * dx + dy * dy + dz * dz);
		}
	}
	const float need = std::sqrt(maxD2);
	if (need > b.radius + 1e-4f * std::max(1.0f, need)) {
		std::fprintf(stderr,
			"[Meshlets] repaired non-containing bound (radius %.3f -> %.3f).\n",
			b.radius, need);
		b.radius = need;
	}
}

} // anonymous namespace

bool BuildMeshletsForUpload(
	const StaticMeshUpload& upload,
	const MeshletConfig& config,
	CookedMeshlets* out,
	MeshletStats* outStats,
	std::string* err)
{
	auto fail = [&](const std::string& msg) {
		if (out) {
			out->meshlets.clear();
			out->vertices.clear();
			out->triangles.clear();
		}
		if (err)
			*err = msg;
		return false;
	};

	if (!out)
		return fail("null output");
	out->meshlets.clear();
	out->vertices.clear();
	out->triangles.clear();
	if (outStats)
		*outStats = MeshletStats{};

	if (!config.enabled)
		return fail("meshlets disabled");
	const size_t vcount = upload.vertices.size();
	const size_t icount = upload.indices.size();
	if (vcount == 0 || icount < 3 || (icount % 3) != 0)
		return fail("degenerate mesh");

	// ENGINE limits first (kMeshletMaxVertices/Triangles â€” 64v/64t: one
	// mesh-shader workgroup covers a whole meshlet, the invariant the mesh
	// path and the cluster HZB cull assume): over-limit configs (stale
	// authored cooks carrying e.g. meshletTris 124) clamp down â€” a meshlet
	// bigger than the workgroup can draw would drop geometry. The absolute
	// meshoptimizer bounds (3..255v / 1..512t) stay as the outer defense,
	// unreachable for engine callers since the engine clamp is tighter.
	// One loud line the first time a config overruns (stale authored data
	// should be visible, not silently reinterpreted).
	static bool sNotedEngineClamp = false;
	if ((config.maxVertices > kMeshletMaxVertices ||
			config.maxTriangles > kMeshletMaxTriangles) && !sNotedEngineClamp) {
		sNotedEngineClamp = true;
		std::fprintf(stderr,
			"[Meshlets] config over the engine limit (mesh-shader workgroup "
			"covers <= %uu/%ut; got %uu/%ut) - clamped to the engine limit "
			"(update the cook settings: the 124-tris authored default is stale)\n",
			kMeshletMaxVertices, kMeshletMaxTriangles,
			config.maxVertices, config.maxTriangles);
	}
	uint32_t maxVerts = std::clamp(config.maxVertices, 3u, kMeshletMaxVertices);
	uint32_t maxTris = std::clamp(config.maxTriangles, 1u, kMeshletMaxTriangles);

	const float* positions = vcount > 0 ? &upload.vertices[0].pos.x : nullptr;

	const size_t bound = meshopt_buildMeshletsBound(icount, maxVerts, maxTris);
	if (bound == 0)
		return fail("meshopt_buildMeshletsBound returned 0");

	std::vector<meshopt_Meshlet> raw(bound);
	std::vector<unsigned int> rawVerts(bound * maxVerts);
	std::vector<unsigned char> rawTris(bound * maxTris * 3);

	const size_t count = meshopt_buildMeshlets(
		raw.data(), rawVerts.data(), rawTris.data(),
		upload.indices.data(), icount,
		positions, vcount, sizeof(StaticVertex),
		maxVerts, maxTris, config.coneWeight);
	if (count == 0)
		return fail("meshopt_buildMeshlets returned 0");

	size_t vertCursor = 0;
	size_t triCursor = 0;
	// First pass: optimize + measure so we can reserve exact storage.
	// (Upper bound for the split path: the halves reuse the parent
	// windows' storage.)
	for (size_t i = 0; i < count; ++i) {
		meshopt_Meshlet& m = raw[i];
		meshopt_optimizeMeshlet(
			&rawVerts[m.vertex_offset], &rawTris[m.triangle_offset],
			m.triangle_count, m.vertex_count);
		vertCursor += m.vertex_count;
		triCursor += (size_t)m.triangle_count * 3;
	}
	CookedMeshlets cooked;
	cooked.meshlets.reserve(count);
	cooked.vertices.reserve(vertCursor);
	cooked.triangles.reserve(triCursor);

	MeshletStats stats;
	// Shape-quality accumulation (the cook's dashboard print): see
	// MeshletStats for the field semantics.
	double radiusSum = 0.0;
	double cutoffSum = 0.0;
	uint32_t degenerateCones = 0;
	uint32_t splitMeshlets = 0;
	// TMP-DIAG (TOURMALINE_MESHLET_DUMP=1): per-meshlet shape dump gate
	// (R + cone cutoff + the DEGENERATE/SPLIT tags). Remove later.
	static const bool sDump = [] {
		const char* e = std::getenv("TOURMALINE_MESHLET_DUMP");
		return e && *e && *e != '0';
	}();
	size_t dumpLeft = 32;

	// A facing-pure split fragment awaiting the re-pack (the merge pass
	// below): the split emits fragments instead of finished meshlets so
	// same-facing neighbors can fill ONE meshlet later.
	struct Fragment {
		unsigned int verts[kMeshletMaxVertices];
		unsigned int vertCount = 0;
		unsigned char tris[kMeshletMaxTriangles * 3];
		unsigned int triCount = 0; // triangles
		glm::vec3 boundCenter{};
		glm::vec3 coneAxis{ 0.0f, 0.0f, 0.0f };
		uint64_t morton = 0;
	};
	std::vector<Fragment> fragments;

	// Emits one finished Meshlet (its window is already pushed into
	// cooked) and accumulates the shape stats + the TMP-DIAG dump line.
	auto emitMeshlet = [&](Meshlet& ml, bool split) {
		radiusSum += (double)ml.boundRadius;
		cutoffSum += (double)ml.coneCutoff;
		++stats.meshletCount;
		stats.totalTriangles += ml.triangleCount;
		stats.maxVertsUsed = std::max(stats.maxVertsUsed, ml.vertexCount);
		stats.maxTrisUsed = std::max(stats.maxTrisUsed, ml.triangleCount);
		if (glm::dot(ml.coneAxis, ml.coneAxis) < 1e-12f)
			++degenerateCones;
		if (sDump && dumpLeft > 0) {
			--dumpLeft;
			std::fprintf(stderr,
				"[ML-DUMP] meshlet %u: tris=%u R=%.3f cutoff=%.3f%s%s\n",
				(unsigned)cooked.meshlets.size(), ml.triangleCount,
				ml.boundRadius, ml.coneCutoff,
				glm::dot(ml.coneAxis, ml.coneAxis) < 1e-12f
					? " DEGENERATE" : "",
				split ? " (SPLIT)" : "");
		}
		cooked.meshlets.push_back(ml);
	};

	// Recursive emit: bounds + radius repair, then either emit as-is
	// (depth 0: meshopt's own full meshlets - untouched) / collect as a
	// fragment (depth > 0: the split's children, facing-pure but small - the
	// re-pack below merges them back into full meshlets) / (degenerate cone,
	// a usable partition axis, depth budget left) cut and recurse. Children
	// are subsets so the 64v/64t caps hold at every level; the two partition
	// modes are described in the block comment at kSplitDepth.
	std::function<void(const unsigned int*, unsigned int,
		const unsigned char*, unsigned int, unsigned int, bool)>
		emitWindow = [&](const unsigned int* winVerts, unsigned int winVertCount,
			const unsigned char* winTris, unsigned int triCount,
			unsigned int depth, bool fromSplit)
	{
		meshopt_Bounds b = meshopt_computeMeshletBounds(
			winVerts, winTris, triCount, positions, vcount,
			sizeof(StaticVertex));
		RepairBoundRadius(b, winVerts, winTris, triCount, upload);

		const bool degenerateCone =
			(b.cone_axis[0] * b.cone_axis[0] + b.cone_axis[1] * b.cone_axis[1]
				+ b.cone_axis[2] * b.cone_axis[2]) < 1e-12f;

		// Degenerate-cone split (see the block comment): a degenerate cone
		// with a usable partition axis and depth budget splits; everything
		// else - including children that STILL wrap at the cap (genuinely
		// round or zero-normal patches) - keeps its conservative keep-cone
		// (degenerate cones never cull wrong, they just fail to cull).
		// Face normals first; a zero-length normal (slivers) dots to 0 with
		// everything so slivers never make the worst pair (they ride side 0).
		// The normal partition axis is one endpoint of the WORST pairwise
		// normal pair - the most-antiparallel faces - so the cut separates the
		// actual pathology (the two skins / the fold crest); a first-face axis
		// can lie IN the spread plane and split nothing. With no opposing pair
		// the patch is round, so the position axis takes over (see below).
		glm::vec3 fn[kMeshletMaxTriangles] = {};
		glm::vec3 centers[kMeshletMaxTriangles] = {};
		glm::vec3 splitAxis(0.0f);
		float splitThreshold = 0.0f;
		bool positionSplit = false;
		bool canSplit = false;
		if (degenerateCone && triCount >= 2 && depth < kSplitDepth) {
			for (unsigned int t = 0; t < triCount; ++t) {
				glm::vec3 pv[3];
				for (unsigned int k = 0; k < 3; ++k) {
					const unsigned int local = winTris[t * 3 + k];
					pv[k] = local < winVertCount
						? upload.vertices[winVerts[local]].pos
						: glm::vec3(0.0f);
				}
				centers[t] = (pv[0] + pv[1] + pv[2]) / 3.0f;
				fn[t] = glm::cross(pv[1] - pv[0], pv[2] - pv[0]);
			}
			float worstDot = 0.0f; // need a strictly-opposing pair at all
			unsigned int worstI = 0;
			for (unsigned int t = 0; t < triCount; ++t) {
				if (glm::dot(fn[t], fn[t]) < 1e-12f)
					continue;
				for (unsigned int u = t + 1; u < triCount; ++u) {
					if (glm::dot(fn[u], fn[u]) < 1e-12f)
						continue;
					const float d = glm::dot(fn[t], fn[u]);
					if (d < worstDot) {
						worstDot = d;
						worstI = t;
						canSplit = true;
					}
				}
			}
			if (canSplit) {
				splitAxis = glm::normalize(fn[worstI]);
			} else {
				// No opposing pair: the cone wraps because the patch
				// itself is round (a chainmail ring, a crumpled fold).
				// Facing planes cannot resolve it; cut the patch by
				// POSITION along its widest spatial axis (the largest
				// covariance eigenvector) at the centroid median. A ring
				// becomes arcs, an arc becomes smaller arcs, and each
				// piece's normals span less of the circle.
				glm::vec3 mean(0.0f);
				for (unsigned int t = 0; t < triCount; ++t)
					mean += centers[t];
				mean /= (float)triCount;
				glm::vec3 covX(0.0f), covY(0.0f), covZ(0.0f);
				for (unsigned int t = 0; t < triCount; ++t) {
					const glm::vec3 d = centers[t] - mean;
					covX += d * d.x;
					covY += d * d.y;
					covZ += d * d.z;
				}
				glm::vec3 axis(0.0f);
				float bestRayleigh = -1.0f;
				const glm::vec3 seeds[3] = {
					glm::vec3(1.0f, 0.0f, 0.0f),
					glm::vec3(0.0f, 1.0f, 0.0f),
					glm::vec3(0.0f, 0.0f, 1.0f)
				};
				for (const glm::vec3& seed : seeds) {
					glm::vec3 v = seed;
					for (int it = 0; it < 12; ++it) {
						const glm::vec3 next =
							covX * v.x + covY * v.y + covZ * v.z;
						const float len2 = glm::dot(next, next);
						if (len2 < 1e-18f) {
							v = glm::vec3(0.0f);
							break;
						}
						v = next / std::sqrt(len2);
					}
					if (glm::dot(v, v) < 1e-12f)
						continue;
					// Rayleigh quotient picks the true dominant seed:
					// a fixed seed can be orthogonal to the widest axis
					// (symmetric point sets), which stalls power iteration.
					const glm::vec3 covV =
						covX * v.x + covY * v.y + covZ * v.z;
					const float rayleigh = glm::dot(v, covV);
					if (rayleigh > bestRayleigh) {
						bestRayleigh = rayleigh;
						axis = v;
					}
				}
				if (glm::dot(axis, axis) > 1e-12f) {
					float proj[kMeshletMaxTriangles];
					for (unsigned int t = 0; t < triCount; ++t)
						proj[t] = glm::dot(centers[t], axis);
					float sorted[kMeshletMaxTriangles];
					std::memcpy(sorted, proj, sizeof(float) * triCount);
					std::sort(sorted, sorted + triCount);
					splitThreshold = sorted[triCount / 2];
					unsigned int below = 0;
					for (unsigned int t = 0; t < triCount; ++t)
						if (proj[t] <= splitThreshold)
							++below;
					if (below > 0 && below < triCount) {
						splitAxis = axis;
						positionSplit = true;
						canSplit = true;
					}
				}
			}
		}

		if (!canSplit) {
			if (fromSplit && depth > 0) {
				// A split child: facing-pure (or a leftover) but small â€”
				// collect for the re-pack instead of emitting a speck.
				Fragment f{};
				std::memcpy(f.verts, winVerts,
					sizeof(unsigned int) * winVertCount);
				f.vertCount = winVertCount;
				std::memcpy(f.tris, winTris, (size_t)triCount * 3);
				f.triCount = triCount;
				f.boundCenter = glm::vec3(b.center[0], b.center[1], b.center[2]);
				f.coneAxis = glm::vec3(
					b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
				fragments.push_back(std::move(f));
				return;
			}
			// Emit as built (meshopt's own full meshlet, not poisoned /
			// no partition axis / out of depth at the top level).
			for (unsigned int k = 0; k < winVertCount; ++k)
				cooked.vertices.push_back(winVerts[k]);
			for (unsigned int k = 0; k < triCount * 3; ++k)
				cooked.triangles.push_back(winTris[k]);
			Meshlet ml;
			ml.vertexOffset = (uint32_t)cooked.vertices.size() - winVertCount;
			ml.vertexCount = winVertCount;
			ml.triangleOffset = (uint32_t)cooked.triangles.size()
				- (size_t)triCount * 3;
			ml.triangleCount = triCount;
			ml.boundCenter = glm::vec3(b.center[0], b.center[1], b.center[2]);
			ml.boundRadius = b.radius;
			ml.coneApex = glm::vec3(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);
			ml.coneAxis = glm::vec3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
			ml.coneCutoff = b.cone_cutoff;
			emitMeshlet(ml, false);
			return;
		}

		// The split: normal mode partitions by the sign of
		// dot(faceNormal, splitAxis) - the two skins of a mix land on
		// opposite sides. Position mode splits at the centroid median along
		// the widest spatial axis. Both sides compact to fresh local
		// windows; an empty side is skipped.
		++splitMeshlets;
		for (int side = 0; side < 2; ++side)
		{
			// Compact the side's faces into a fresh local window.
			unsigned int halfVerts[kMeshletMaxVertices];
			unsigned int halfVertCount = 0;
			unsigned char remap[kMeshletMaxVertices];
			std::memset(remap, 0xFF, sizeof(remap));
			unsigned char halfTris[kMeshletMaxTriangles * 3];
			unsigned int halfTriBytes = 0;
			for (unsigned int t = 0; t < triCount; ++t) {
				const bool thisSide = positionSplit
					? ((glm::dot(centers[t], splitAxis) <= splitThreshold)
						== (side == 0))
					: ((glm::dot(fn[t], splitAxis) >= 0.0f)
						== (side == 0));
				if (!thisSide)
					continue;
				// Corrupt locals (local >= vertexCount) drop the whole
				// triangle â€” a partial window push would shift every
				// later byte (the pack audit owns the loud report).
				bool triOk = true;
				for (unsigned int k = 0; k < 3; ++k)
					if (winTris[t * 3 + k] >= winVertCount) {
						triOk = false;
						break;
					}
				if (!triOk)
					continue;
				for (unsigned int k = 0; k < 3; ++k) {
					const unsigned int local = winTris[t * 3 + k];
					if (remap[local] == 0xFF) {
						remap[local] = (unsigned char)halfVertCount;
						halfVerts[halfVertCount++] = winVerts[local];
					}
					halfTris[halfTriBytes++] = remap[local];
				}
			}
			if (halfTriBytes == 0)
				continue; // empty side
			emitWindow(halfVerts, halfVertCount, halfTris,
				halfTriBytes / 3, depth + 1, true);
		}
	};

	for (size_t i = 0; i < count; ++i)
	{
		const meshopt_Meshlet& m = raw[i];
		emitWindow(&rawVerts[m.vertex_offset], m.vertex_count,
			&rawTris[m.triangle_offset], m.triangle_count, 0, false);
	}

	// ---- Facing-fragment re-pack (the "speckle" fix) -------------------
	// The split leaves facing-pure but TINY fragments (the crumpled cloth
	// quarters nearly everything â€” observed knight mesh.0: 4.2x the
	// meshlets at ~23 tris each; the debug view reads that as speckle,
	// and the per-cluster cull/indirect overhead scales with the count).
	// The re-pack merges same-facing spatial neighbors back into FULL
	// meshlets: fragments sorted (facing octant, Morton center) so
	// neighbors stream adjacent, a small open-accumulator set greedily
	// absorbs â€” the 64v/64t caps hold, and the MERGED CONE MUST STAY
	// LIVE (checked per absorb, rollback on rejection), so the merge
	// never builds what the cone test cannot use â€” the split's culling
	// wins survive the packing. Opposing facings sit in different octants
	// and never meet. Unmergeable leftovers emit solo (their conservative
	// keep-cone just fails to cull, same as before).
	if (!fragments.empty())
	{
		auto octantOf = [](const glm::vec3& a) -> unsigned {
			return (a.x >= 0.0f ? 1u : 0u) | (a.y >= 0.0f ? 2u : 0u)
				| (a.z >= 0.0f ? 4u : 0u);
		};
		glm::vec3 fmn(1e30f), fmx(-1e30f);
		for (const Fragment& f : fragments) {
			fmn = glm::min(fmn, f.boundCenter);
			fmx = glm::max(fmx, f.boundCenter);
		}
		const float fscale = 65535.0f
			/ glm::max(glm::max(fmx.x - fmn.x, fmx.y - fmn.y),
				glm::max(fmx.z - fmn.z, 1e-9f));
		auto morton = [](unsigned x, unsigned y, unsigned z) -> uint64_t {
			uint64_t m = 0;
			for (int i = 0; i < 16; ++i)
				m |= ((uint64_t)((x >> i) & 1) << (3 * i))
					| ((uint64_t)((y >> i) & 1) << (3 * i + 1))
					| ((uint64_t)((z >> i) & 1) << (3 * i + 2));
			return m;
		};
		for (Fragment& f : fragments)
			f.morton = morton(
				(unsigned)glm::clamp((f.boundCenter.x - fmn.x) * fscale,
					0.0f, 65535.0f),
				(unsigned)glm::clamp((f.boundCenter.y - fmn.y) * fscale,
					0.0f, 65535.0f),
				(unsigned)glm::clamp((f.boundCenter.z - fmn.z) * fscale,
					0.0f, 65535.0f));
		std::stable_sort(fragments.begin(), fragments.end(),
			[&](const Fragment& a, const Fragment& b) {
			const unsigned oa = octantOf(a.coneAxis);
			const unsigned ob = octantOf(b.coneAxis);
			if (oa != ob)
				return oa < ob;
			return a.morton < b.morton;
		});

		struct Acc {
			unsigned int verts[kMeshletMaxVertices];
			unsigned int vertCount = 0;
			unsigned char tris[kMeshletMaxTriangles * 3];
			unsigned int triCount = 0;
		};
		// Absorb with rollback: any rejection (caps, dead merged cone)
		// truncates the accumulator back to its saved counts.
		auto tryAbsorb = [&](Acc& a, const Fragment& f) -> bool {
			const unsigned savedVerts = a.vertCount;
			const unsigned savedTris = a.triCount;
			auto reject = [&]() {
				a.vertCount = savedVerts;
				a.triCount = savedTris;
				return false;
			};
			if (a.triCount + f.triCount > kMeshletMaxTriangles)
				return reject();
			unsigned char map[kMeshletMaxVertices];
			for (unsigned i = 0; i < f.vertCount; ++i) {
				unsigned char hit = 0xFF;
				for (unsigned j = 0; j < savedVerts; ++j)
					if (a.verts[j] == f.verts[i]) {
						hit = (unsigned char)j;
						break;
					}
				if (hit == 0xFF) {
					if (a.vertCount >= kMeshletMaxVertices)
						return reject();
					hit = (unsigned char)a.vertCount;
					a.verts[a.vertCount++] = f.verts[i];
				}
				map[i] = hit;
			}
			for (unsigned t = 0; t < f.triCount; ++t)
				for (unsigned k = 0; k < 3; ++k)
					a.tris[(a.triCount + t) * 3 + k] =
						map[f.tris[t * 3 + k]];
			a.triCount += f.triCount;
			meshopt_Bounds mb = meshopt_computeMeshletBounds(
				a.verts, a.tris, a.triCount, positions, vcount,
				sizeof(StaticVertex));
			if ((mb.cone_axis[0] * mb.cone_axis[0]
				+ mb.cone_axis[1] * mb.cone_axis[1]
				+ mb.cone_axis[2] * mb.cone_axis[2]) < 1e-12f)
				return reject();
			return true;
		};
		auto emitAcc = [&](Acc& a) {
			meshopt_optimizeMeshlet(a.verts, a.tris, a.triCount, a.vertCount);
			meshopt_Bounds mb = meshopt_computeMeshletBounds(
				a.verts, a.tris, a.triCount, positions, vcount,
				sizeof(StaticVertex));
			RepairBoundRadius(mb, a.verts, a.tris, a.triCount, upload);
			for (unsigned k = 0; k < a.vertCount; ++k)
				cooked.vertices.push_back(a.verts[k]);
			for (unsigned k = 0; k < a.triCount * 3; ++k)
				cooked.triangles.push_back(a.tris[k]);
			Meshlet ml;
			ml.vertexOffset = (uint32_t)cooked.vertices.size() - a.vertCount;
			ml.vertexCount = a.vertCount;
			ml.triangleOffset = (uint32_t)cooked.triangles.size()
				- (size_t)a.triCount * 3;
			ml.triangleCount = a.triCount;
			ml.boundCenter = glm::vec3(mb.center[0], mb.center[1], mb.center[2]);
			ml.boundRadius = mb.radius;
			ml.coneApex = glm::vec3(mb.cone_apex[0], mb.cone_apex[1], mb.cone_apex[2]);
			ml.coneAxis = glm::vec3(mb.cone_axis[0], mb.cone_axis[1], mb.cone_axis[2]);
			ml.coneCutoff = mb.cone_cutoff;
			emitMeshlet(ml, true);
		};
		constexpr unsigned kMaxOpenAccs = 16;
		std::vector<Acc> open;
		open.reserve(kMaxOpenAccs + 1);
		for (const Fragment& f : fragments) {
			bool absorbed = false;
			for (Acc& a : open)
				if (tryAbsorb(a, f)) {
					absorbed = true;
					break;
				}
			if (absorbed)
				continue;
			if (open.size() >= kMaxOpenAccs) {
				// FIFO: the oldest left the locality window.
				emitAcc(open.front());
				open.erase(open.begin());
			}
			Acc a{};
			std::memcpy(a.verts, f.verts, sizeof(unsigned int) * f.vertCount);
			a.vertCount = f.vertCount;
			std::memcpy(a.tris, f.tris, (size_t)f.triCount * 3);
			a.triCount = f.triCount;
			open.push_back(std::move(a));
		}
		for (Acc& a : open)
			emitAcc(a);
	}

	stats.meanRadius = stats.meshletCount > 0
		? (float)(radiusSum / (double)stats.meshletCount) : 0.0f;
	stats.meanCutoff = stats.meshletCount > 0
		? (float)(cutoffSum / (double)stats.meshletCount) : 0.0f;
	stats.degenerateCones = degenerateCones;
	stats.splitMeshlets = splitMeshlets;
	// maxRadius over the finished set (the repair path can grow radii
	// after the accumulation above, so derive it from the final values).
	for (const Meshlet& m : cooked.meshlets)
		stats.maxRadius = std::max(stats.maxRadius, m.boundRadius);

	*out = std::move(cooked);
	if (outStats)
		*outStats = stats;
	return true;
}

void BuildMeshletIndirectCommands(
	const CookedMeshlets& cooked,
	std::vector<uint32_t>& outIndices,
	std::vector<MeshletDrawCommand>& outCommands)
{
	outIndices.clear();
	outCommands.clear();
	outCommands.reserve(cooked.meshlets.size());
	size_t total = 0;
	for (const Meshlet& m : cooked.meshlets)
		total += (size_t)m.triangleCount * 3;
	outIndices.reserve(total);

	for (uint32_t i = 0; i < (uint32_t)cooked.meshlets.size(); ++i) {
		const Meshlet& m = cooked.meshlets[i];
		MeshletDrawCommand cmd;
		cmd.firstIndex = (uint32_t)outIndices.size();
		cmd.indexCount = m.triangleCount * 3;
		cmd.instanceCount = 1;
		cmd.baseVertex = 0;
		cmd.meshletIndex = i;
		for (uint32_t k = 0; k < m.triangleCount * 3; ++k) {
			const uint8_t local = cooked.triangles[m.triangleOffset + k];
			outIndices.push_back(cooked.vertices[m.vertexOffset + local]);
		}
		outCommands.push_back(cmd);
	}
}

} // namespace smvk
