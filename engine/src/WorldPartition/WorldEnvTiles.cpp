// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include <Engine/Rendergraph/envzones.h>
#include <Engine/WorldPartition/WorldEnvTiles.h>

namespace engine::worldpartition {

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

	// ---- Raster: every tile column evaluates the winner at THREE sample
	// heights (lo/mid/hi over the scene's Y range); equal winners collapse
	// to the no-split cell, different winners get a bisection split. ----
	const float yLo = yMin + 0.01f;
	const float yHi = yMax - 0.01f;
	const float yMid = 0.5f * (yLo + yHi);
	cells_.assign(size_t(dimX_) * dimZ_, EnvTileCell{});
	for (uint32_t iz = 0; iz < dimZ_; ++iz)
		for (uint32_t ix = 0; ix < dimX_; ++ix) {
			const float wx = gridMin_.x + (float(ix) + 0.5f) * kEnvTileSize;
			const float wz = gridMin_.y + (float(iz) + 0.5f) * kEnvTileSize;
			EnvTileCell& cell = cells_[size_t(iz) * dimX_ + ix];
			const Lookup lo = Evaluate(glm::vec3(wx, yLo, wz));
			const Lookup mid = Evaluate(glm::vec3(wx, yMid, wz));
			const Lookup hi = Evaluate(glm::vec3(wx, yHi, wz));
			if (lo.record == hi.record && mid.record == lo.record) {
				// No vertical change: one record, the mid-sample weight.
				cell.lowerRecord = mid.record;
				cell.upperRecord = mid.record;
				cell.splitY = -std::numeric_limits<float>::max();
				cell.lowerWeight = mid.weight;
				cell.upperWeight = mid.weight;
				continue;
			}
			// Vertical split: bisect the Y where the winner flips between
			// the lo and hi records (6 steps — a 1 m resolution on a 64 m
			// scene height; sub-tile Y detail is not representable by
			// design, this only needs the PLANE).
			float a = yLo;
			float b = yHi;
			for (int it = 0; it < 6; ++it) {
				const float m = 0.5f * (a + b);
				const Lookup lm = Evaluate(glm::vec3(wx, m, wz));
				if (lm.record == lo.record)
					a = m;
				else
					b = m;
			}
			cell.lowerRecord = lo.record;
			cell.upperRecord = hi.record;
			cell.splitY = 0.5f * (a + b);
			// Slot weights at the quarter samples of their halves.
			const Lookup loQ = Evaluate(glm::vec3(wx, 0.5f * (yLo + cell.splitY), wz));
			const Lookup hiQ = Evaluate(glm::vec3(wx, 0.5f * (cell.splitY + yHi), wz));
			cell.lowerWeight = loQ.record == lo.record ? loQ.weight : lo.weight;
			cell.upperWeight = hiQ.record == hi.record ? hiQ.weight : hi.weight;
		}
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
	const bool upper = pos.y > cell.splitY;
	out.record = upper ? cell.upperRecord : cell.lowerRecord;
	out.weight = upper ? cell.upperWeight : cell.lowerWeight;
	out.valid = true;
	return out;
}

} // namespace engine::worldpartition
