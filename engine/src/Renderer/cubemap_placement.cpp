#include <Engine/Renderer/cubemap_placement.h>

#include <algorithm>
#include <cstdio>
#include <deque>

namespace smvk {
namespace {

struct Cell {
	glm::vec3 mn{ 0.0f };
	glm::vec3 mx{ 0.0f };
	uint32_t depth = 0;
};

glm::vec3 CellCenter(const Cell& c) { return (c.mn + c.mx) * 0.5f; }
glm::vec3 CellHalf(const Cell& c) { return (c.mx - c.mn) * 0.5f; }
float CellSize(const Cell& c) {
	const glm::vec3 e = c.mx - c.mn;
	return std::max(e.x, std::max(e.y, e.z));
}

const glm::vec3 kAxes[6] = {
	{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
	{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};

// 14 openness directions: 6 axial + 8 box diagonals.
void OpennessDirs(glm::vec3 out[14]) {
	for (int i = 0; i < 6; ++i)
		out[i] = kAxes[i];
	int k = 6;
	for (int sx = -1; sx <= 1; sx += 2)
		for (int sy = -1; sy <= 1; sy += 2)
			for (int sz = -1; sz <= 1; sz += 2)
				out[k++] = glm::normalize(
					glm::vec3(float(sx), float(sy), float(sz)));
}

// Fraction of openness rays escaping to openDist (1 = open sky all
// around, 0 = buried). Null predicate scores 0 (keeps the center).
float Openness(const glm::vec3& p, float openDist,
	const CubemapOcclusionRay& occluded) {
	if (!occluded || openDist <= 0.0f)
		return 0.0f;
	glm::vec3 dirs[14];
	OpennessDirs(dirs);
	uint32_t free = 0;
	for (const glm::vec3& d : dirs) {
		if (!occluded(p, d, openDist))
			++free;
	}
	return float(free) / 14.0f;
}

// True when the point is buried in solid: every axial ray blocked within
// embedDist. Corridor/air points always survive (an axis escapes); only
// clearly-embedded centers are dropped.
bool IsEmbedded(const glm::vec3& p, float embedDist,
	const CubemapOcclusionRay& occluded) {
	if (!occluded || embedDist <= 0.0f)
		return false;
	for (const glm::vec3& a : kAxes) {
		if (!occluded(p, a, embedDist))
			return false;
	}
	return true;
}

} // namespace

size_t BuildOctreeProbePositions(const glm::vec3& bmin, const glm::vec3& bmax,
	const CubemapOctreeOptions& opts, const CubemapOcclusionRay& occluded,
	std::vector<CubemapPlacedProbe>& out, std::string& log) {
	out.clear();
	log.clear();
	char buf[256];

	const uint32_t budget = std::max(1u, opts.maxProbes);
	const float minCell = std::max(0.05f, opts.minCellSize);
	const float maxCell = std::max(minCell, opts.maxCellSize);

	// Degenerate bounds (empty scene, flat import): one centered fallback
	// probe so the capture still runs instead of baking nothing.
	if (!(bmax.x > bmin.x && bmax.y > bmin.y && bmax.z > bmin.z)) {
		CubemapPlacedProbe p{};
		p.position = (bmin + bmax) * 0.5f;
		out.push_back(p);
		std::snprintf(buf, sizeof(buf),
			"[octree] degenerate bounds: single fallback probe\n");
		log += buf;
		return out.size();
	}

	size_t droppedEmbedded = 0;
	size_t subdivided = 0;
	size_t depthHist[16] = {};

	// Breadth-first growth: coarse coverage decides before detail, so
	// budget pressure truncates detail, never coverage. emitted[] stays in
	// emission (coarse-first) order.
	std::vector<Cell> emitted;
	emitted.reserve(budget + 8);
	std::deque<Cell> queue;
	queue.push_back(Cell{ bmin, bmax, 0 });

	while (!queue.empty()) {
		Cell c = queue.front();
		queue.pop_front();
		const glm::vec3 center = CellCenter(c);
		const glm::vec3 half = CellHalf(c);
		const float halfDiag = glm::length(half);
		const float size = CellSize(c);

		const bool atFloor = size <= minCell || c.depth >= opts.maxDepth;
		// Surface proximity: any center ray hitting within reach means the
		// cell touches or neighbors geometry (detail candidate).
		uint32_t surfHits = 0;
		if (occluded) {
			const float reach = halfDiag * 1.25f + 1e-4f;
			for (const glm::vec3& a : kAxes) {
				if (occluded(center, a, reach))
					++surfHits;
			}
		}
		const bool nearSurface = surfHits > 0;
		const bool tooCoarse = !nearSurface && size > maxCell;
		const bool wantSplit = !atFloor && (nearSurface || tooCoarse);

		// Splitting one decided leaf into 8 children nets +7 leaves:
		// emitted[] + queue[] (each queued cell yields >= 1 leaf) + 8 new
		// must fit the budget, otherwise emit coarse (coverage over detail).
		// This conservatism is load-bearing: emitted[] is a COMPLETE
		// partition of the root (disjoint cells, union = bounds), and the
		// emission below keeps every cell — truncating the tail would punch
		// coverage holes (dropped cells have no ancestor left to cover
		// them). A few budget slots may go unused; holes never happen.
		const size_t liveLeaves = emitted.size() + queue.size();
		if (wantSplit && liveLeaves + 8 <= budget) {
			++subdivided;
			for (int iz = 0; iz < 2; ++iz)
				for (int iy = 0; iy < 2; ++iy)
					for (int ix = 0; ix < 2; ++ix) {
						Cell ch{};
						ch.mn = { ix ? center.x : c.mn.x,
							iy ? center.y : c.mn.y,
							iz ? center.z : c.mn.z };
						ch.mx = { ix ? c.mx.x : center.x,
							iy ? c.mx.y : center.y,
							iz ? c.mx.z : center.z };
						ch.depth = c.depth + 1;
						queue.push_back(ch);
					}
		} else {
			emitted.push_back(c);
		}
	}

	// Emit coarse-first, dropping embedded centers. Each leaf emits at
	// its most open interior point (center + 6 face centers by openness):
	// cell coverage only tells you the probe OWNS a volume, the point
	// decides what it SEES. Radii scale with the emitting cell so coarse
	// air cells cover volumes and fine surface cells stay tight.
	const float openDist = glm::length(bmax - bmin);
	size_t relocated = 0;
	for (const Cell& c : emitted) {
		const glm::vec3 ctr = CellCenter(c);
		glm::vec3 best = ctr;
		if (occluded) {
			float bestScore = Openness(ctr, openDist, occluded);
			glm::vec3 cand[6] = {
				{ c.mn.x, ctr.y, ctr.z }, { c.mx.x, ctr.y, ctr.z },
				{ ctr.x, c.mn.y, ctr.z }, { ctr.x, c.mx.y, ctr.z },
				{ ctr.x, ctr.y, c.mn.z }, { ctr.x, ctr.y, c.mx.z },
			};
			for (const glm::vec3& q : cand) {
				const float s = Openness(q, openDist, occluded);
				if (s > bestScore) {
					bestScore = s;
					best = q;
				}
			}
			if (glm::dot(best - ctr, best - ctr) > 1e-6f)
				++relocated;
		}
		if (IsEmbedded(best, opts.embedDist, occluded)) {
			++droppedEmbedded;
			continue;
		}
		const glm::vec3 p = best;
		const float halfDiag = glm::length(CellHalf(c));
		CubemapPlacedProbe pr{};
		pr.position = p;
		pr.influenceRadius = std::max(halfDiag, 0.5f);
		pr.fadeRadius = std::max(halfDiag * 0.5f, 0.25f);
		pr.depth = c.depth;
		out.push_back(pr);
		if (c.depth < 16)
			++depthHist[c.depth];
		if (out.size() >= budget)
			break;
	}

	std::snprintf(buf, sizeof(buf),
		"[octree] cells subdivided=%zu emitted=%zu kept=%zu droppedEmbedded=%zu relocated=%zu (maxCell=%.2g minCell=%.2g depth<=%u budget=%u)\n",
		subdivided, emitted.size(), out.size(), droppedEmbedded, relocated,
		(double)maxCell, (double)minCell, opts.maxDepth, budget);
	log += buf;
	for (uint32_t d = 0; d < 16; ++d) {
		if (!depthHist[d])
			continue;
		std::snprintf(buf, sizeof(buf), "[octree]   depth %u: %zu probes\n",
			d, depthHist[d]);
		log += buf;
	}
	return out.size();
}

} // namespace smvk
