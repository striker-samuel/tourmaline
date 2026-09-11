#include <Engine/GI/bake.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <map>
#include <thread>

#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>
#include <Engine/Utils/thread_pool.h>

namespace smvk::gi {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxRayDist = 1.0e5f;
constexpr float kSurfEps = 1.0e-3f; // ray offset along the shading normal (m)

inline float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

glm::vec3 UniformSphere(Pcg32& rng) {
	const glm::vec2 u = rng.Float2();
	const float z = 1.0f - 2.0f * u.x;
	const float r = SafeSqrt(1.0f - z * z);
	const float phi = 2.0f * kPi * u.y;
	return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

inline void Onb(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
	const glm::vec3 up = std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	t = glm::normalize(glm::cross(up, n));
	b = glm::cross(n, t);
}

glm::vec3 CosineHemisphere(Pcg32& rng, const glm::vec3& n) {
	const glm::vec2 u = rng.Float2();
	const float r = std::sqrt(u.x);
	const float phi = 2.0f * kPi * u.y;
	glm::vec3 t, b;
	Onb(n, t, b);
	return glm::normalize(t * (r * std::cos(phi)) + n * std::sqrt(1.0f - u.x) + b * (r * std::sin(phi)));
}

} // namespace

// ---------------------------------------------------------------------------
// GiTraceScene
// ---------------------------------------------------------------------------

void GiTraceScene::Finalize() {
	bvh_.Build(positions, indices);
	const uint32_t triCount = uint32_t(indices.size() / 3);
	triMaterial.resize(std::min(size_t(triCount), triMaterial.size()));

	emissiveTris_.clear();
	emissiveCdf_.clear();
	emissiveTotal_ = 0.0f;
	for (uint32_t k = 0; k < triCount; ++k) {
		if (k >= triMaterial.size())
			break;
		const GiTraceMaterial& m = materials[triMaterial[k]];
		const float lum = Luminance(m.emissive);
		if (lum <= 1e-6f)
			continue;
		glm::vec3 v0, v1, v2;
		bvh_.TriangleVertices(k, v0, v1, v2);
		const float area = 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
		if (area <= 1e-9f)
			continue;
		EmissiveTri e{};
		e.tri = k;
		e.area = area;
		e.radiance = m.emissive;
		emissiveTotal_ += area * lum;
		emissiveTris_.push_back(e);
	}
	if (!emissiveTris_.empty()) {
		emissiveCdf_.resize(emissiveTris_.size());
		float cdf = 0.0f;
		for (size_t i = 0; i < emissiveTris_.size(); ++i) {
			cdf += emissiveTris_[i].area * Luminance(emissiveTris_[i].radiance);
			emissiveCdf_[i] = cdf;
		}
	}
}

glm::vec3 GiTraceScene::SkyRadiance(const glm::vec3& dir) const {
	const float exponent = std::max(skyExponent, 0.01f);
	const glm::vec3 zenith = skyZenith * skyIntensity;
	const glm::vec3 horizon = skyHorizon * skyIntensity;
	const glm::vec3 groundCol = skyGround * skyIntensity;
	const float t = std::clamp(dir.y, 0.0f, 1.0f);
	glm::vec3 sky = glm::mix(horizon, zenith, std::pow(t, exponent));
	if (dir.y < 0.0f)
		sky = glm::mix(horizon, groundCol, std::clamp(-dir.y * 3.0f, 0.0f, 1.0f));
	return sky;
}

glm::vec3 GiTraceScene::SampleEmissive(const glm::vec3& pos, const glm::vec3* n,
	const GiTraceMaterial* m, Pcg32& rng) const {
	if (emissiveTris_.empty() || emissiveTotal_ <= 0.0f)
		return glm::vec3(0.0f);

	const float u = rng.Float() * emissiveTotal_;
	const auto it = std::lower_bound(emissiveCdf_.begin(), emissiveCdf_.end(), u);
	size_t idx = size_t(it - emissiveCdf_.begin());
	idx = std::min(idx, emissiveTris_.size() - 1);
	const EmissiveTri& e = emissiveTris_[idx];
	const float pSelect = (e.area * Luminance(e.radiance)) / emissiveTotal_;

	glm::vec3 v0, v1, v2;
	bvh_.TriangleVertices(e.tri, v0, v1, v2);
	const glm::vec2 r = rng.Float2();
	const float s = std::sqrt(r.x);
	const float a = 1.0f - s;
	const float b = s * (1.0f - r.y);
	const float c = s * r.y;
	const glm::vec3 y = v0 * a + v1 * b + v2 * c;

	const glm::vec3 w = y - pos;
	const float dist2 = glm::dot(w, w);
	const float dist = std::sqrt(dist2);
	if (dist < 1e-4f)
		return glm::vec3(0.0f);
	const glm::vec3 dirTo = w / dist;

	if (n && glm::dot(dirTo, *n) <= 0.0f)
		return glm::vec3(0.0f);

	const glm::vec3 triN = bvh_.TriangleNormal(e.tri);
	const float cosTri = std::abs(glm::dot(triN, dirTo));
	if (cosTri < 1e-4f)
		return glm::vec3(0.0f);

	const glm::vec3 origin = n ? pos + (*n) * kSurfEps : pos + dirTo * kSurfEps;
	if (bvh_.AnyHit(origin, dirTo, dist - 2.0f * kSurfEps))
		return glm::vec3(0.0f);

	const float pdfSa = dist2 / (pSelect * e.area * cosTri);
	const float brdf = m ? glm::dot(dirTo, *n) * (1.0f / kPi) : 1.0f;
	return e.radiance * (brdf / pdfSa);
}

glm::vec3 GiTraceScene::SampleSun(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m) const {
	const float ndl = glm::dot(n, sunDirection);
	if (ndl <= 0.0f)
		return glm::vec3(0.0f);
	if (bvh_.AnyHit(pos + n * kSurfEps, sunDirection, kMaxRayDist))
		return glm::vec3(0.0f);
	return sunColor * (sunIntensity * ndl) * m.albedo * (1.0f / kPi);
}

glm::vec3 GiTraceScene::TraceIncoming(const glm::vec3& origin, const glm::vec3& dir,
	Pcg32& rng, uint32_t maxDepth) const {
	glm::vec3 L(0.0f);
	glm::vec3 throughput(1.0f);
	glm::vec3 p = origin;
	glm::vec3 d = glm::normalize(dir);

	bool primary = true;
	for (uint32_t depth = 0; depth <= maxDepth; ++depth) {
		TriangleBvh::Hit h;
		if (!bvh_.Raycast(p, d, kMaxRayDist, h)) {
			L += throughput * SkyRadiance(d);
			break;
		}
		const glm::vec3 pos = p + d * h.t;
		glm::vec3 tn = bvh_.TriangleNormal(h.tri);
		const glm::vec3 n = glm::dot(tn, d) < 0.0f ? tn : -tn;
		const GiTraceMaterial& m = materials[triMaterial[h.tri]];

		// NEE emissive at every vertex (primary vertex = raw radiance sample,
		// bounce vertices = Lambertian BRDF term). On-hit emission is always
		// skipped, so the two never double count.
		L += throughput * SampleEmissive(pos, primary ? nullptr : &n,
			primary ? nullptr : &m, rng);
		// Direct sun only at bounce vertices: primary-vertex sun is the
		// runtime's cascaded-shadow-map job.
		if (!primary)
			L += throughput * SampleSun(pos, n, m);

		throughput *= m.albedo;
		if (Luminance(throughput) < 1e-4f)
			break;
		d = CosineHemisphere(rng, n);
		p = pos + n * kSurfEps;
		primary = false;
	}
	return L;
}

glm::vec3 GiTraceScene::TraceIrradiance(const glm::vec3& origin, const glm::vec3& n,
	Pcg32& rng, uint32_t sampleCount, uint32_t maxDepth) const {
	glm::vec3 sum(0.0f);
	const glm::vec3 p = origin + n * kSurfEps;
	for (uint32_t i = 0; i < sampleCount; ++i)
		sum += TraceIncoming(p, CosineHemisphere(rng, n), rng, maxDepth);
	// Cosine-hemisphere sampling: E = pi * mean(L).
	return sum * (kPi / float(std::max(sampleCount, 1u)));
}

// ---------------------------------------------------------------------------
// Cluster extraction
// ---------------------------------------------------------------------------

void BuildTraceSceneFromCluster(Cluster& cluster, const GiSceneBuildOptions& opts,
	GiTraceScene& out, std::string& err) {
	::Scene& scene = cluster.scene;
	recalculateGlobalTransforms(scene);

	out.positions.clear();
	out.indices.clear();
	out.materials.clear();
	out.triMaterial.clear();

	// Node-material pairs first so materials vector indices are stable.
	auto internMaterial = [&](const Material& mat) -> uint32_t {
		GiTraceMaterial gm{};
		// metallic = factor * MR-texture B (the runtime shader convention:
		// glTF metallicFactor defaults to 1 with the real value in the
		// metallicRoughness texture's blue channel).
		float metallic = std::clamp(mat.values.params.y, 0.0f, 1.0f);
		if (mat.values.textures.y >= 0) {
			if (auto it = opts.texelAvg.find(uint32_t(mat.values.textures.y));
				it != opts.texelAvg.end())
				metallic *= it->second.b;
		}
		metallic = std::clamp(metallic, 0.0f, 1.0f);
		glm::vec3 base = glm::vec3(mat.values.diffuse);
		if (mat.values.textures.x >= 0) {
			if (auto it = opts.texelAvg.find(uint32_t(mat.values.textures.x));
				it != opts.texelAvg.end())
				base *= it->second;
		}
		gm.albedo = base * (1.0f - metallic);
		gm.emissive = opts.includeEmissive
			? glm::vec3(mat.values.emissiveFactor) * mat.values.params2.z
			: glm::vec3(0.0f);
		if (opts.includeEmissive && mat.values.textures2.x >= 0) {
			if (auto it = opts.texelAvg.find(uint32_t(mat.values.textures2.x));
				it != opts.texelAvg.end())
				gm.emissive *= it->second;
		}
		// Deduplicate identical materials (cheap win: scenes reuse them).
		for (uint32_t i = 0; i < out.materials.size(); ++i) {
			const GiTraceMaterial& o = out.materials[i];
			if (glm::all(glm::equal(o.albedo, gm.albedo))
				&& glm::all(glm::equal(o.emissive, gm.emissive)))
				return i;
		}
		out.materials.push_back(gm);
		return uint32_t(out.materials.size() - 1);
	};

	struct NodeGeo {
		uint32_t node;
		uint32_t materialIdx;
		const StaticMeshUpload* upload;
		glm::mat4 global;
	};
	std::vector<NodeGeo> geos;

	for (const auto& [node, meshIdx] : scene.meshForNode) {
		const ClusterStaticMeshHandle meshHandle = cluster.staticMeshes.getHandle(meshIdx);
		if (!meshHandle.valid()) {
			err = "BuildTraceSceneFromCluster: stale mesh handle";
			return;
		}
		const StaticMeshUpload* upload = cluster.GetRetainedUpload(meshHandle);
		if (!upload || upload->vertices.empty() || upload->indices.empty())
			continue;

		auto matIt = scene.materialForNode.find(node);
		if (matIt == scene.materialForNode.end())
			continue;
		const ClusterMaterialHandle matHandle = cluster.materials.getHandle(matIt->second);
		if (!matHandle.valid())
			continue;
		const Material* mat = cluster.materials.get(matHandle);
		if (!mat)
			continue;
		if (opts.skipBlended && mat->alphaMode == AlphaMode::Blend)
			continue; // glass: not GI-relevant, would only darken bounces
		if (!opts.includeMaskedCutout && mat->alphaMode == AlphaMode::Mask)
			continue;

		NodeGeo g{};
		g.node = node;
		g.upload = upload;
		g.global = scene.globalTransform[node];
		g.materialIdx = internMaterial(*mat);
		geos.push_back(g);
	}

	if (geos.empty()) {
		err = "BuildTraceSceneFromCluster: no renderable geometry (cook-mode cluster with retained uploads required)";
		return;
	}

	uint32_t triCount = 0;
	for (const NodeGeo& g : geos)
		triCount += uint32_t(g.upload->indices.size() / 3);
	out.positions.reserve(size_t(triCount) * 3);
	out.indices.reserve(size_t(triCount) * 3);
	out.triMaterial.reserve(triCount);

	for (const NodeGeo& g : geos) {
		const uint32_t base = uint32_t(out.positions.size());
		for (const StaticVertex& v : g.upload->vertices) {
			const glm::vec4 wp = g.global * glm::vec4(v.pos, 1.0f);
			out.positions.push_back(glm::vec3(wp));
		}
		for (uint32_t k = 0; k < g.upload->indices.size(); ++k)
			out.indices.push_back(base + g.upload->indices[k]);
		for (uint32_t t = 0; t < g.upload->indices.size() / 3; ++t)
			out.triMaterial.push_back(g.materialIdx);
	}

	// Environment from the runtime settings (same defaults as the app UI).
	const EnvironmentSettings& env = opts.env;
	out.sunDirection = env.SunDirectionToSun();
	out.sunColor = env.sunColor;
	out.sunIntensity = env.sunIntensity;
	out.skyZenith = env.skyZenithColor;
	out.skyHorizon = env.skyHorizonColor;
	out.skyGround = env.groundColor;
	out.skyIntensity = env.skyIntensity;
	out.skyExponent = env.skyExponent;

	out.Finalize();
}

// ---------------------------------------------------------------------------
// Bake: bricks, relocation, probes, refinement waves
// ---------------------------------------------------------------------------

namespace {

struct BakeBrick {
	glm::vec3 center{};
	float size = 0.0f;
	uint32_t level = 0;
	bool hasProbe = false;
	bool subdivided = false;
	glm::vec3 probePos{};
	float error = 0.0f;
	glm::vec3 amps[kSgLobeCount]{};
	float vis[6] = {};
};


// Pushes a probe candidate into free space: first away from the closest
// surface point, then a fixed direction sweep. Candidates stay inside the
// brick (the probe must keep representing it).
bool RelocateProbe(const TriangleBvh& bvh, const BakeBrick& brick,
	float clearance, glm::vec3& outPos) {
	const glm::vec3& center = brick.center;
	const float half = brick.size * 0.5f;
	auto clamped = [&](const glm::vec3& p) {
		return glm::clamp(p, center - half, center + half);
	};

	glm::vec3 best = center;
	float bestDist = bvh.ClosestDistance(center);
	if (bestDist >= clearance) {
		outPos = best;
		return true;
	}

	// March away from the closest surface point.
	{
		glm::vec3 cp;
		bvh.ClosestDistance(center, &cp);
		glm::vec3 dir = center - cp;
		const float len = glm::length(dir);
		dir = len > 1e-5f ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f);
		const float step = std::max(brick.size * 0.25f, 1e-3f);
		for (float t = step; t <= half * 2.0f + 1e-6f; t += step) {
			const glm::vec3 cand = clamped(center + dir * t);
			const float d = bvh.ClosestDistance(cand);
			if (d > bestDist) {
				bestDist = d;
				best = cand;
				if (d >= clearance)
					break;
			}
		}
	}
	if (bestDist >= clearance) {
		outPos = best;
		return true;
	}

	// Direction sweep: 6 axes + 8 cube diagonals (inside-solid rescue).
	static const glm::vec3 kDirs[14] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
		glm::normalize(glm::vec3(1, 1, 1)), glm::normalize(glm::vec3(1, 1, -1)),
		glm::normalize(glm::vec3(1, -1, 1)), glm::normalize(glm::vec3(1, -1, -1)),
		glm::normalize(glm::vec3(-1, 1, 1)), glm::normalize(glm::vec3(-1, 1, -1)),
		glm::normalize(glm::vec3(-1, -1, 1)), glm::normalize(glm::vec3(-1, -1, -1)),
	};
	const float step = std::max(brick.size * 0.25f, 1e-3f);
	for (const glm::vec3& dir : kDirs) {
		for (float t = step; t <= half * 2.0f + 1e-6f; t += step) {
			const glm::vec3 cand = clamped(center + dir * t);
			const float d = bvh.ClosestDistance(cand);
			if (d > bestDist) {
				bestDist = d;
				best = cand;
			}
		}
		if (bestDist >= clearance)
			break;
	}
	// Soft accept: partially-clear probes are still useful (their baked
	// visibility distances keep the runtime honest about the walls).
	if (bestDist > 0.0f && bestDist >= std::min(clearance, brick.size * 0.05f + 1e-3f)) {
		outPos = best;
		return true;
	}
	return false;
}

// Traces + fits one probe (radiance fit + 6-axis visibility + reconstruction
// error at off-center test points).
void BakeProbeBrick(const GiTraceScene& scene, const SgGiBakeConfig& cfg,
	BakeBrick& brick, uint64_t seed) {
	const float clearance = std::max(cfg.probeMinClearance,
		cfg.probeClearanceFactor * brick.size);
	if (!RelocateProbe(scene.Bvh(), brick, clearance, brick.probePos))
		return; // fully embedded: drop the leaf, neighbors cover
	brick.hasProbe = true;

	Pcg32 rng(seed);
	SgFitAccumulator acc;
	const float weight = 4.0f * kPi / float(cfg.raysPerProbe);
	for (uint32_t i = 0; i < cfg.raysPerProbe; ++i) {
		const glm::vec3 dir = UniformSphere(rng);
		const glm::vec3 L = scene.TraceIncoming(brick.probePos, dir, rng, cfg.maxBounces);
		acc.AddSample(dir, L, weight);
	}
	acc.Solve(brick.amps);

	// 6-axis visibility (obstacle distances, capped).
	static const glm::vec3 kAxes[6] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
	};
	for (uint32_t k = 0; k < 6; ++k) {
		TriangleBvh::Hit h;
		float d = cfg.visibilityMaxDistance;
		if (scene.Bvh().Raycast(brick.probePos, kAxes[k], cfg.visibilityMaxDistance, h))
			d = std::min(h.t, cfg.visibilityMaxDistance);
		brick.vis[k] = std::max(d, 0.0f);
	}

	// Error-driven refinement metric: reference irradiance at off-center
	// test points vs the probe's own SG reconstruction. Measures combined
	// spatial + angular error inside the brick.
	static const glm::vec3 kTestOffsets[2] = {
		{ 0.29f, 0.11f, -0.23f },
		{ -0.17f, -0.21f, 0.31f },
	};
	static const glm::vec3 kTestNormals[3] = {
		{ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
	};
	const float step = std::max(brick.size * 0.125f, 1e-3f);
	float errSum = 0.0f;
	float refLumSum = 0.0f;
	int refCount = 0;
	glm::vec3 refs[6];
	for (int t = 0; t < 2; ++t) {
		const glm::vec3 tp = brick.probePos + kTestOffsets[t] * brick.size;
		for (int nn = 0; nn < 3; ++nn) {
			// Nudge test points out of solid contact (they may sit inside a
			// wall the probe escaped): trace from slightly along the normal.
			const glm::vec3 n = kTestNormals[nn];
			glm::vec3 origin = tp;
			if (scene.Occluded(origin, -n, step))
				origin = tp + n * step;
			const glm::vec3 ref = scene.TraceIrradiance(
				origin, n, rng, cfg.errorRaysPerProbe, cfg.maxBounces);
			refs[t * 3 + nn] = ref;
			refLumSum += Luminance(ref);
			++refCount;
		}
	}
	const float floor = cfg.errorFloor * std::max(refLumSum / float(std::max(refCount, 1)), 1e-5f);
	for (int i = 0; i < 6; ++i) {
		const glm::vec3& ref = refs[i];
		const glm::vec3 rec = SgDiffuseIrradiance(brick.amps, kTestNormals[i % 3]);
		errSum += Luminance(glm::abs(ref - rec)) / std::max(Luminance(ref), floor);
	}
	brick.error = errSum / 6.0f;
}

} // namespace

bool BakeSgGiVolume(const GiTraceScene& scene, const SgGiBakeConfig& configIn,
	SgGiVolume& outVolume, SgGiBakeStats& stats, GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	SgGiBakeConfig cfg = configIn;
	cfg.raysPerProbe = std::max(cfg.raysPerProbe, 16u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);
	cfg.errorRaysPerProbe = std::max(cfg.errorRaysPerProbe, 8u);
	cfg.rootBrickSize = std::max(cfg.rootBrickSize, cfg.minBrickSize);
	cfg.maxProbes = std::max(cfg.maxProbes, 64u);
	cfg.visibilityMaxDistance = std::max(cfg.visibilityMaxDistance, 0.05f);

	auto say = [&](const char* line) {
		if (progress)
			progress(line);
	};

	// ------------------------------------------------------------------
	// Root bricks over the scene bounds
	// ------------------------------------------------------------------
	glm::vec3 bmin(1e30f), bmax(-1e30f);
	{
		glm::vec3 v0, v1, v2;
		for (uint32_t k = 0; k < scene.Bvh().TriangleCount(); ++k) {
			scene.Bvh().TriangleVertices(k, v0, v1, v2);
			bmin = glm::min(bmin, glm::min(v0, glm::min(v1, v2)));
			bmax = glm::max(bmax, glm::max(v0, glm::max(v1, v2)));
		}
		if (scene.Bvh().TriangleCount() == 0) {
			if (progress)
				progress("bake: empty scene\n");
			return false;
		}
		bmin -= cfg.rootBrickSize * 0.25f;
		bmax += cfg.rootBrickSize * 0.25f;
	}

	std::vector<BakeBrick> bricks;
	// Subdivides near geometry (distance-to-surface heuristic). One probe
	// per leaf; empty space stays coarse. The leaf budget (--max-probes)
	// forces over-budget branches to stay coarse instead of abandoning
	// them (every region keeps at least one probe; no coverage holes).
	uint32_t geometryLeaves = 0;
	auto geometryRefine = [&](auto&& self, const glm::vec3& center, float size,
			uint32_t level) -> void {
		if (bricks.size() > cfg.maxProbes * 4u + 128u)
			return; // hard safety valve (pathological inputs)
		BakeBrick b{};
		b.center = center;
		b.size = size;
		b.level = level;
		const uint32_t idx = uint32_t(bricks.size());
		bricks.push_back(b);

		if (size * 0.5f + 1e-6f < cfg.minBrickSize) {
			++geometryLeaves;
			return; // finest level reached
		}
		if (geometryLeaves >= cfg.maxProbes) {
			++geometryLeaves;
			return; // leaf budget spent: stay coarse rather than hole out
		}
		// Conservative near-surface test: lower bound on the true brick
		// distance via the center distance minus the half diagonal.
		const float halfDiag = size * 0.8660254f;
		const float dist = scene.Bvh().ClosestDistance(center) - halfDiag;
		if (dist < size * cfg.nearSurfaceFactor) {
			bricks[idx].subdivided = true;
			const float half = size * 0.5f;
			const float q = size * 0.25f;
			for (int sx = -1; sx <= 1; sx += 2)
				for (int sy = -1; sy <= 1; sy += 2)
					for (int sz = -1; sz <= 1; sz += 2)
						self(self, center + glm::vec3(sx, sy, sz) * q, half, level + 1);
		} else {
			++geometryLeaves;
		}
	};
	{
		const float rs = cfg.rootBrickSize;
		glm::vec3 lo(std::floor(bmin.x / rs), std::floor(bmin.y / rs), std::floor(bmin.z / rs));
		glm::vec3 hi(std::floor(bmax.x / rs), std::floor(bmax.y / rs), std::floor(bmax.z / rs));
		for (float x = lo.x; x <= hi.x; ++x)
			for (float y = lo.y; y <= hi.y; ++y)
				for (float z = lo.z; z <= hi.z; ++z)
					geometryRefine(geometryRefine,
						glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f) * rs, rs, 0);
	}
	stats.bricksBuilt = uint32_t(bricks.size());
	{
		char line[160];
		std::snprintf(line, sizeof(line),
			"bake: %u bricks after geometry refinement\n", stats.bricksBuilt);
		say(line);
	}

	// ------------------------------------------------------------------
	// Waves: bake leaves, subdivide high-error bricks, repeat
	// ------------------------------------------------------------------
	std::vector<uint32_t> pending;
	for (uint32_t i = 0; i < bricks.size(); ++i)
		if (!bricks[i].subdivided)
			pending.push_back(i);

	const uint32_t threadCount = cfg.threadCount ? cfg.threadCount : std::thread::hardware_concurrency();
	ThreadPool pool(threadCount ? threadCount : 1);

	// Bakes a set of leaves in parallel (deterministic per-brick seeds keep
	// results independent of task scheduling). Returns the live-probe count.
	auto bakeLeaves = [&](std::vector<uint32_t>& leaves) -> uint64_t {
		std::atomic<uint64_t> baked{ 0 };
		constexpr size_t kBatch = 512;
		for (size_t base = 0; base < leaves.size(); base += kBatch) {
			const size_t end = std::min(base + kBatch, leaves.size());
			std::vector<std::future<void>> futures;
			futures.reserve(end - base);
			for (size_t k = base; k < end; ++k) {
				const uint32_t idx = leaves[k];
				futures.push_back(pool.enqueue([&, idx] {
					const uint64_t seed = (uint64_t(cfg.seed) << 32)
						^ (uint64_t(idx) * 0x9e3779b97f4a7c15ull);
					BakeProbeBrick(scene, cfg, bricks[idx], seed);
					if (bricks[idx].hasProbe)
						++baked;
				}));
			}
			for (auto& f : futures)
				f.get();
		}
		stats.pathsTraced += uint64_t(leaves.size())
			* (cfg.raysPerProbe + uint64_t(6) * cfg.errorRaysPerProbe);
		return baked.load();
	};

	uint32_t wave = 0;
	while (!pending.empty()) {
		const uint64_t baked = bakeLeaves(pending);
		{
			char line[160];
			std::snprintf(line, sizeof(line),
				"bake: wave %u: %zu leaves, %llu probes\n",
				wave, pending.size(), (unsigned long long)baked);
			say(line);
		}

		// Subdivide high-error leaves: the parent probe is replaced by
		// (up to) 8 children, which the next wave bakes.
		std::vector<uint32_t> next;
		uint32_t liveProbes = 0;
		for (const auto& b : bricks)
			if (b.hasProbe)
				++liveProbes;
		for (uint32_t idx : pending) {
			BakeBrick& b = bricks[idx];
			if (!b.hasProbe)
				continue;
			if (b.error <= cfg.errorTolerance)
				continue;
			if (b.level + 1 >= kSgGiMaxLevels || b.size * 0.5f + 1e-6f < cfg.minBrickSize)
				continue;
			if (liveProbes + 8 > cfg.maxProbes) {
				++stats.probeBudgetHits;
				continue;
			}
			// Copy before the push_backs below: bricks reallocates and
			// would dangle the reference.
			const glm::vec3 center = b.center;
			const float size = b.size;
			const uint32_t level = b.level;
			b.subdivided = true;
			b.hasProbe = false;
			liveProbes -= 1;
			const float half = size * 0.5f;
			const float q = size * 0.25f;
			for (int sx = -1; sx <= 1; sx += 2) {
				for (int sy = -1; sy <= 1; sy += 2) {
					for (int sz = -1; sz <= 1; sz += 2) {
						BakeBrick c{};
						c.center = center + glm::vec3(sx, sy, sz) * q;
						c.size = half;
						c.level = level + 1;
						next.push_back(uint32_t(bricks.size()));
						bricks.push_back(c);
						++liveProbes;
					}
				}
			}
		}
		if (next.empty())
			break;
		// Wave budget spent: bake the children once so the freshly created
		// leaves still have probes, then stop refining.
		if (wave + 1 >= std::min(cfg.maxRefineWaves, kSgGiMaxLevels)) {
			const uint64_t finalBaked = bakeLeaves(next);
			(void)finalBaked;
			{
				char line[160];
				std::snprintf(line, sizeof(line),
					"bake: final wave: %zu leaves, %llu probes\n",
					next.size(), (unsigned long long)finalBaked);
				say(line);
			}
			break;
		}
		pending = std::move(next);
		++wave;
	}
	stats.refineWaves = wave + 1;

	// ------------------------------------------------------------------
	// Assemble the volume
	// ------------------------------------------------------------------
	uint32_t maxLevel = 0;
	uint32_t probeCount = 0;
	for (const BakeBrick& b : bricks) {
		if (b.hasProbe) {
			++probeCount;
			maxLevel = std::max(maxLevel, b.level);
		}
	}
	stats.probeCount = probeCount;

	outVolume = SgGiVolume{};
	outVolume.boundsMin = bmin;
	outVolume.boundsMax = bmax;
	outVolume.levelCount = std::min(maxLevel + 1, kSgGiMaxLevels);
	for (uint32_t lvl = 0; lvl < outVolume.levelCount; ++lvl) {
		const float brickSize = cfg.rootBrickSize / float(1u << lvl);
		// Cell = 2 bricks: a probe sphere (R = half diagonal ~= 0.87 brick)
		// always intersects <= 2 cells per axis.
		outVolume.cellSize[lvl] = brickSize * 2.0f;
	}
	outVolume.probes.clear();
	outVolume.probes.reserve(probeCount);
	for (const BakeBrick& b : bricks) {
		if (!b.hasProbe)
			continue;
		SgProbe p{};
		p.position = b.probePos;
		p.radius = b.size * 0.87f;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			p.amps[i] = b.amps[i];
		for (uint32_t k = 0; k < 6; ++k)
			p.visibility[k] = b.vis[k];
		p.level = b.level;
		p.valid = true;
		if (p.level < kSgGiMaxLevels) {
			outVolume.probes.push_back(p);
			++stats.probesPerLevel[p.level];
		}
	}
	outVolume.BuildGrids();

	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	{
		char line[160];
		std::snprintf(line, sizeof(line),
			"bake: done: %u probes in %.1fs (%.1f MiB GPU)\n",
			stats.probeCount, stats.seconds,
			double(outVolume.GpuBytes()) / (1024.0 * 1024.0));
		say(line);
	}
	return true;
}

} // namespace smvk::gi
