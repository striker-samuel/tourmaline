// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// See bake_inputs.h for the model (the unit hash, the merkle, the 1 m
// bound's case, the observed-reach sweep, the no-compat contract).

#include <Engine/GI/bake_inputs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace smvk::gi {

// ---------------------------------------------------------------------------
// The hash primitive
// ---------------------------------------------------------------------------

namespace {

// FNV-1a 64 with a length finish — byte-identical to the artifact cache's
// HashBytes (local-cache grade, deterministic across runs/platforms).
uint64_t Fnv64(const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < size; ++i) {
		h ^= bytes[i];
		h *= 1099511628211ull;
	}
	h ^= uint64_t(size) * 0x9E3779B97F4A7C15ull;
	h *= 0xFF51AFD7ED558CCDull;
	h ^= h >> 32;
	return h;
}

void AppendHex(std::string& out, uint64_t v) {
	static const char kHex[] = "0123456789abcdef";
	for (int i = 15; i >= 0; --i)
		out.push_back(kHex[(v >> (4 * i)) & 0xF]);
}

// Streaming hasher: the unit hashes fold labeled fields (domain tags
// make collisions between differently-shaped inputs structurally
// impossible, and the length finish separates concatenations).
struct Hasher {
	uint64_t h = 1469598103934665603ull;
	size_t bytes = 0;
	void feed(const void* data, size_t size) {
		const auto* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i) {
			h ^= p[i];
			h *= 1099511628211ull;
		}
		bytes += size;
	}
	void feed(const std::string& s) { feed(s.data(), s.size()); }
	template <typename T>
	void feedPod(const T& v) {
		static_assert(std::is_trivially_copyable_v<T>);
		feed(&v, sizeof(v));
	}
	std::string str() const {
		uint64_t f = h;
		f ^= uint64_t(bytes) * 0x9E3779B97F4A7C15ull;
		f *= 0xFF51AFD7ED558CCDull;
		f ^= f >> 32;
		std::string out;
		out.reserve(16);
		AppendHex(out, f);
		return out;
	}
};

inline int32_t CellOf(float v) {
	return int32_t(std::floor(v / BakeCellGrid::kCell));
}

} // namespace

std::string BakeInputHash(const void* data, size_t size) {
	std::string out;
	out.reserve(16);
	AppendHex(out, Fnv64(data, size));
	return out;
}

// ---------------------------------------------------------------------------
// The env + static-lights hashes (GLOBAL inputs — fold into every unit)
// ---------------------------------------------------------------------------

std::string BakeEnvHash(const Scene& scene) {
	// The bake env inputs (EnvForBake's set): the resolved ACTIVE block's
	// sun (az/el/I + color) + sky (I + color), raw float BITS. Exposure
	// is a view setting, not a bake input; fog is not traced by bakes.
	Hasher hs;
	hs.feed("env");
	const SceneEnv& e = scene.environment;
	hs.feedPod(e.skyColor);
	hs.feedPod(e.skyIntensity);
	hs.feedPod(e.sunColor);
	hs.feedPod(e.sunAzimuth);
	hs.feedPod(e.sunElevation);
	hs.feedPod(e.sunIntensity);
	return hs.str();
}

std::string BakeLightsHash(const Cluster& cluster) {
	// Every STATIC-MARKED light (the bake light set), sorted by node id:
	// the light's own fields + the node's global transform bits (the
	// position/direction ride the transform) + the cookie uri. Non-static
	// lights are runtime-only — never bake inputs.
	const ::Scene& scene = cluster.scene;
	if (scene.lightForNode.empty())
		return BakeInputHash(std::string("lights-none"));
	std::vector<uint32_t> nodes;
	nodes.reserve(scene.lightForNode.size());
	for (const auto& [node, idx] : scene.lightForNode)
		nodes.push_back(node);
	std::sort(nodes.begin(), nodes.end());
	Hasher hs;
	hs.feed("lights");
	size_t count = 0;
	for (uint32_t node : nodes) {
		auto it = scene.lightForNode.find(node);
		if (it == scene.lightForNode.end())
			continue;
		const ClusterLightHandle handle =
			cluster.lights.getHandle(it->second);
		if (!handle.valid())
			continue;
		const SceneLight* L = cluster.lights.get(handle);
		if (!L || !(L->flags & SceneLight::kSceneLightFlag_Static))
			continue;
		++count;
		const glm::mat4& g =
			scene.globalTransform.size() > node
			? scene.globalTransform[node] : glm::mat4(1.0f);
		hs.feedPod(node);
		hs.feedPod(L->type);
		hs.feedPod(L->color);
		hs.feedPod(L->intensity);
		hs.feedPod(L->range);
		hs.feedPod(L->innerConeAngle);
		hs.feedPod(L->outerConeAngle);
		hs.feedPod(L->flags);
		hs.feed(L->cookieUri);
		hs.feedPod(g);
	}
	hs.feedPod(uint64_t(count)); // a light deleted from the set changes it
	return hs.str();
}

// ---------------------------------------------------------------------------
// The geometry merkle
// ---------------------------------------------------------------------------

BakeCellGrid BuildBakeCellGrid(Cluster& cluster) {
	BakeCellGrid grid;
	::Scene& scene = cluster.scene;
	recalculateGlobalTransforms(scene);

	// Membership mirrors BuildTraceSceneFromCluster's SUPerset (masked
	// cutouts + decal overlays included — some bake opts trace them):
	// a retained upload + a material. The hash must cover everything any
	// bake might trace.
	// Per-cell member lists first (unordered iteration of meshForNode is
	// order-nondeterministic — sorted members make the fold stable).
	std::map<CellId, std::vector<std::string>> members;
	grid.meshCount = 0;
	for (const auto& [node, meshIdx] : scene.meshForNode) {
		const ClusterStaticMeshHandle meshHandle =
			cluster.staticMeshes.getHandle(meshIdx);
		if (!meshHandle.valid())
			continue;
		const StaticMeshUpload* upload =
			cluster.GetRetainedUpload(meshHandle);
		if (!upload || upload->vertices.empty()
				|| upload->indices.empty())
			continue;
		auto matIt = scene.materialForNode.find(node);
		if (matIt == scene.materialForNode.end())
			continue;
		const ClusterMaterialHandle matHandle =
			cluster.materials.getHandle(matIt->second);
		if (!matHandle.valid())
			continue;
		const Material* mat = cluster.materials.get(matHandle);
		if (!mat)
			continue;

		const glm::mat4 g =
			scene.globalTransform.size() > node
			? scene.globalTransform[node] : glm::mat4(1.0f);
		// The leaf: the mesh bytes (vertices + indices — the actual traced
		// content; ~30 MB folds in milliseconds) + the world AABB + the
		// transform bits + the material's full state (MaterialValues is a
		// 128-byte POD) + the material's non-values fields.
		Hasher leaf;
		leaf.feed("mesh");
		leaf.feed(upload->vertices.data(),
			upload->vertices.size() * sizeof(StaticVertex));
		leaf.feed(upload->indices.data(),
			upload->indices.size() * sizeof(uint32_t));
		leaf.feedPod(g);
		leaf.feedPod(mat->values);
		leaf.feedPod(mat->shadingModel);
		leaf.feedPod(mat->alphaMode);
		leaf.feedPod(mat->alphaCutoff);
		leaf.feedPod(mat->castsShadows);
		const std::string leafHash = leaf.str();
		++grid.meshCount;

		// The world AABB: transform the 8 local corners (the upload's aabb
		// is local; blend/transmissive included like the trace does).
		glm::vec3 mn(1e30f), mx(-1e30f);
		const glm::vec3 c[8] = {
			{ upload->aabbMin.x, upload->aabbMin.y, upload->aabbMin.z },
			{ upload->aabbMax.x, upload->aabbMin.y, upload->aabbMin.z },
			{ upload->aabbMax.x, upload->aabbMax.y, upload->aabbMin.z },
			{ upload->aabbMin.x, upload->aabbMax.y, upload->aabbMin.z },
			{ upload->aabbMin.x, upload->aabbMin.y, upload->aabbMax.z },
			{ upload->aabbMax.x, upload->aabbMin.y, upload->aabbMax.z },
			{ upload->aabbMax.x, upload->aabbMax.y, upload->aabbMax.z },
			{ upload->aabbMin.x, upload->aabbMax.y, upload->aabbMax.z },
		};
		for (const glm::vec3& p : c) {
			const glm::vec3 w(g * glm::vec4(p, 1.0f));
			mn = glm::min(mn, w);
			mx = glm::max(mx, w);
		}
		for (int32_t z = CellOf(mn.z); z <= CellOf(mx.z); ++z)
			for (int32_t y = CellOf(mn.y); y <= CellOf(mx.y); ++y)
				for (int32_t x = CellOf(mn.x); x <= CellOf(mx.x); ++x)
					members[{ x, y, z }].push_back(leafHash);
	}

	// Fold each cell over its SORTED members (deterministic regardless of
	// the iteration order above), then the root over the ordered cells.
	for (auto& [cell, list] : members) {
		std::sort(list.begin(), list.end());
		Hasher ch;
		ch.feed("cell");
		ch.feedPod(cell);
		for (const std::string& m : list)
			ch.feed(m);
		grid.cells[cell] = ch.str();
	}
	{
		Hasher root;
		root.feed("root");
		for (const auto& [cell, hash] : grid.cells) {
			root.feedPod(cell);
			root.feed(hash);
		}
		grid.root = root.str();
	}
	return grid;
}

std::string BakeRegionHash(const BakeCellGrid& grid,
	const glm::vec3& mn, const glm::vec3& mx) {
	Hasher hs;
	hs.feed("region");
	bool any = false;
	for (int32_t z = CellOf(mn.z); z <= CellOf(mx.z); ++z)
		for (int32_t y = CellOf(mn.y); y <= CellOf(mx.y); ++y)
			for (int32_t x = CellOf(mn.x); x <= CellOf(mx.x); ++x) {
				auto it = grid.cells.find({ x, y, z });
				// A cell ABSENT from the grid is empty geometry — fold a
				// stable empty marker so new content appearing in a
				// formerly-empty region changes the fold.
				hs.feedPod(std::array<int32_t, 3>{ x, y, z });
				hs.feed(it != grid.cells.end() ? it->second
					: std::string("<empty>"));
				any = true;
			}
	hs.feedPod(any);
	return hs.str();
}

// ---------------------------------------------------------------------------
// Unit hashes
// ---------------------------------------------------------------------------

std::string CvolBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const SceneCvol& def,
	const glm::mat4& nodeGlobal, const CvolBakeConfigHashInputs& cfg) {
	// The volume's world AABB: the node global's basis lengths are the
	// full extents (the harvest's OBB decode); the 8 corners give the
	// region, expanded by the margin ring.
	glm::vec3 mn(1e30f), mx(-1e30f);
	for (int i = 0; i < 8; ++i) {
		glm::vec4 p(0.0f);
		p.x = (i & 1) ? 1.0f : -1.0f;
		p.y = (i & 2) ? 1.0f : -1.0f;
		p.z = (i & 4) ? 1.0f : -1.0f;
		const glm::vec3 w(nodeGlobal * p);
		mn = glm::min(mn, w);
		mx = glm::max(mx, w);
	}
	const float m = BakeCellGrid::kCell * float(BakeCellGrid::kMargin);
	mn -= glm::vec3(m);
	mx += glm::vec3(m);

	Hasher hs;
	hs.feed("cvol");
	hs.feed(BakeEnvHash(cluster.scene));
	hs.feed(BakeLightsHash(cluster));
	// The definition's bake-relevant fields (the shape rides nodeGlobal,
	// which is folded above).
	hs.feedPod(def.priority);
	hs.feedPod(def.spacing);
	hs.feedPod(def.insideFade);
	hs.feedPod(def.outsideFade);
	hs.feedPod(cfg.captureDirs);
	hs.feedPod(cfg.maxBounces);
	hs.feedPod(cfg.captureSpacing);
	hs.feedPod(cfg.smoothingPasses);
	hs.feedPod(cfg.maskTransmission);
	hs.feed(BakeRegionHash(grid, mn, mx));
	return hs.str();
}

std::string CubemapBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const glm::vec3& position,
	float influenceRadius, float fadeRadius, const glm::vec3& boxMin,
	const glm::vec3& boxMax, bool pinned,
	const CubemapBakeConfigHashInputs& cfg,
	const std::vector<CellId>& reachCells) {
	Hasher hs;
	hs.feed("cubemap");
	hs.feed(BakeEnvHash(cluster.scene));
	hs.feed(BakeLightsHash(cluster));
	hs.feedPod(position);
	hs.feedPod(influenceRadius);
	hs.feedPod(fadeRadius);
	hs.feedPod(boxMin);
	hs.feedPod(boxMax);
	hs.feedPod(pinned);
	hs.feedPod(cfg.size);
	hs.feedPod(cfg.rays);
	hs.feedPod(cfg.maskTransmission);
	// The OBSERVED reach: the recorded cells' CURRENT grid hashes (a
	// cell now absent = empty geometry = the stable empty marker). This
	// closes both directions — a changed seen cell, and new content in a
	// formerly-empty traversed cell.
	for (const CellId& cell : reachCells) {
		hs.feedPod(cell);
		auto it = grid.cells.find(cell);
		hs.feed(it != grid.cells.end() ? it->second
			: std::string("<empty>"));
	}
	hs.feedPod(uint64_t(reachCells.size()));
	return hs.str();
}

std::string WorldBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const WorldBakeConfigHashInputs& cfg) {
	Hasher hs;
	hs.feed("world");
	hs.feed(BakeEnvHash(cluster.scene));
	hs.feed(BakeLightsHash(cluster));
	hs.feedPod(cfg.maxBounces);
	hs.feedPod(cfg.maskTransmission);
	hs.feed(grid.root); // global: the octree bake path-traces globally
	return hs.str();
}

// ---------------------------------------------------------------------------
// The observed-reach sweep
// ---------------------------------------------------------------------------

std::vector<CellId> RecordCaptureReach(const GiTraceScene& trace,
	const glm::vec3& position, uint32_t dirCount) {
	// The deterministic ray set: a Fibonacci sphere (even coverage, no
	// RNG state, stable across runs — the sweep must observe the same
	// cells for the same scene).
	if (dirCount == 0)
		dirCount = 128;
	std::vector<CellId> cells;
	cells.reserve(dirCount * 16);
	const TriangleBvh& bvh = trace.Bvh();
	const float kMaxDist = 2000.0f; // escapes the scene = open sky
	for (uint32_t i = 0; i < dirCount; ++i) {
		const float z = 1.0f - 2.0f * float(i + 0.5f) / float(dirCount);
		const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
		const float phi = 2.399963229728653f * float(i);
		const glm::vec3 dir(r * std::cos(phi), r * std::sin(phi), z);
		TriangleBvh::Hit hit;
		const float tHit = bvh.Raycast(position, dir, kMaxDist, hit)
			? hit.t : kMaxDist;
		// The traversal: an Amanatides-Woo voxel walk over [0, tHit].
		int32_t cx = CellOf(position.x), cy = CellOf(position.y),
			cz = CellOf(position.z);
		const glm::vec3 invDir(
			1.0f / (std::abs(dir.x) < 1e-12f ? 1e-12f * (dir.x < 0 ? -1.0f : 1.0f) : dir.x),
			1.0f / (std::abs(dir.y) < 1e-12f ? 1e-12f * (dir.y < 0 ? -1.0f : 1.0f) : dir.y),
			1.0f / (std::abs(dir.z) < 1e-12f ? 1e-12f * (dir.z < 0 ? -1.0f : 1.0f) : dir.z));
		const int32_t sx = dir.x > 0 ? 1 : -1, sy = dir.y > 0 ? 1 : -1,
			sz = dir.z > 0 ? 1 : -1;
		float tMaxX = ((float(cx + (sx > 0 ? 1 : 0)) * BakeCellGrid::kCell)
				- position.x) / dir.x;
		float tMaxY = ((float(cy + (sy > 0 ? 1 : 0)) * BakeCellGrid::kCell)
				- position.y) / dir.y;
		float tMaxZ = ((float(cz + (sz > 0 ? 1 : 0)) * BakeCellGrid::kCell)
				- position.z) / dir.z;
		const float stepX = BakeCellGrid::kCell / std::abs(dir.x);
		const float stepY = BakeCellGrid::kCell / std::abs(dir.y);
		const float stepZ = BakeCellGrid::kCell / std::abs(dir.z);
		float t = 0.0f;
		int32_t guard = 0;
		while (t <= tHit && guard++ < 4096) {
			cells.push_back({ cx, cy, cz });
			if (tMaxX < tMaxY && tMaxX < tMaxZ) {
				t = tMaxX;
				tMaxX += stepX;
				cx += sx;
			}
			else if (tMaxY < tMaxZ) {
				t = tMaxY;
				tMaxY += stepY;
				cy += sy;
			}
			else {
				t = tMaxZ;
				tMaxZ += stepZ;
				cz += sz;
			}
		}
		// The hit cell + a one-cell ring (the bounce neighborhood —
		// bounce 1 leaves the hit surface into the adjacent cells).
		if (tHit < kMaxDist) {
			const glm::vec3 h = position + dir * tHit;
			const int32_t hx = CellOf(h.x), hy = CellOf(h.y),
				hz2 = CellOf(h.z);
			for (int32_t dz = -1; dz <= 1; ++dz)
				for (int32_t dy = -1; dy <= 1; ++dy)
					for (int32_t dx = -1; dx <= 1; ++dx)
						cells.push_back({ hx + dx, hy + dy, hz2 + dz });
		}
	}
	std::sort(cells.begin(), cells.end());
	cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
	return cells;
}

// ---------------------------------------------------------------------------
// The .reach sidecar
// ---------------------------------------------------------------------------

namespace {
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
bool GetU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
	if (end - p < 4)
		return false;
	v = 0;
	for (int i = 0; i < 4; ++i)
		v |= uint32_t(*p++) << (8 * i);
	return true;
}
} // namespace

bool SerializeCaptureReach(const CaptureReachFile& reach,
	std::vector<uint8_t>& out) {
	out.clear();
	PutU32(out, CaptureReachFile::kMagic);
	PutU32(out, CaptureReachFile::kVersion);
	PutU32(out, uint32_t(reach.entries.size()));
	for (const CaptureReachFile::Entry& e : reach.entries) {
		PutU32(out, e.rowIndex);
		PutU32(out, uint32_t(e.cells.size()));
		for (const CellId& c : e.cells)
			for (int i = 0; i < 3; ++i)
				PutU32(out, *reinterpret_cast<const uint32_t*>(&c[i]));
	}
	return true;
}

bool ParseCaptureReach(const uint8_t* data, size_t size,
	CaptureReachFile& out) {
	out.entries.clear();
	if (size < 12)
		return false;
	const uint8_t* p = data;
	const uint8_t* end = data + size;
	uint32_t magic = 0, version = 0, count = 0;
	if (!GetU32(p, end, magic) || !GetU32(p, end, version)
			|| !GetU32(p, end, count))
		return false;
	if (magic != CaptureReachFile::kMagic
			|| version != CaptureReachFile::kVersion)
		return false;
	if (count > 65536)
		return false;
	out.entries.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t cellCount = 0;
		if (!GetU32(p, end, out.entries[i].rowIndex)
				|| !GetU32(p, end, cellCount) || cellCount > (1u << 20))
			return false;
		out.entries[i].cells.resize(cellCount);
		for (uint32_t k = 0; k < cellCount; ++k)
			for (int a = 0; a < 3; ++a) {
				uint32_t v = 0;
				if (!GetU32(p, end, v))
					return false;
				std::memcpy(&out.entries[i].cells[k][a], &v, 4);
			}
	}
	return p == end;
}

} // namespace smvk::gi
