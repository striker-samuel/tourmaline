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
	auto it = layers_.find(programId);
	if (it == layers_.end()) return 0;
	TileKey key{ tile, programId };
	PlacementTile& t = tiles_[key]; // creates on demand
	t.key = key;
	t.state = TileState::Generating;
	t.instances = EvaluateTile(it->second, tile);
	t.deps = deps;
	t.state = TileState::Resident;
	t.lastAccess = ++tick_;
	return t.instances.size();
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

std::vector<RenderInstance> EvaluateTile(const PlacementSystem::LayerBinding& layer, TileID tile) {
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
		auto values = EvaluateProgram(layer.program, s);
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
		in.customB = 0;
		accepted.push_back(in);
		acceptedIds.push_back(id);
	}

	if (layer.config.minSpacing > 0.0f && accepted.size() > 1)
		ApplyMinDistance(accepted, acceptedIds, layer.config.minSpacing);
	return accepted;
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
	const std::vector<StableID>& ids, float spacing) {
	if (instances.empty() || spacing <= 0.0f) return;
	const size_t n = instances.size();
	// Phase A: hash into cells, keep highest-priority index per cell.
	std::unordered_map<CellKey, size_t, CellKeyHash> winner;
	winner.reserve(n * 2);
	auto cellOf = [spacing](const glm::vec3& p) -> CellKey {
		return CellKey{ (int32_t)std::floor(p.x / spacing),
			(int32_t)std::floor(p.y / spacing),
			(int32_t)std::floor(p.z / spacing) };
	};
	for (size_t i = 0; i < n; ++i) {
		CellKey c = cellOf(instances[i].position);
		auto it = winner.find(c);
		if (it == winner.end()) winner[c] = i;
		else if (PriorityOf(ids[i]) > PriorityOf(ids[it->second])) it->second = i;
	}
	// Phase B: winners test 27-neighbourhood; lower priority dies on conflict.
	// Resolution order is ascending candidate index (deterministic on all
	// platforms — never unordered_map iteration order, which is not portable).
	std::vector<size_t> order;
	order.reserve(winner.size());
	for (auto& [c, idx] : winner) order.push_back(idx);
	std::sort(order.begin(), order.end());
	std::vector<uint8_t> alive(n, 0);
	for (size_t idx : order) alive[idx] = 1;
	const float minD2 = spacing * spacing;
	for (size_t oi = 0; oi < order.size(); ++oi) {
		size_t idx = order[oi];
		if (!alive[idx]) continue;
		CellKey c = cellOf(instances[idx].position);
		for (int dx = -1; dx <= 1; ++dx)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dz = -1; dz <= 1; ++dz) {
					auto it = winner.find(CellKey{ c.x + dx, c.y + dy, c.z + dz });
					if (it == winner.end() || it->second == idx || !alive[it->second]) continue;
					size_t j = it->second;
					glm::vec3 d = instances[idx].position - instances[j].position;
					if (glm::dot(d, d) < minD2) {
						// Lower priority dies. If j dies, it stays dead;
						// if idx dies, stop testing it further.
						if (PriorityOf(ids[idx]) >= PriorityOf(ids[j])) alive[j] = 0;
						else { alive[idx] = 0; goto nextWinner; }
					}
				}
	nextWinner:;
	}
	// Compact survivors in ascending candidate-index order (deterministic).
	std::vector<RenderInstance> kept;
	kept.reserve(n);
	// Deterministic output order: ascending candidate-index order.
	for (size_t i = 0; i < n; ++i)
		if (alive[i]) kept.push_back(instances[i]);
	instances.swap(kept);
}

} // namespace smvk::placement
