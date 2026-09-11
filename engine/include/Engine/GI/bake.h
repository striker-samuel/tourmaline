#pragma once
// GI bake pipeline: adaptive probe placement + CPU path tracing + fixed-basis
// SG fitting (see GI/sg.h for the representation).
//
// Pipeline (bake a fully static scene, no GPU required):
//   1. Collect world-space opaque triangles + materials from a cook-mode
//      Cluster (BuildTraceSceneFromCluster) and build a triangle BVH.
//   2. Cover the scene bounds with a coarse brick grid ("root bricks").
//   3. Geometry-driven refinement: recursively subdivide bricks that are
//      close to surfaces (distance-to-geometry heuristic — surfaces are
//      where indirect lighting starts to vary, the same observation behind
//      volumetric lightmaps). Far-from-geometry bricks stay coarse; open
//      empty space keeps a handful of large probes.
//   4. Spawn one probe per leaf brick, relocated into valid free space
//      (pushed away from walls / out of solid geometry).
//   5. Path trace incoming radiance at every probe (uniform sphere
//      sampling of directions, next-event estimation for the sun and
//      emissive geometry, analytic sky on misses) and fit the fixed SG
//      basis amplitudes (non-negative least squares).
//   6. Error-driven refinement: measure the probe's reconstruction error
//      at off-center test points against fresh path-traced reference
//      irradiance; subdivide high-error bricks and bake the children.
//      This is the pass that makes probe density follow LIGHTING
//      complexity (doorways, corners, sharp indirect gradients) rather
//      than geometry complexity — a blank uniformly-lit wall spends no
//      probes while a bright doorway gets very dense sampling.
//   7. Trace 6-axis visibility distances per probe (baked wall-awareness
//      for the runtime interpolation) and assemble/serialize the volume.
//
// Light transport conventions (matching the runtime renderer):
//   - Direct SUN light is EXCLUDED from the probe field at the primary
//     vertex (the runtime gets sun through the cascaded shadow maps;
//     storing it would double-count). Sun bounce (vertex >= 1) IS included.
//   - The analytic SKY (gradient, no sun disc) is included at every
//     depth: probes replace the env-IBL ambient entirely when present.
//   - Emissive static geometry is included via NEE at every vertex
//     (the runtime has no other way to light from emissive surfaces).
//   - Dynamic forward+ point/spot lights are runtime-only by design and
//     never baked.

#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/GI/probe_volume.h>
#include <Engine/GI/triangle_bvh.h>
// NOTE: must precede Rendergraph/environment.h — rendergraph.h relies on
// the includer having pulled Engine/Memory/pool.h in first.
#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/environment.h>

namespace smvk {

struct Cluster;

} // namespace smvk

namespace smvk::gi {

// ---------------------------------------------------------------------------
// Trace scene
// ---------------------------------------------------------------------------

struct GiTraceMaterial {
	glm::vec3 albedo{ 0.8f };   // baseColor * (1 - metallic): Lambertian bounce color
	glm::vec3 emissive{ 0.0f }; // HDR radiance (emissiveFactor * strength * texture)
};

// Deterministic, fast RNG (PCG32) so bakes are reproducible per probe id.
struct Pcg32 {
	uint64_t state = 0x853c49e6748fea9bull;
	uint64_t inc = 0xda3e39cb94b95bdbull;

	explicit Pcg32(uint64_t seed) {
		uint64_t z = seed + 0x9e3779b97f4a7c15ull;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		state = z;
		Next();
	}

	uint32_t Next() {
		const uint64_t old = state;
		state = old * 6364136223846793005ull + inc;
		const uint32_t xorshifted = uint32_t(((old >> 18u) ^ (old >> 22u)) >> 27u);
		const uint32_t rot = uint32_t(old >> 59u);
		return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
	}

	float Float() { return float(Next() >> 8) * (1.0f / 16777216.0f); }

	glm::vec2 Float2() { return glm::vec2(Float(), Float()); }
};

class GiTraceScene {
public:
	std::vector<glm::vec3> positions; // world-space triangle soup
	std::vector<uint32_t> indices;    // 3 per triangle
	std::vector<GiTraceMaterial> materials;
	std::vector<uint32_t> triMaterial; // per triangle

	// Environment (mirrors the runtime EnvironmentSettings semantics).
	glm::vec3 sunDirection{ 0.0f, 1.0f, 0.0f }; // TO sun (normalized)
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	float sunIntensity = 10000.0f;
	glm::vec3 skyZenith{ 0.25f, 0.45f, 0.90f };
	glm::vec3 skyHorizon{ 0.65f, 0.75f, 0.90f };
	glm::vec3 skyGround{ 0.20f, 0.18f, 0.15f };
	float skyIntensity = 600.0f;
	float skyExponent = 2.0f;

	void Finalize(); // build BVH + emissive light distribution

	const TriangleBvh& Bvh() const { return bvh_; }

	// Analytic sky gradient (CPU mirror of environment.hlsli
	// EvaluateSkyGradient). NO sun disc: the sun is handled by NEE and at
	// runtime by the cascaded shadow maps.
	glm::vec3 SkyRadiance(const glm::vec3& dir) const;

	// Incoming radiance arriving at `origin` along `dir` (probe primary
	// ray). `maxDepth` = number of diffuse bounce vertices after the
	// first hit. Direct sun is excluded at the primary vertex.
	glm::vec3 TraceIncoming(const glm::vec3& origin, const glm::vec3& dir,
		Pcg32& rng, uint32_t maxDepth) const;

	// Monte-Carlo cosine-hemisphere irradiance estimate at a point
	// (reference for the error-driven refinement).
	glm::vec3 TraceIrradiance(const glm::vec3& origin, const glm::vec3& n,
		Pcg32& rng, uint32_t sampleCount, uint32_t maxDepth) const;

	bool Occluded(const glm::vec3& from, const glm::vec3& dir, float dist) const {
		return bvh_.AnyHit(from, dir, dist);
	}

private:
	TriangleBvh bvh_;
	struct EmissiveTri {
		uint32_t tri = 0;
		float area = 0.0f;
		glm::vec3 radiance{ 0.0f };
	};
	std::vector<EmissiveTri> emissiveTris_;
	std::vector<float> emissiveCdf_; // SelectEmmissive() binary search
	float emissiveTotal_ = 0.0f;

	// One NEE sample of the emissive-light set at a surface vertex
	// (surface: BRDF term; primary vertex: raw radiance sample).
	glm::vec3 SampleEmissive(const glm::vec3& pos, const glm::vec3* n,
		const GiTraceMaterial* m, Pcg32& rng) const;
	glm::vec3 SampleSun(const glm::vec3& pos, const glm::vec3& n,
		const GiTraceMaterial& m) const;
};

struct GiSceneBuildOptions {
	EnvironmentSettings env{}; // sun + sky parameters

	// Optional average-texel colors keyed by the cluster's cook texture id
	// (MaterialValues texture slots): the GI baker tool decodes KTX2 files
	// and fills this so baked albedo/emissive/metallic include texture
	// content. Role-agnostic per-texture averages: baseColor slots tint
	// the albedo, emissive slots tint the emission, metallicRoughness
	// slots supply the effective metallic (B channel) the way the
	// runtime shader combines factor * texture.
	std::unordered_map<uint32_t, glm::vec3> texelAvg;
	bool includeMaskedCutout = true;  // alpha-tested foliage/walls as opaque
	bool skipBlended = true;          // glass/translucent never bounces
};

// Extracts world-space opaque triangles + materials from a cook-mode
// Cluster (retained uploads; see Cluster::SetCookMode). Emissive geometry
// (emissiveFactor/strength != 0) is kept and sampled as an area light.
// Non-const: global transforms are recalculated before the walk.
void BuildTraceSceneFromCluster(Cluster& cluster, const GiSceneBuildOptions& opts,
	GiTraceScene& out, std::string& err);

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

struct SgGiBakeConfig {
	uint32_t raysPerProbe = 128;        // uniform-sphere radiance samples
	uint32_t maxBounces = 3;           // diffuse path depth (vertices after the first hit)
	uint32_t errorRaysPerProbe = 32;    // reference irradiance rays per test point
	float rootBrickSize = 8.0f;        // coarsest brick (m)
	float minBrickSize = 0.25f;        // finest brick (m)
	float nearSurfaceFactor = 1.0f;    // geometry stage: subdivide while distToGeom < factor * brickSize
	float errorTolerance = 0.12f;      // relative irradiance error that triggers subdivision
	float errorFloor = 0.02f;          // luminance floor (x scene mean) in the error metric
	uint32_t maxProbes = 262144;
	uint32_t maxRefineWaves = 8;
	float probeClearanceFactor = 0.2f;  // keep probes >= factor * brickSize from surfaces
	float probeMinClearance = 0.03f;   // absolute clearance floor (m)
	float visibilityMaxDistance = 2.5f; // 6-axis obstacle distance cap (m)
	uint32_t threadCount = 0;          // 0 = hardware concurrency
	uint32_t seed = 1;
};

struct SgGiBakeStats {
	uint32_t probeCount = 0;
	uint32_t probesPerLevel[kSgGiMaxLevels] = {};
	uint32_t bricksBuilt = 0;
	uint32_t refineWaves = 0;
	uint32_t probeBudgetHits = 0; // leaves not subdivided because of the budget
	uint64_t pathsTraced = 0;
	double seconds = 0.0;
};

// Full bake: placement (geometry + error refinement), tracing, fitting,
// visibility, volume assembly. Progress callbacks are line-buffered
// stdout-friendly (called from the calling thread only).
using GiBakeProgress = void (*)(const char* line);

bool BakeSgGiVolume(const GiTraceScene& scene, const SgGiBakeConfig& config,
	SgGiVolume& outVolume, SgGiBakeStats& stats, GiBakeProgress progress = nullptr);

} // namespace smvk::gi
