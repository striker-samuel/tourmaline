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
//   5. Path trace incoming SUN-BOUNCE radiance at every probe (uniform
//      sphere sampling of directions, next-event estimation for the sun
//      at bounce vertices only; sky/emissive contribute nothing) and fit
//      the fixed SG basis amplitudes (non-negative least squares).
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
//   - The probe field is SUN-ONLY: bounced sun IS included at every
//     vertex, INCLUDING the primary hit (the light a sunlit surface
//     reflects toward the probe is one-bounce indirect at the probe).
//     At runtime the sun-bounce irradiance is ADDED on top of the
//     analytic sky IBL (see default_lit_body.hlsli). This does not
//     double-count the cascaded shadow maps: those evaluate direct sun
//     at the shaded surface itself, while probes only ever transport
//     light arriving from OTHER surfaces.
//   - The analytic SKY is NOT baked (runtime EvaluateIrradiance owns sky
//     ambient; baking the dome gradient caused a ~30x brightness-scale
//     mismatch vs the IBL it replaced).
//   - Emissive static geometry is NOT baked (its NEE 1/pdfSa blowups were
//     the bright-dot firefly source; Sponza has no meaningful emissives).
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
	// runtime by the cascaded shadow maps. RETAINED for tooling/debug
	// only: the sun-only probe field does NOT sample it.
	glm::vec3 SkyRadiance(const glm::vec3& dir) const;

	// Incoming SUN-BOUNCE radiance arriving at `origin` along `dir`
	// (probe primary ray). `maxDepth` = number of diffuse bounce vertices
	// after the first hit. Sun NEE runs at every vertex including the
	// primary hit; sky and emissive contribute nothing (sun-only field).
	glm::vec3 TraceIncoming(const glm::vec3& origin, const glm::vec3& dir,
		Pcg32& rng, uint32_t maxDepth) const;

	// Monte-Carlo cosine-hemisphere irradiance estimate at a point
	// (reference for the error-driven refinement; sun-only, matching
	// TraceIncoming).
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
	// (Lambertian reflected term, including the vertex albedo).
	// RETAINED but UNSAMPLED: the sun-only field never calls it (it was
	// the firefly source via 1/pdfSa blowups).
	glm::vec3 SampleEmissive(const glm::vec3& pos, const glm::vec3& n,
		const GiTraceMaterial& m, Pcg32& rng) const;
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
	bool includeEmissive = true;      // DEPRECATED (no-op): emissive is not
	                                  // baked in the sun-only field; kept so
	                                  // existing callers still compile.
	// White-world mode: override EVERY triangle's albedo with a single
	// known value (textures/factors/metallic ignored, emissive zeroed).
	// Removes the whole material/texture axis from the bake, so the
	// transport becomes analytically checkable (each bounce attenuates by
	// exactly uniformAlbedo) and texture-cooking bugs can't print through
	// as dots. Runtime still multiplies its local albedo on top.
	bool uniformAlbedo = false;
	glm::vec3 uniformAlbedoValue{ 0.7f };
};

// Extracts world-space opaque triangles + materials from a cook-mode
// Cluster (retained uploads; see Cluster::SetCookMode). Emissive material
// values are still extracted (albedo path needs them) but never sampled
// as lights: the field is sun-only.
// Non-const: global transforms are recalculated before the walk.
void BuildTraceSceneFromCluster(Cluster& cluster, const GiSceneBuildOptions& opts,
	GiTraceScene& out, std::string& err);

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

struct SgGiBakeConfig {
	uint32_t raysPerProbe = 256;        // uniform-sphere radiance samples
	uint32_t maxBounces = 3;           // diffuse path depth (vertices after the first hit)
	uint32_t errorRaysPerProbe = 32;    // reference irradiance rays per test point
	float rootBrickSize = 8.0f;        // coarsest brick (m)
	float minBrickSize = 0.25f;        // finest brick (m)
	float nearSurfaceFactor = 1.0f;    // geometry stage: subdivide while distToGeom < factor * brickSize
	float errorTolerance = 0.12f;      // relative irradiance error that triggers subdivision
	float errorFloor = 0.02f;          // luminance floor (x scene mean) in the error metric
	uint32_t maxProbes = 1048576;      // == SG_GI_MAX_PROBES (GPU cap)
	uint32_t maxRefineWaves = 8;
	float probeClearanceFactor = 0.2f;  // keep probes >= factor * brickSize from surfaces
	float probeMinClearance = 0.03f;   // absolute clearance floor (m)
	float visibilityMaxDistance = 2.5f; // 6-axis obstacle distance cap (m)
	float probeSupportFactor = 1.25f;  // support radius = factor * brickSize
	                                     // (>1: neighboring probes overlap,
	                                     // blends mix more probes and stay
	                                     // smooth under visibility rejects)
	// Diffusion procedure (see bake.cpp): placement relaxation +
	// radiance smoothing passes over the probe graph.
	uint32_t diffusionIterations = 0;  // repulsion passes; MUST stay 0 for
	                                     // correctness (post-bake moves invalidate
	                                     // baked radiance). Opt-in only.
	uint32_t smoothingPasses = 0;      // Laplacian passes; 0 = ship raw amps
	                                     // (smoothing without a visibility term
	                                     // leaks bright across walls). Opt-in only.
	float smoothingLambda = 0.6f;      // Laplacian step size per pass
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
	uint64_t samplesClamped = 0; // firefly-filter hits (sun-pool outliers tamed)
	double seconds = 0.0;
};

// Full bake: placement (geometry + error refinement), tracing, fitting,
// visibility, volume assembly. Progress callbacks are line-buffered
// stdout-friendly (called from the calling thread only).
using GiBakeProgress = void (*)(const char* line);

bool BakeSgGiVolume(const GiTraceScene& scene, const SgGiBakeConfig& config,
	SgGiVolume& outVolume, SgGiBakeStats& stats, GiBakeProgress progress = nullptr);

} // namespace smvk::gi
