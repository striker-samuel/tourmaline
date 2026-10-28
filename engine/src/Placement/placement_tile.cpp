// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_tile.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace smvk::placement {
namespace {

// Model matrix for transient draws is built by the caller; yaw-only here.
float SlopeDegFromNormal(const glm::vec3& n) {
	if (n.y >= 1.0f) return 0.0f;
	if (n.y <= 0.0f) return 90.0f;
	return std::acos(std::clamp(n.y, -1.0f, 1.0f)) * 57.29578f;
}

} // namespace

void PlacementSystem::registerLayer(LayerBinding layer) {
	layers_[layer.programId] = std::move(layer);
}

bool PlacementSystem::hasLayer(uint64_t programId) const {
	return layers_.find(programId) != layers_.end();
}

size_t PlacementSystem::generateTile(uint64_t programId, TileID tile, const DependencyVersions& deps) {
	uint64_t generation = 0;
	if (!GenerateTilePending(programId, tile, deps, generation))
		return 0;
	// The synchronous form publishes immediately (the result IS complete
	// when this returns; the token never goes stale in between).
	PublishActivation(TileKey{ tile, programId }, generation);
	const PlacementTile* t = findTile(programId, tile);
	return t ? t->instances.size() : 0;
}

bool PlacementSystem::GenerateTilePending(uint64_t programId, TileID tile,
	const DependencyVersions& deps, uint64_t& generationOut) {
	generationOut = 0;
	auto it = layers_.find(programId);
	if (it == layers_.end()) return false;
	TileKey key{ tile, programId };
	PlacementTile& t = tiles_[key]; // creates on demand
	t.key = key;
	// The token contract: a Generating tile KEEPS its in-flight
	// activation token (UpdateResidency's claim) — the caller publishes
	// against THAT token and a re-activation (bumping past it) refuses
	// the stale job. Every other state (fresh, Resident-refresh, Dirty)
	// claims a NEW token.
	if (t.state != TileState::Generating)
		t.generation = ++tick_;
	t.state = TileState::Generating;
	t.deps = deps;
	t.lastAccess = tick_;
	generationOut = t.generation;
	std::vector<StableID> ids;
	t.instances = EvaluateTile(it->second, tile, &ids);
	// Sort for the instanced draw path: contiguous per-prototype runs.
	// Key (prototypeId, StableID) is fully deterministic, so regen and
	// cross-platform order agree exactly.
	SortInstancesForDraw(t.instances, ids);
	ComputeTileBounds(t);
	return true;
}

void PlacementSystem::invalidateLayer(uint64_t programId) {
	for (auto& [key, tile] : tiles_)
		if (key.programId == programId && tile.state == TileState::Resident)
			tile.state = TileState::Dirty;
}

void PlacementSystem::invalidateAll() {
	for (auto& [key, tile] : tiles_)
		if (tile.state == TileState::Resident)
			tile.state = TileState::Dirty;
}

void PlacementSystem::evictBeyond(size_t maxResident, const std::vector<TileKey>& keep) {
	++tick_;
	for (const TileKey& k : keep) {
		auto it = tiles_.find(k);
		if (it != tiles_.end()) it->second.lastAccess = tick_;
	}
	// Collect resident candidates, oldest first.
	std::vector<std::pair<uint64_t, TileKey>> order;
	for (const auto& [key, tile] : tiles_)
		if (tile.state == TileState::Resident)
			order.emplace_back(tile.lastAccess, key);
	if (order.size() <= maxResident) return;
	std::sort(order.begin(), order.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });
	for (size_t i = 0; i < order.size() - maxResident; ++i) {
		auto it = tiles_.find(order[i].second);
		if (it != tiles_.end()) {
			it->second.state = TileState::Evicting;
			it->second.instances.clear();
			it->second.instances.shrink_to_fit();
			tiles_.erase(it);
		}
	}
}

void PlacementSystem::pushVisibleDraws(uint64_t programId,
	const std::function<bool(const glm::vec3&, float)>& frustumTest,
	std::vector<TransientDraw>& out) const {
	for (const auto& [key, tile] : tiles_) {
		if (key.programId != programId || tile.state != TileState::Resident) continue;
		for (const RenderInstance& in : tile.instances) {
			const float radius = 0.5f * std::max(in.scale.x, std::max(in.scale.y, in.scale.z));
			if (frustumTest && !frustumTest(in.position, radius)) continue;
			TransientDraw d;
			d.position = in.position;
			d.yaw = in.yaw;
			d.scale = in.scale;
			d.prototypeId = in.prototypeId;
			d.stableLow = in.stableLow;
			out.push_back(d);
		}
	}
}

size_t PlacementSystem::residentTileCount() const {
	size_t n = 0;
	for (const auto& [key, tile] : tiles_)
		if (tile.state == TileState::Resident) ++n;
	return n;
}
size_t PlacementSystem::residentInstanceCount(uint64_t programId) const {
	size_t n = 0;
	for (const auto& [key, tile] : tiles_)
		if (key.programId == programId && tile.state == TileState::Resident)
			n += tile.instances.size();
	return n;
}

const PlacementTile* PlacementSystem::findTile(uint64_t programId, TileID tile) const {
	auto it = tiles_.find(TileKey{ tile, programId });
	return it != tiles_.end() ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// World-tile residency (PR05)
// ---------------------------------------------------------------------------
void PlacementSystem::UpdateResidency(uint64_t programId, float tileSize,
	const glm::vec4* rects, size_t rectCount,
	const DependencyVersions& deps, size_t maxResidentTiles,
	std::vector<ActivationRequest>& out) {
	out.clear();
	if (tileSize <= 0.0f || !hasLayer(programId)) return;
	// The half-open interest tile set (deduplicated; small — a linear
	// member test beats a hash dependency).
	std::vector<TileID> wanted;
	auto addTile = [&](TileID t) {
		for (const TileID& w : wanted)
			if (w == t) return;
		wanted.push_back(t);
	};
	auto hasTile = [&](TileID t) {
		for (const TileID& w : wanted)
			if (w == t) return true;
		return false;
	};
	for (size_t r = 0; r < rectCount; ++r) {
		const glm::vec4& rc = rects[r]; // minX, minZ, maxX, maxZ
		if (!(rc.z > rc.x) || !(rc.w > rc.y)) continue;
		// Floor at the min, exclusive at the max (the half-open rule:
		// a max exactly on a tile boundary does NOT include the next
		// tile — neighbors never duplicate shared borders).
		const int32_t tx0 = (int32_t)std::floor(rc.x / tileSize);
		const int32_t tz0 = (int32_t)std::floor(rc.y / tileSize);
		const int32_t tx1 = (int32_t)std::ceil(rc.z / tileSize) - 1;
		const int32_t tz1 = (int32_t)std::ceil(rc.w / tileSize) - 1;
		for (int32_t tz = tz0; tz <= tz1; ++tz)
			for (int32_t tx = tx0; tx <= tx1; ++tx)
				addTile(TileID{ tx, 0, tz });
	}

	// 1) Fresh interests enter Generating; stale residents (deps changed
	// OR region-invalidated Dirty) regenerate with a NEW token; tiles
	// already Generating keep their in-flight claim (a deps change
	// under them re-activates — the older result's publish refuses).
	for (const TileID& tile : wanted) {
		const TileKey key{ tile, programId };
		auto it = tiles_.find(key);
		if (it == tiles_.end()) {
			PlacementTile& t = tiles_[key];
			t.key = key;
			t.state = TileState::Generating;
			t.deps = deps;
			t.generation = ++tick_;
			t.lastAccess = tick_;
			ActivationRequest req;
			req.key = key;
			req.generation = t.generation;
			req.deps = deps;
			req.regenerate = false;
			out.push_back(req);
		} else if (it->second.state == TileState::Generating) {
			// In flight. If the deps CHANGED under it, re-activate with a
			// new token — the in-flight result evaluated the old deps and
			// its publish must be refused (never publish stale contents
			// recorded as fresh).
			if (it->second.deps != deps) {
				it->second.deps = deps;
				it->second.generation = ++tick_;
				it->second.lastAccess = tick_;
				ActivationRequest req;
				req.key = key;
				req.generation = it->second.generation;
				req.deps = deps;
				req.regenerate = true;
				out.push_back(req);
			} else {
				it->second.lastAccess = ++tick_;
			}
		} else if (it->second.state == TileState::Resident
			&& it->second.deps == deps) {
			// Resident + fresh: keep (the LRU touch below covers it).
			// Camera movement NEVER regenerates an unchanged tile.
		} else {
			// Resident-with-changed-deps, or region-Dirtied (the
			// InvalidateRegion path): regenerate with a NEW token.
			it->second.state = TileState::Generating;
			it->second.deps = deps;
			it->second.generation = ++tick_;
			it->second.lastAccess = tick_;
			ActivationRequest req;
			req.key = key;
			req.generation = it->second.generation;
			req.deps = deps;
			req.regenerate = true;
			out.push_back(req);
		}
	}

	// 2) LRU-touch every resident interest tile (never evict interests).
	for (const TileID& tile : wanted) {
		auto it = tiles_.find(TileKey{ tile, programId });
		if (it != tiles_.end() && it->second.state == TileState::Resident)
			it->second.lastAccess = ++tick_;
	}

	// 3) Evict beyond budget: NON-interest residents first, LRU. The
	// interest set itself is the caller's budget responsibility (it
	// derives from the camera interest — bounded by construction).
	{
		size_t kept = 0; // the interest set's residents AND in-flight
		for (const auto& [key, t] : tiles_)
			if (key.programId == programId
				&& (t.state == TileState::Resident || t.state == TileState::Generating)
				&& hasTile(key.tile))
				++kept;
		size_t nonInterest = 0;
		for (const auto& [key, t] : tiles_)
			if (key.programId == programId && t.state == TileState::Resident
				&& !hasTile(key.tile))
				++nonInterest;
		size_t toDrop = kept + nonInterest > maxResidentTiles
			? kept + nonInterest - maxResidentTiles : 0;
		if (toDrop > 0) {
			std::vector<std::pair<uint64_t, TileKey>> order;
			order.reserve(nonInterest);
			for (const auto& [key, t] : tiles_)
				if (key.programId == programId && t.state == TileState::Resident
					&& !hasTile(key.tile))
					order.emplace_back(t.lastAccess, key);
			std::sort(order.begin(), order.end(),
				[](const auto& a, const auto& b) { return a.first < b.first; });
			for (size_t i = 0; i < order.size() && toDrop > 0; ++i) {
				auto it = tiles_.find(order[i].second);
				if (it != tiles_.end()) {
					tiles_.erase(it);
					--toDrop;
				}
			}
		}
	}
}

void PlacementSystem::InvalidateRegion(uint64_t programId, float tileSize,
	const glm::vec2& rectMin, const glm::vec2& rectMax, float haloMeters) {
	if (tileSize <= 0.0f) return;
	// The rect + halo (the cross-boundary influence: a min-spacing
	// winner or a central-difference normal reading a neighbour's edge
	// data), then the same half-open walk.
	const glm::vec2 mn = rectMin - glm::vec2(haloMeters);
	const glm::vec2 mx = rectMax + glm::vec2(haloMeters);
	const int32_t tx0 = (int32_t)std::floor(mn.x / tileSize);
	const int32_t tz0 = (int32_t)std::floor(mn.y / tileSize);
	const int32_t tx1 = (int32_t)std::ceil(mx.x / tileSize) - 1;
	const int32_t tz1 = (int32_t)std::ceil(mx.y / tileSize) - 1;
	for (int32_t tz = tz0; tz <= tz1; ++tz)
		for (int32_t tx = tx0; tx <= tx1; ++tx) {
			auto it = tiles_.find(TileKey{ TileID{ tx, 0, tz }, programId });
			if (it != tiles_.end() && it->second.state == TileState::Resident)
				it->second.state = TileState::Dirty;
		}
}

bool PlacementSystem::PublishActivation(const TileKey& key, uint64_t generation) {
	auto it = tiles_.find(key);
	if (it == tiles_.end()) return false;   // dropped while running
	PlacementTile& t = it->second;
	// The stale-token refusal: a newer activation took the tile (or it
	// was invalidated + re-activated) while this job ran — drop the
	// result; the next residency tick regenerates with the fresh deps.
	if (t.state != TileState::Generating || t.generation != generation)
		return false;
	t.state = TileState::Resident;
	t.lastAccess = ++tick_;
	return true;
}

std::vector<RenderInstance> EvaluateTile(const PlacementSystem::LayerBinding& layer,
	TileID tile, std::vector<StableID>* idsOut) {
	std::vector<RenderInstance> accepted;
	std::vector<StableID> acceptedIds;
	const uint32_t n = layer.config.candidatesPerTile;
	accepted.reserve(n / 4);
	acceptedIds.reserve(n / 4);

	for (uint32_t i = 0; i < n; ++i) {
		StableID id = MakeStableID(layer.program.graphSeed, layer.program.nodeId, tile, 0, i);
		DomainSample s;
		switch (layer.domainKind) {
		case DomainKind::Grid:
			if (!layer.grid) continue;
			s = layer.grid->sample(id, i);
			break;
		case DomainKind::MeshSurface:
			if (!layer.mesh) continue;
			s = SampleMeshSurface(*layer.mesh, id);
			break;
		case DomainKind::Heightfield:
			if (!layer.heightfield) continue;
			s = layer.heightfield->sample(id);
			break;
		}
		if (!layer.program.valid()) continue;
		auto values = EvaluateProgram(layer.program, s, layer.ecotope);
		float density = values[layer.program.acceptIndex];
		if (!AcceptCandidate(density, id, layer.config.densityScale)) continue;

		// Prototype: weighted pick on VARIANT, optional slope-to-rock rule.
		uint32_t proto = 0;
		if (!layer.prototypes.empty()) {
			float r = Random01(id, RandomChannel::Variant);
			proto = layer.prototypes.select(r).value_or(0);
			if (layer.config.slopeToRock && layer.prototypes.size() > 1)
				proto = SelectBySlope(layer.prototypes, SlopeDegFromNormal(s.n),
					layer.config.slopeThresholdDeg, proto, 1);
		}
		RenderInstance in;
		in.position = s.p;
		in.yaw = YawFromId(id);
		float sc = UniformScaleFromId(id, layer.config.minScale, layer.config.maxScale);
		in.scale = glm::vec3(sc);
		in.prototypeId = proto;
		in.stableLow = id.w[0];
		in.customB = i; // candidate index: GPU constraint passes recompute IDs
		accepted.push_back(in);
		acceptedIds.push_back(id);
	}

	if (layer.config.minSpacing > 0.0f && accepted.size() > 1)
		ApplyMinDistance(accepted, acceptedIds, layer.config.minSpacing,
			ConstraintVolumeForLayer(layer));
	if (idsOut) *idsOut = acceptedIds;
	return accepted;
}

ConstraintVolume ConstraintVolumeForLayer(const PlacementSystem::LayerBinding& layer) {
	ConstraintVolume vol;
	vol.spacing = layer.config.minSpacing;
	// Constraint volume covers the tile that spawned these candidates.
	// Grid candidates live in [grid.origin, +extent]; mesh candidates
	// may overshoot and skip via the out-of-volume rule (same on GPU).
	if (layer.domainKind == DomainKind::Grid && layer.grid) {
		vol.origin = glm::vec3(layer.grid->origin.x, layer.grid->origin.y - 64.0f,
			layer.grid->origin.z);
		vol.dimX = (uint32_t)(layer.grid->extent.x / vol.spacing) + 2;
		vol.dimY = (uint32_t)(128.0f / vol.spacing) + 2;
		vol.dimZ = (uint32_t)(layer.grid->extent.y / vol.spacing) + 2;
	}
	return vol;
}

namespace {
// 3D cell key for the min-distance spatial hash.
struct CellKey { int32_t x, y, z; bool operator==(const CellKey& o) const {
	return x == o.x && y == o.y && z == o.z; } };
struct CellKeyHash { size_t operator()(const CellKey& k) const {
	uint32_t h = SplitMix32((uint32_t)k.x + 0x9e3779b9u);
	h = HashMix32(h, (uint32_t)k.y);
	h = HashMix32(h, (uint32_t)k.z);
	return (size_t)h;
} };
} // namespace

void ApplyMinDistance(std::vector<RenderInstance>& instances,
	std::vector<StableID>& ids, float spacing, const ConstraintVolume& volume) {
	if (instances.empty() || spacing <= 0.0f) return;
	const size_t n = instances.size();
	const bool bounded = volume.dimX > 0 && volume.dimY > 0 && volume.dimZ > 0
		&& volume.spacing > 0.0f;
	const float cs = bounded ? volume.spacing : spacing;

	struct EKey { int64_t a, b, c; bool operator==(const EKey& o) const {
		return a == o.a && b == o.b && c == o.c; } };
	struct EKeyHash { size_t operator()(const EKey& k) const {
		uint32_t h = SplitMix32((uint32_t)(k.a & 0xFFFFFFFF) + 0x9e3779b9u);
		h = HashMix32(h, (uint32_t)(k.b & 0xFFFFFFFF));
		h = HashMix32(h, (uint32_t)(k.c & 0xFFFFFFFF));
		return (size_t)h;
	} };

	// Volume-cell coords of p; false when outside a bounded volume.
	// Unbounded volumes use absolute spacing-sized cells instead.
	auto volCell = [&](const glm::vec3& p, int64_t& ox, int64_t& oy, int64_t& oz) -> bool {
		if (!bounded) {
			ox = (int64_t)std::floor(p.x / cs);
			oy = (int64_t)std::floor(p.y / cs);
			oz = (int64_t)std::floor(p.z / cs);
			return true;
		}
		ox = (int64_t)std::floor((p.x - volume.origin.x) / volume.spacing);
		oy = (int64_t)std::floor((p.y - volume.origin.y) / volume.spacing);
		oz = (int64_t)std::floor((p.z - volume.origin.z) / volume.spacing);
		return ox >= 0 && oy >= 0 && oz >= 0
			&& ox < (int64_t)volume.dimX && oy < (int64_t)volume.dimY
			&& oz < (int64_t)volume.dimZ;
	};
	// Elector key from cell coords: volume-linear when bounded (exact for
	// any origin alignment), raw coords otherwise.
	auto keyFor = [&](int64_t ix, int64_t iy, int64_t iz) -> EKey {
		if (bounded)
			return EKey{ (iz * (int64_t)volume.dimY + iy) * (int64_t)volume.dimX + ix, 0, 0 };
		return EKey{ ix, iy, iz };
	};

	// Phase 1 — elect: per key, argmax (priority, tie -> smaller index).
	std::unordered_map<EKey, size_t, EKeyHash> winner;
	winner.reserve(n * 2);
	std::vector<uint8_t> inVolume(n, 1);
	std::vector<int64_t> coX(n, 0), coY(n, 0), coZ(n, 0);
	for (size_t i = 0; i < n; ++i) {
		int64_t ix, iy, iz;
		if (!volCell(instances[i].position, ix, iy, iz)) { inVolume[i] = 0; continue; }
		coX[i] = ix; coY[i] = iy; coZ[i] = iz;
		EKey k = keyFor(ix, iy, iz);
		auto it = winner.find(k);
		if (it == winner.end()) {
			winner[k] = i;
		} else {
			size_t j = it->second;
			uint64_t pi = PriorityOf(ids[i]), pj = PriorityOf(ids[j]);
			if (pi > pj || (pi == pj && i < j)) it->second = i;
		}
	}
	// Phase 2 — kill vs the winner snapshot (order-free: reads never
	// observe concurrent writes, so any thread order agrees bit-exactly).
	std::vector<uint8_t> alive(n, 0);
	for (auto& [k, idx] : winner) alive[idx] = 1;
	const float minD2 = spacing * spacing;
	std::vector<size_t> order;
	order.reserve(winner.size());
	for (auto& [k, idx] : winner) order.push_back(idx);
	std::sort(order.begin(), order.end()); // deterministic traversal only
	for (size_t idx : order) {
		// Neighbourhood walk in cell-COORDINATE space: any conflicting
		// partner (dist < spacing <= cell size) is at most one cell away
		// per axis, whatever the volume alignment. Bounded neighbours get
		// bounds-checked (outside -> no winner there).
		for (int dx = -1; dx <= 1; ++dx)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dz = -1; dz <= 1; ++dz) {
					int64_t nx = coX[idx] + dx, ny = coY[idx] + dy, nz = coZ[idx] + dz;
					if (bounded && (nx < 0 || ny < 0 || nz < 0
						|| nx >= (int64_t)volume.dimX || ny >= (int64_t)volume.dimY
						|| nz >= (int64_t)volume.dimZ))
						continue;
					auto it = winner.find(keyFor(nx, ny, nz));
					if (it == winner.end() || it->second == idx) continue;
					size_t j = it->second;
					glm::vec3 d = instances[idx].position - instances[j].position;
					if (glm::dot(d, d) < minD2) {
						uint64_t pi = PriorityOf(ids[idx]), pj = PriorityOf(ids[j]);
						if (pj > pi || (pj == pi && j < idx)) { alive[idx] = 0; goto nextCand; }
					}
				}
	nextCand:;
	}
	// Compact: survivors plus out-of-volume skips, ascending index order.
	// ids compact in parallel so they stay aligned with instances.
	std::vector<RenderInstance> kept;
	kept.reserve(n);
	std::vector<StableID> keptIds;
	keptIds.reserve(n);
	for (size_t i = 0; i < n; ++i)
		if (alive[i] || !inVolume[i]) { kept.push_back(instances[i]); keptIds.push_back(ids[i]); }
	instances.swap(kept);
	ids.swap(keptIds);
}

void SortInstancesForDraw(std::vector<RenderInstance>& instances, std::vector<StableID>& ids) {
	const size_t n = instances.size();
	if (n < 2 || ids.size() != n) return;
	std::vector<size_t> order(n);
	for (size_t i = 0; i < n; ++i) order[i] = i;
	auto less = [&](size_t a, size_t b) {
		if (instances[a].prototypeId != instances[b].prototypeId)
			return instances[a].prototypeId < instances[b].prototypeId;
		for (int w = 0; w < 4; ++w)
			if (ids[a].w[w] != ids[b].w[w]) return ids[a].w[w] < ids[b].w[w];
		return false;
	};
	std::sort(order.begin(), order.end(), less);
	std::vector<RenderInstance> si;
	si.reserve(n);
	std::vector<StableID> sj;
	sj.reserve(n);
	for (size_t k : order) { si.push_back(instances[k]); sj.push_back(ids[k]); }
	instances.swap(si);
	ids.swap(sj);
}

void ComputeTileBounds(PlacementTile& tile) {
	if (tile.instances.empty()) {
		tile.boundsMin = tile.boundsMax = glm::vec3(0.0f);
		return;
	}
	glm::vec3 mn = tile.instances[0].position;
	glm::vec3 mx = mn;
	for (const RenderInstance& in : tile.instances) {
		mn = glm::min(mn, in.position);
		mx = glm::max(mx, in.position);
	}
	tile.boundsMin = mn;
	tile.boundsMax = mx;
}

} // namespace smvk::placement
