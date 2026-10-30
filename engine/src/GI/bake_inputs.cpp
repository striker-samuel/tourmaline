// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// See bake_inputs.h for the model (the unit hash, the merkle, the 1 m
// bound's case, the observed-reach sweep, the no-compat contract).

#include <Engine/GI/bake_inputs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
	if (scene.tables.lights.empty())
		return BakeInputHash(std::string("lights-none"));
	std::vector<uint32_t> nodes;
	nodes.reserve(scene.tables.lights.size());
	for (const LightRow& row : scene.tables.lights)
		nodes.push_back(row.node);
	std::sort(nodes.begin(), nodes.end());
	Hasher hs;
	hs.feed("lights");
	size_t count = 0;
	for (uint32_t node : nodes) {
		const LightRow* it = FindLight(scene, node);
		if (!it)
			continue;
		const ClusterLightHandle handle =
			cluster.lights.getHandle(it->light);
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
	// Bake-relevant TEXTURE CONTENT fingerprints, keyed by the cluster
	// cook texture id (the MaterialValues texture slots): the bakes trace
	// TEXTURED albedo/transmission (per-hit baseColor sampling —
	// GiTraceScene::AlbedoAt/TransmissionAt — plus the metallicRoughness
	// and emissive averages BuildTraceSceneFromCluster folds in), so the
	// texture CONTENT is a bake input. The old leaf folded only the
	// material's scalar state: swapping a texture's PIXELS while the
	// material parameters (and the slot ids) stood still kept the same
	// hash — a stale bake rode into the pak undetected. The fingerprint
	// hashes the embedded SOURCE bytes (what every decode path decodes);
	// a registered uri with no embedded source folds a stable marker so
	// the bake and the pak gate replay the SAME deterministic fold (both
	// replay the same cluster build — sources present or absent in
	// lockstep).
	std::map<uint32_t, std::string> textureFp;
	for (const auto& [uri, handle] : cluster.ListTextureUris()) {
		Hasher th;
		th.feed("tex");
		th.feed(uri.data(), uri.size());
		const std::vector<uint8_t>* src = cluster.GetTextureSource(uri);
		if (src && !src->empty())
			th.feed(src->data(), src->size());
		else
			th.feed("<no-source>");
		textureFp[uint32_t(handle.index())] = th.str();
	}
	// Per-cell member lists first (table row order is the merge/append
	// order — deterministic per cluster, but sorted members make the
	// fold stable across cluster sets).
	std::map<CellId, std::vector<std::string>> members;
	grid.meshCount = 0;
	for (const MeshRow& row : scene.tables.meshes) {
		const NodeId node = row.node;
		const ClusterStaticMeshHandle meshHandle =
			cluster.staticMeshes.getHandle(row.mesh);
		if (!meshHandle.valid())
			continue;
		const StaticMeshUpload* upload =
			cluster.GetRetainedUpload(meshHandle);
		if (!upload || upload->vertices.empty()
				|| upload->indices.empty())
			continue;
		const MaterialRow* matRow = FindMaterial(scene, node);
		if (!matRow)
			continue;
		const ClusterMaterialHandle matHandle =
			cluster.materials.getHandle(matRow->material);
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
			upload->vertices.size() * sizeof(ImportStaticVertex));
		leaf.feed(upload->indices.data(),
			upload->indices.size() * sizeof(uint32_t));
		leaf.feedPod(g);
		leaf.feedPod(mat->values);
		leaf.feedPod(mat->shadingModel);
		leaf.feedPod(mat->alphaMode);
		leaf.feedPod(mat->alphaCutoff);
		leaf.feedPod(mat->castsShadows);
		// The material's BAKE-RELEVANT texture content: the slots any bake
		// may sample — baseColor (the textured albedo + the weave
		// transmission texels), metallicRoughness (the effective metallic
		// average), emissive (the emissive average). No slot (-1) or a slot
		// with no fingerprint folds the stable empty marker.
		{
			const int32_t slots[3] = {
				mat->values.textures.x, mat->values.textures.y,
				mat->values.textures2.x,
			};
			for (int32_t id : slots) {
				auto it = id >= 0
					? textureFp.find(uint32_t(id)) : textureFp.end();
				leaf.feed(it != textureFp.end() ? it->second
					: std::string("<no-texture>"));
			}
		}
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

std::string GiVolumeBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const SceneGiVolume& def,
	const glm::mat4& nodeGlobal, const GiVolumeBakeConfigHashInputs& cfg) {
	// The volume's world AABB: the node global's basis lengths are the
	// full extents (the harvest's OBB decode); the 8 corners give the
	// region, expanded by the margin ring. The corners are POSITIONS
	// (w = 1): the old w = 0 dropped nodeGlobal's translation column, so
	// a MOVED volume kept hashing the cells around the ORIGIN — the
	// covered region (and the hash) stayed put while the volume flew
	// away, and stale bakes kept matching.
	glm::vec3 mn(1e30f), mx(-1e30f);
	for (int i = 0; i < 8; ++i) {
		glm::vec4 p(0.0f);
		p.x = (i & 1) ? 1.0f : -1.0f;
		p.y = (i & 2) ? 1.0f : -1.0f;
		p.z = (i & 4) ? 1.0f : -1.0f;
		p.w = 1.0f; // homogeneous POINT: transform carries the translation
		const glm::vec3 w(nodeGlobal * p);
		mn = glm::min(mn, w);
		mx = glm::max(mx, w);
	}
	const float m = BakeCellGrid::kCell * float(BakeCellGrid::kMargin);
	mn -= glm::vec3(m);
	mx += glm::vec3(m);

	Hasher hs;
	hs.feed("givolume");
	// The bake algorithm revision (see bake_inputs.h): behavior changes
	// must invalidate every pre-change record.
	hs.feedPod(kBakeAlgorithmRevision);
	hs.feed(BakeEnvHash(cluster.scene));
	hs.feed(BakeLightsHash(cluster));
	// The volume's FULL transform, by bits: the covered-cell fold below
	// only sees the world AABB, so a rotation that leaves the covered
	// cells identical (a near-symmetric OBB spin) or any re-aimed basis
	// never changed the hash — the bake's sample lattice rides the whole
	// matrix, so fold the whole matrix.
	hs.feedPod(nodeGlobal);
	// The definition's bake-relevant fields (the shape rides nodeGlobal,
	// folded above).
	hs.feedPod(def.priority);
	hs.feedPod(def.spacing);
	hs.feedPod(def.insideFade);
	hs.feedPod(def.outsideFade);
	// Layout-selection inputs (the grid optimizer contract): any gate or
	// seed change must invalidate the stale bake — the chosen lattice
	// moved, so the recorded hash must refuse to match.
	hs.feedPod(def.spacingLow);
	hs.feedPod(def.spacingHigh);
	hs.feedPod(def.optSpacing);
	hs.feedPod(def.optPhase[0]);
	hs.feedPod(def.optPhase[1]);
	hs.feedPod(def.optPhase[2]);
	hs.feedPod(cfg.captureDirs);
	hs.feedPod(cfg.maxBounces);
	hs.feedPod(cfg.captureSpacing);
	hs.feedPod(cfg.smoothingPasses);
	hs.feedPod(cfg.maskTransmission);
	// The firefly clamp + the trace-scene albedo mode (the review-found
	// gaps: changing --clamp or --uniform-albedo re-traces everything,
	// so stale records must refuse).
	hs.feedPod(cfg.clampRatio);
	hs.feedPod(cfg.albedoUniform);
	hs.feedPod(cfg.albedoValue);
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
	// The bake algorithm revision (see bake_inputs.h): behavior changes
	// must invalidate every pre-change record.
	hs.feedPod(kBakeAlgorithmRevision);
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
	hs.feedPod(cfg.spp);
	hs.feedPod(cfg.maskTransmission);
	// The transport depth + the trace-scene albedo mode (the same
	// review-found gap class: a --cubemap-bounces or albedo change must
	// refuse stale captures).
	hs.feedPod(cfg.maxBounces);
	hs.feedPod(cfg.albedoUniform);
	hs.feedPod(cfg.albedoValue);
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

// (WorldBakeUnitHash — the legacy .sggi world-field hash — is deleted with
// the octree bake: stage 3 of the octree retirement. The live per-product
// unit hashes above carry the freshness model.)

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
		// Zero-direction axes never cross a boundary on that axis: their
		// tMax/step are +infinity, which stays out of every min() race.
		// The old code divided by the RAW axis component — an exactly-zero
		// component with the position exactly on a cell boundary produced
		// 0/0 = NaN (NaN never wins a < comparison, silently skewing the
		// axis picks), and a zero component off-boundary produced a
		// sign-dependent ±inf by luck. The boundary-crossing parameter
		// below uses the guarded reciprocal form (a multiply, never a
		// division by a possibly-zero raw component).
		int32_t cell[3] = { CellOf(position.x), CellOf(position.y),
			CellOf(position.z) };
		float tMax[3], step[3];
		int32_t sgn[3];
		for (int a = 0; a < 3; ++a) {
			const float d = dir[a];
			if (std::abs(d) < 1e-12f) {
				// The ray runs parallel to this axis's cell planes: no
				// crossing ever (tMax=+inf loses every min() race; the
				// step is never added before t > tHit).
				sgn[a] = 1; // unused: tMax[a] is +inf forever
				tMax[a] = (std::numeric_limits<float>::infinity)();
				step[a] = (std::numeric_limits<float>::infinity)();
				continue;
			}
			sgn[a] = d > 0.0f ? 1 : -1;
			const float inv = 1.0f / d; // |d| >= 1e-12: the divide is safe
			tMax[a] = ((float(cell[a] + (sgn[a] > 0 ? 1 : 0))
					* BakeCellGrid::kCell) - position[a]) * inv;
			step[a] = BakeCellGrid::kCell * std::abs(inv);
		}
		float t = 0.0f;
		int32_t guard = 0;
		while (t <= tHit && guard++ < 4096) {
			cells.push_back({ cell[0], cell[1], cell[2] });
			// Advance the axis with the NEAREST boundary crossing (ties
			// fall to the later axis — deterministic and matches the old
			// if/else chain's ordering).
			int axis = 2;
			if (tMax[0] < tMax[1])
				axis = (tMax[0] < tMax[2]) ? 0 : 2;
			else
				axis = (tMax[1] < tMax[2]) ? 1 : 2;
			t = tMax[axis];
			tMax[axis] += step[axis];
			cell[axis] += sgn[axis];
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
	if (size < 8)
		return false;
	const uint8_t* p = data;
	const uint8_t* end = data + size;
	uint32_t magic = 0, count = 0;
	if (!GetU32(p, end, magic) || !GetU32(p, end, count))
		return false;
	if (magic != CaptureReachFile::kMagic)
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
