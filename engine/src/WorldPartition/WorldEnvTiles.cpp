// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include <Engine/Rendergraph/envzones.h>
#include <Engine/WorldPartition/WorldEnvTiles.h>

namespace engine::worldpartition {

EnvGridExtent ComputeEnvGridExtent(const Scene& scene,
	const EnvGridBounds& sceneBounds)
{
	// ---- The seed: the mesh scene bounds (degenerate/no bounds — a flat
	// or mesh-less scene — falls to the 1 km default window around the
	// origin; env volumes below can still grow it out). ----
	glm::vec3 mn = sceneBounds.mn, mx = sceneBounds.mx;
	const bool valid = mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z
		&& (mx.x > mn.x || mx.z > mn.z);
	if (!valid) {
		mn = glm::vec3(-512.0f, -16.0f, -512.0f);
		mx = glm::vec3(512.0f, 16.0f, 512.0f);
	}
	const auto unionPoint = [&](const glm::vec3& p) {
		mn = glm::min(mn, p);
		mx = glm::max(mx, p);
	};

	// ---- The env-volume OBB bounds, unioned in. ----
	// The infinite-axis stand-in: half the max grid edge — the grid then
	// spans the full 16 km on that axis after the margin + dim clamp.
	const float infExtent = 0.5f * float(kEnvTileMaxDim) * kEnvTileSize;
	for (const auto& [node, env] : scene.envForNode) {
		if (node >= scene.globalTransform.size())
			continue;
		uint32_t infMask = 0;
		float blendWeight = 1.0f;
		bool enabled = true;
		if (const auto vit = scene.envVolumeForNode.find(node);
			vit != scene.envVolumeForNode.end()) {
			infMask = vit->second.infMask;
			blendWeight = vit->second.blendWeight;
			enabled = vit->second.enabled;
		}
		if (!enabled || blendWeight <= 0.0f)
			continue; // the bake skips it: no bounds contribution
		const glm::mat4& g = scene.globalTransform[node];
		// Per local axis: the column length is the half extent; INF
		// length (the legacy INF scale) or an inf-mask face on the axis
		// makes it unbounded (the bake's own rule).
		bool axisInf[3] = { false, false, false };
		for (int c = 0; c < 3; ++c) {
			const float len = glm::length(glm::vec3(g[c]));
			axisInf[c] = !std::isfinite(len)
				|| (infMask & (3u << (2 * c))) != 0u;
		}
		// The FINITE axes' corners (translation + rotation applied): the
		// zone's world AABB — an all-infinite zone degenerates to its
		// translation.
		for (int sz = -1; sz <= 1; sz += 2)
			for (int sy = -1; sy <= 1; sy += 2)
				for (int sx = -1; sx <= 1; sx += 2) {
					const glm::vec4 local(
						axisInf[0] ? 0.0f : float(sx),
						axisInf[1] ? 0.0f : float(sy),
						axisInf[2] ? 0.0f : float(sz),
						1.0f);
					unionPoint(glm::vec3(g * local));
				}
		// The UNBOUNDED axes: extend the WORLD XZ grid where the axis
		// projects (a nonzero column component), or — a flat column, the
		// authoring norm is axis-aligned — the world axis c. NEVER the
		// vertical range (see the header).
		const auto unionAxisX = [&](float v) {
			mn.x = std::min(mn.x, v);
			mx.x = std::max(mx.x, v);
		};
		const auto unionAxisZ = [&](float v) {
			mn.z = std::min(mn.z, v);
			mx.z = std::max(mx.z, v);
		};
		for (int c = 0; c < 3; ++c) {
			if (!axisInf[c])
				continue;
			const glm::vec3 col = glm::vec3(g[c]);
			const bool flat = !(glm::length(col) > 0.0f);
			const bool extX = flat ? c == 0 : std::abs(col.x) > 1e-6f;
			const bool extZ = flat ? c == 2 : std::abs(col.z) > 1e-6f;
			if (extX) {
				unionAxisX(-infExtent);
				unionAxisX(infExtent);
			}
			if (extZ) {
				unionAxisZ(-infExtent);
				unionAxisZ(infExtent);
			}
		}
	}

	// ---- Tile-aligned + one tile of margin + the 16 km dim clamp. ----
	const glm::vec2 margin(kEnvTileSize);
	EnvGridExtent out{};
	out.gridMin =
		glm::floor(glm::vec2(mn.x, mn.z) / kEnvTileSize) * kEnvTileSize
			- margin;
	const glm::vec2 gMax =
		glm::ceil(glm::vec2(mx.x, mx.z) / kEnvTileSize) * kEnvTileSize
			+ margin;
	out.dimX = std::min(uint32_t((gMax.x - out.gridMin.x) / kEnvTileSize),
		kEnvTileMaxDim);
	out.dimZ = std::min(uint32_t((gMax.y - out.gridMin.y) / kEnvTileSize),
		kEnvTileMaxDim);
	out.yMin = mn.y;
	out.yMax = mx.y;
	return out;
}

bool WorldEnvTiles::Build(const Scene& scene, glm::vec2 gridMin,
	uint32_t dimX, uint32_t dimZ, float yMin, float yMax)
{
	built_ = false;
	dimX = std::min(dimX, kEnvTileMaxDim);
	dimZ = std::min(dimZ, kEnvTileMaxDim);
	if (dimX == 0 || dimZ == 0)
		return false;
	gridMin_ = gridMin;
	dimX_ = dimX;
	dimZ_ = dimZ;

	// ---- Records: [0] = the base (the default environment block derived
	// against ITSELF — the old zone-zero, always present, weight-1
	// fallback), then the authored node zones derived against the
	// RESOLVED base (scene.environment). ----
	records_.clear();
	records_.resize(kEnvTileMaxRecords);
	{
		const smvk::envzones::EnvZone baseLook = smvk::envzones::DeriveZoneLook(
			scene.defaultEnvironment, scene.defaultEnvironment);
		records_[0].sky = glm::vec4(baseLook.skyTint, baseLook.ambientI);
		records_[0].fog = glm::vec4(baseLook.fogColor, baseLook.fogDensity);
		records_[0].vol = glm::vec4(baseLook.volumetrics,
			baseLook.volumetricDensity, 0.0f, 0.0f);
	}

	// ---- Candidates: the SAME collection/sort the retired per-frame
	// packer used (priority desc -> bounded volume asc -> node id), with
	// the base prepended as the all-infinite fallback. ----
	candidates_.clear();
	{
		// The base candidate: an identity-transform, all-infinite OBB (the
		// whole world covered at weight 1), priority from the default
		// block (usually 0 — node zones win inside their volumes).
		Candidate base{};
		(void)smvk::envzones::MakeEnvZoneGpu(glm::mat4(1.0f), 63u,
			smvk::envzones::EnvZone{}, scene.defaultEnvironment.zonePriority,
			1.0f, 0.0f, base.zone);
		base.record = 0;
		base.boundedVolume = std::numeric_limits<double>::infinity();
		base.nodeId = UINT32_MAX;
		base.priority = scene.defaultEnvironment.zonePriority;
		candidates_.push_back(base);

		struct NodeZone
		{
			Candidate cand;
			float priority = 0.0f;
			double volume = 0.0;
			uint32_t nodeId = 0;
		};
		std::vector<NodeZone> nodeZones;
		nodeZones.reserve(scene.envForNode.size());
		for (const auto& [node, env] : scene.envForNode) {
			if (node >= scene.globalTransform.size())
				continue;
			// EnvVolume shape knobs (absent entry = the legacy defaults).
			uint32_t infMask = 0;
			float priority = env.zonePriority;
			float blendWeight = 1.0f;
			float blendRadius = 0.0f;
			bool enabled = true;
			if (const auto vit = scene.envVolumeForNode.find(node);
				vit != scene.envVolumeForNode.end()) {
				infMask = vit->second.infMask;
				priority = vit->second.priority; // volume entry wins
				blendWeight = vit->second.blendWeight;
				blendRadius = vit->second.blendRadius;
				enabled = vit->second.enabled;
			}
			if (!enabled || blendWeight <= 0.0f)
				continue;

			Candidate cand{};
			const smvk::envzones::EnvZone look = smvk::envzones::DeriveZoneLook(
				env, scene.environment);
			if (!smvk::envzones::MakeEnvZoneGpu(scene.globalTransform[node],
					infMask, look, priority, blendWeight, blendRadius, cand.zone))
				continue; // empty zone (flat finite axis without an inf face)

			// Bounded volume: the product of the finite column lengths;
			// any unbounded face/INF column = +inf.
			const glm::mat4& g = scene.globalTransform[node];
			double volume = 1.0;
			bool unbounded = false;
			for (int c = 0; c < 3; ++c) {
				const float len = glm::length(glm::vec3(g[c]));
				if (!std::isfinite(len) || len <= 0.0f
					|| (infMask & (3u << (2 * c))) != 0u) {
					unbounded = true;
					break;
				}
				volume *= double(len) * 2.0;
			}
			NodeZone nz{};
			nz.cand = cand;
			nz.cand.record = 0; // assigned after the sort
			nz.cand.boundedVolume = unbounded
				? std::numeric_limits<double>::infinity() : volume;
			nz.cand.nodeId = node;
			nz.priority = priority;
			nz.volume = nz.cand.boundedVolume;
			nz.nodeId = node;
			nodeZones.push_back(nz);
		}
		// The packer's sort: priority desc -> volume asc (innermost) ->
		// node id. Past the record budget: keep the TOP zones (the base
		// never truncates — it is candidates_[0] and the sort is stable,
		// but slice from the front and re-append the base if the cut hits
		// it, exactly like the old overflow guard).
		std::stable_sort(nodeZones.begin(), nodeZones.end(),
			[](const NodeZone& a, const NodeZone& b) {
				if (a.priority != b.priority)
					return a.priority > b.priority;
				if (a.volume != b.volume)
					return a.volume < b.volume;
				return a.nodeId < b.nodeId;
			});
		const uint32_t maxNodeZones = kEnvTileMaxRecords - 1; // [0] = base
		uint32_t assigned = 0;
		for (const NodeZone& nz : nodeZones) {
			if (assigned >= maxNodeZones)
				break;
			Candidate cand = nz.cand;
			cand.record = 1 + assigned; // records_[0] is the base
			candidates_.push_back(cand);
			// Store the LOOK into the record.
			smvk::envzones::EnvZone look;
			look.skyTint = glm::vec3(cand.zone.sky);
			look.ambientI = cand.zone.sky.w;
			look.fogColor = glm::vec3(cand.zone.fog);
			look.fogDensity = cand.zone.fog.w;
			look.volumetrics = cand.zone.vol.x;
			look.volumetricDensity = cand.zone.vol.y;
			records_[cand.record].sky = glm::vec4(look.skyTint, look.ambientI);
			records_[cand.record].fog = glm::vec4(look.fogColor, look.fogDensity);
			records_[cand.record].vol = glm::vec4(look.volumetrics,
				look.volumetricDensity, 0.0f, 0.0f);
			++assigned;
		}
		recordCount_ = 1 + assigned;
	}

	// Re-sort the FULL candidate list (base included) with the packer's
	// ordering so Evaluate()'s first-hit is the winner, matching the
	// retired shader walk exactly.
	std::stable_sort(candidates_.begin(), candidates_.end(),
		[](const Candidate& a, const Candidate& b) {
			if (a.priority != b.priority)
				return a.priority > b.priority;
			if (a.boundedVolume != b.boundedVolume)
				return a.boundedVolume < b.boundedVolume;
			return a.nodeId < b.nodeId;
		});

	// ---- Raster: every tile column walks a ~1 m vertical slice ladder,
	// bisects each flip edge, and packs the merged interval list. (The
	// retired lo/mid/hi triple + a single lo/hi bisection could only ever
	// find ONE flip: an enclosed room — base below, the interior inside,
	// base above — bisected between the SAME base winners at both ends
	// and the interior record VANISHED from the bake entirely; the
	// review's finding. The ladder sees every band at its resolution and
	// the cell now carries three records.)
	struct ColumnInterval
	{
		uint32_t record = 0;
		float lo = 0.0f, hi = 0.0f; // flip edges (refined)
	};
	const float yLo = yMin + 0.01f;
	const float yHi = yMax - 0.01f;
	const float height = yHi - yLo;
	// ~1 m slices on a 64 m scene; clamped 17..129 (taller scenes trade
	// vertical resolution, same class as the retired 6-step bisection).
	const int sliceCount = int(std::clamp(std::ceil(height) + 1.0f,
		17.0f, 129.0f));
	// (resize, not the vexing parse: vector<T> v(count) declares a
	// function when count is wrapped in a cast's parens.)
	std::vector<Lookup> slices;
	slices.resize(size_t(sliceCount));
	std::vector<ColumnInterval> intervals;
	uint32_t rejectedColumns = 0;
	cells_.assign(size_t(dimX_) * dimZ_, EnvTileCell{});
	for (uint32_t iz = 0; iz < dimZ_; ++iz)
		for (uint32_t ix = 0; ix < dimX_; ++ix) {
			const float wx = gridMin_.x + (float(ix) + 0.5f) * kEnvTileSize;
			const float wz = gridMin_.y + (float(iz) + 0.5f) * kEnvTileSize;
			EnvTileCell& cell = cells_[size_t(iz) * dimX_ + ix];

			// 1) The ladder: the winner at each slice height.
			for (int i = 0; i < sliceCount; ++i)
				slices[size_t(i)] = Evaluate(glm::vec3(wx,
					yLo + height * (float(i) / float(sliceCount - 1)), wz));

			// 2) The intervals: runs of equal winners, each flip edge
			// refined by a 6-step bisection INSIDE its ladder gap (a
			// sub-gap band reads as the gap's far record — 1 m accuracy,
			// the retired bake's own class).
			intervals.clear();
			uint32_t cur = slices[0].record;
			float runLo = yLo;
			for (int i = 0; i + 1 < sliceCount; ++i) {
				if (slices[size_t(i + 1)].record == cur)
					continue;
				float a = yLo + height * (float(i) / float(sliceCount - 1));
				float b = yLo + height * (float(i + 1) / float(sliceCount - 1));
				for (int it = 0; it < 6; ++it) {
					const float m = 0.5f * (a + b);
					if (Evaluate(glm::vec3(wx, m, wz)).record == cur)
						a = m;
					else
						b = m;
				}
				const float edge = 0.5f * (a + b);
				intervals.push_back(ColumnInterval{ cur, runLo, edge });
				cur = slices[size_t(i + 1)].record;
				runLo = edge;
			}
			intervals.push_back(ColumnInterval{ cur, runLo, yHi });
			// Sanitize the list: drop degenerate zero-width runs (two
			// bisection edges landing equal — belt and braces), then fuse
			// same-record neighbors the drop can leave behind. Adjacent
			// intervals SHARE an edge by construction (one run's hi is
			// the next run's lo) — that is NOT a merge condition.
			for (size_t k = 0; k < intervals.size();)
				if (intervals[k].hi <= intervals[k].lo)
					intervals.erase(intervals.begin() + ptrdiff_t(k));
				else
					++k;
			for (size_t k = 0; intervals.size() > 1
				&& k < intervals.size() - 1;)
				if (intervals[k].record == intervals[k + 1].record) {
					intervals[k].hi = intervals[k + 1].hi;
					intervals.erase(intervals.begin() + ptrdiff_t(k) + 1);
				} else {
					++k;
				}

			// 3) Pack: 1 = no split, 2 = the splitY plane, 3 = the mid
			// band, >3 = the explicit reject (the largest vertical mass —
			// a 4+ stack of env zones in one 64 m column is not
			// representable; keep the dominant look, log once per bake).
			const auto midSample = [&](const ColumnInterval& in) {
				const Lookup l = Evaluate(glm::vec3(wx,
					0.5f * (in.lo + in.hi), wz));
				return l.record == in.record ? l.weight : 1.0f;
			};
			if (intervals.size() > 3) {
				size_t best = 0;
				for (size_t k = 1; k < intervals.size(); ++k)
					if (intervals[k].hi - intervals[k].lo
						> intervals[best].hi - intervals[best].lo)
						best = k;
				cell.lowerRecord = intervals[best].record;
				cell.upperRecord = intervals[best].record;
				cell.splitY = -std::numeric_limits<float>::max();
				cell.lowerWeight = midSample(intervals[best]);
				cell.upperWeight = cell.lowerWeight;
				cell.midRecord = 0xFFFFFFFFu;
				++rejectedColumns;
				continue;
			}
			cell.lowerRecord = intervals.front().record;
			cell.upperRecord = intervals.back().record;
			cell.lowerWeight = midSample(intervals.front());
			cell.upperWeight = midSample(intervals.back());
			if (intervals.size() == 1) {
				cell.splitY = -std::numeric_limits<float>::max();
				cell.midRecord = 0xFFFFFFFFu;
				continue;
			}
			cell.splitY = intervals[0].hi;
			if (intervals.size() == 2) {
				cell.midRecord = 0xFFFFFFFFu;
				continue;
			}
			// Three: the mid band bounded by [splitY, midHiY].
			cell.midRecord = intervals[1].record;
			cell.midHiY = intervals[1].hi;
			cell.midWeight = midSample(intervals[1]);
		}
	if (rejectedColumns > 0)
		std::fprintf(stderr, "[EnvTiles] %u column(s) carry 4+ vertical "
			"records — not representable (64 m tile, 3 slots); kept the "
			"largest vertical mass per column\n", rejectedColumns);
	built_ = true;
	return true;
}

WorldEnvTiles::Lookup WorldEnvTiles::Evaluate(const glm::vec3& p) const
{
	// The retired shader walk, verbatim: the FIRST influencing candidate
	// in the sorted list wins; nothing covering falls to the BASE record
	// at weight 1 (candidates_[0] is the all-infinite base).
	for (const Candidate& c : candidates_) {
		const float w = std::clamp(c.zone.shape.y, 0.0f, 1.0f)
			* smvk::envzones::EnvZoneFalloff(c.zone, p);
		if (!(w > 0.0f))
			continue;
		Lookup out{};
		out.record = c.record;
		out.weight = w;
		out.valid = true;
		return out;
	}
	Lookup base{};
	base.record = 0;
	base.weight = 1.0f;
	base.valid = true;
	return base;
}

WorldEnvTiles::Lookup WorldEnvTiles::At(glm::vec3 pos) const
{
	Lookup out{};
	out.valid = false;
	if (!built_)
		return out;
	// Out of grid: the base record at weight 1 (the zone-zero semantic —
	// the tile grid is the scene bounds + margin, beyond it the default
	// environment applies, matching infinite zones' outside behavior).
	const float fx = std::floor((pos.x - gridMin_.x) / kEnvTileSize);
	const float fz = std::floor((pos.z - gridMin_.y) / kEnvTileSize);
	if (!(fx >= 0.0f) || !(fz >= 0.0f)
		|| fx >= float(dimX_) || fz >= float(dimZ_)) {
		out.record = 0;
		out.weight = 1.0f;
		out.valid = true;
		return out;
	}
	const EnvTileCell& cell = cells_[size_t(fz) * dimX_ + size_t(fx)];
	// The vertical profile: the optional mid band first (the enclosed
	// room), else the splitY half-spaces.
	if (cell.midRecord != 0xFFFFFFFFu
		&& pos.y >= cell.splitY && pos.y <= cell.midHiY) {
		out.record = cell.midRecord;
		out.weight = cell.midWeight;
	} else if (pos.y > cell.splitY) {
		out.record = cell.upperRecord;
		out.weight = cell.upperWeight;
	} else {
		out.record = cell.lowerRecord;
		out.weight = cell.lowerWeight;
	}
	out.valid = true;
	return out;
}

} // namespace engine::worldpartition
