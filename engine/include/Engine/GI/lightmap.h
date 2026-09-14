#pragma once
// 2D SG lightmaps for STATIC geometry (companion to the dynamic-object path).
//
// Design (The Order:1886 / BakingLab lineage, see reference/BakingLab):
//  - Static meshes are split into chunks (one chunk per static mesh / merged
//    static block). Each chunk owns a 2D atlas rect and a lobe count of
//    5, 6, 9 or 12, chosen per chunk at bake time (flat chunks need fewer
//    lobes, high-curvature / high-contrast chunks get more).
//  - Lobe axes + sharpness are FIXED per lobe count (global constants, never
//    per-texel data): Fibonacci-spiral axes in Y-up engine convention with
//    the BakingLab sharpness rule  s = log(0.65)*N / (minDP - 1.0001).
//    Neighboring texels blend safely (amplitudes only, correspondence is
//    trivially preserved) and the GPU bilinearly filters amplitudes BEFORE
//    per-pixel lighting evaluation (hardware texture filtering does the
//    spatial interpolation, exactly like the 3D probe grids).
//  - Diffuse uses the Stephen Hill fitted SG-irradiance approximation and
//    specular uses the SG-warped NDF approximation (BakingLab SG.hlsl,
//    MIT-licensed; CPU mirrors here, GPU mirrors in sg_lightmap.hlsli).
//  - Dynamic meshes NEVER sample lightmaps: they sample the uniform 3D SG
//    probe grids (SgRegularVolume, 9 fixed lobes in 3D textures, artist OBB
//    + density). Static/dynamic separation is per-draw via materialId bits
//    (see push_constants.h / common.hlsli).
//
// File format: .sglm (SGLM v1, LZ4-compressed half payload, see .cpp).
// GPU layout: 9 RGBA16F 2D textures (36 half slots = 12 lobes x RGB);
// chunks with fewer lobes use the prefix subset. Chunk rects address one
// shared UV space; lightmap UVs ride in StaticVertex::texCoord2.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Renderer/mesh.h> // StaticVertex / StaticMeshUpload (runs)

namespace smvk::gi {

// ---------------------------------------------------------------------------
// Fixed per-count SG bases
// ---------------------------------------------------------------------------

inline constexpr uint32_t kLightmapMaxLobes = 12;
inline constexpr uint32_t kLightmapLayerCount = 9; // 12 lobes * RGB / 4

// Supported per-chunk lobe counts (BakingLab BakeModes SG5/SG6/SG9/SG12).
inline bool LightmapLobeCountValid(uint32_t n) {
	return n == 5 || n == 6 || n == 9 || n == 12;
}

// Sharpness per lobe count (BakingLab rule, see above).
float LightmapSharpness(uint32_t lobeCount);
// Axis for (lobeCount, lobe). Y-up Fibonacci spiral, shared CPU/GPU.
glm::vec3 LightmapAxis(uint32_t lobeCount, uint32_t lobe);

// ---------------------------------------------------------------------------
// Evaluation (CPU mirrors of sg_lightmap.hlsli; unit tested against each
// other via the bake-then-sample round trip)
// ---------------------------------------------------------------------------

// SGIrradianceFitted: Stephen Hill's fitted clamped-cosine approximation
// for one lobe (BakingLab SG.hlsl, MIT). Returns irradiance (radiance units).
glm::vec3 SgLightmapIrradianceFitted(const glm::vec3& amp, const glm::vec3& axis,
	float sharpness, const glm::vec3& n);

// WarpDistributionSG + SGInnerProduct specular for one lobe (BakingLab
// SpecularTermSGWarp core, without the visibility/Fresnel/cosine terms the
// runtime shader applies itself).
glm::vec3 SgLightmapSpecularLobe(const glm::vec3& amp, const glm::vec3& axis,
	float sharpness, const glm::vec3& n, const glm::vec3& view,
	float roughness, const glm::vec3& specAlbedo);

// Full-chunk evaluation helpers (sum over lobes).
glm::vec3 SgLightmapDiffuse(const glm::vec3* amps, uint32_t lobeCount,
	const glm::vec3& n);
glm::vec3 SgLightmapSpecular(const glm::vec3* amps, uint32_t lobeCount,
	const glm::vec3& n, const glm::vec3& view, float roughness,
	const glm::vec3& specAlbedo);

// ---------------------------------------------------------------------------
// Fitting (fixed per-count basis, diagonal projection — same production
// path as SgFitAccumulator::Solve, generalized to N lobes)
// ---------------------------------------------------------------------------

class SgLightmapFitAccumulator {
public:
	explicit SgLightmapFitAccumulator(uint32_t lobeCount);

	void AddSample(const glm::vec3& dir, const glm::vec3& radiance, float weight);
	void Solve(glm::vec3* outAmps) const; // outAmps[lobeCount]

	uint32_t lobeCount() const { return lobeCount_; }

private:
	uint32_t lobeCount_ = 9;
	float sharpness_ = 1.0f;
	glm::vec3 rhs_[kLightmapMaxLobes]{};
	uint32_t sampleCount_ = 0;
};

// ---------------------------------------------------------------------------
// Atlas types
// ---------------------------------------------------------------------------

// One chunk = one static mesh's lightmap rect. GPU mirror:
// SgLightmapChunkGpu in sg_lightmap.hlsli (16 B).
struct SgLightmapChunk {
	uint32_t lobeCount = 9;   // 5/6/9/12
	uint32_t baseLayer = 0;   // first of the 9 RGBA16F atlas textures (shared atlas: 0)
	float rectMin[2] = {};    // atlas UV rect (texel-exact, includes gutter)
	float rectMax[2] = {};
	uint32_t meshKey = 0;     // bake-side mesh index (node id at bake time)
};

class SgLightmapAtlas {
public:
	uint32_t width = 0;   // texels (shared across the 9 layers)
	uint32_t height = 0;
	std::vector<SgLightmapChunk> chunks;
	// Voxel-major texels: coeffs[(y * width + x)][lobe], lobeCount varies
	// per chunk; bake fills only inside chunk rects (gutters dilated).
	// Stored per chunk-run to allow mixed counts: flat array of
	// per-chunk payloads in chunk order.
	struct ChunkPayload {
		uint32_t width = 0, height = 0; // rect size in texels
		std::vector<glm::vec3> coeffs;  // width*height*lobeCount
	};
	std::vector<ChunkPayload> payloads;

	// Bilinear CPU sample inside a chunk rect (mirror of the hardware
	// filtered texture path; uv in chunk-local [0,1]).
	bool Sample(uint32_t chunk, const glm::vec2& uv,
		glm::vec3* outAmps) const;

	// Shelf-packs per-chunk requests into rows of atlasWidth, growing
	// height up to maxHeight; width/height land on the packed extent
	// (+1 padding texel, clamped). chunks/payloads are rebuilt: rects from
	// the pack, lobeCount/meshKey from the requests, payloads zeroed to
	// each rect. Returns false when the rects do not fit.
	struct RectRequest {
		glm::uvec2 size{ 0, 0 };
		uint32_t lobeCount = 9;
		uint32_t meshKey = 0;
	};
	bool PackRects(const std::vector<RectRequest>& requests,
		uint32_t atlasWidth, uint32_t maxHeight);

	// Expanded per-mesh draw runs, parallel to `chunks`. vertices carry
	// texCoord2 = chunk-local lightmap UV; indices are trivial (3 per
	// triangle). The runtime swaps these in via
	// Cluster::ReplaceStaticMesh when the lightmap is active, BEFORE gfx
	// registration. Empty runs = bake without geometry swap (analyze).
	std::vector<StaticMeshUpload> runs;

	// 9 RGBA16F layer payloads for GPU upload (uint16 halves, row-major).
	// Layer t texel (x,y) carries stream halves [t*4, t*4+3] of the
	// 36-half lobe-major RGB stream (h = lobe*3 + channel); chunks with
	// fewer lobes fill the prefix subset, the rest stay zero.
	void PackGpuLayers(std::vector<uint16_t> out[kLightmapLayerCount]) const;

	size_t GpuBytes() const;

	bool WriteFile(const std::string& path, std::string& err) const;
	bool ReadFromMemory(std::span<const uint8_t> bytes, std::string& err);
	static bool ReadFile(const std::string& path, SgLightmapAtlas& out, std::string& err);
};

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

struct SgLightmapTexel {
	glm::vec3 position; // world-space surface point
	glm::vec3 normal;   // world-space shading normal
	glm::vec2 uv;       // chunk-local lightmap UV in [0,1]
};

struct SgLightmapBakeConfig {
	uint32_t raysPerTexel = 128; // uniform-sphere radiance samples
	uint32_t maxBounces = 3;
	uint32_t threadCount = 0;    // 0 = hardware concurrency
	uint32_t seed = 1;
};

struct SgLightmapBakeStats {
	uint64_t texelsBaked = 0;
	uint64_t pathsTraced = 0;
	uint64_t samplesClamped = 0;
	double seconds = 0.0;
};

class GiTraceScene;
using GiBakeProgress = void (*)(const char* line);

// Bakes one chunk: traces every texel at its surface point (offset along
// the normal), fits the chunk's fixed basis with the diagonal projection,
// 8x-mean firefly clamp (same as the probe bakers).
bool BakeSgLightmapChunk(const GiTraceScene& scene, uint32_t lobeCount,
	const std::vector<SgLightmapTexel>& texels, const SgLightmapBakeConfig& config,
	SgLightmapAtlas::ChunkPayload& out, SgLightmapBakeStats& stats,
	GiBakeProgress progress = nullptr);

// UV2 chart generation (no external unwrapper): per-triangle dominant-plane
// projection, uniform-texel-density scaling, shelf-packed triangle rects
// into the mesh's atlas region. Chart rects TOUCH their neighbors: each
// rect carries its own 1..4-texel inner margin (opts.gutterTexels) that
// bilinear taps never cross, and the bake dilates values into margins, so
// there are no wasted gutter boxes between charts (dense meshes stay
// affordable). Per-triangle charts duplicate every vertex 1:1 with its
// triangle (a shared corner needs different lightmap UVs per face), so the
// output is an EXPANDED vertex/index run: vertices' texCoord2 carries the
// lightmap UV and texCoord keeps the material UV. The renderer draws
// lightmapped meshes with the expanded runs when lightmaps are active.
// Seams at every triangle (no chart merging) — correct, just less dense
// than Bakery/xatlas output; Bakery remains the high-quality alternative
// for hero assets.
struct SgLightmapChartOptions {
	uint32_t texelsPerMeter = 32; // lightmap density
	uint32_t gutterTexels = 1;    // inner margin per chart (1..4)
	uint32_t maxRectWidth = 1024;  // shelf row width for the mesh region
	uint32_t maxRectHeight = 8192; // mesh region height cap
	// Mesh-region texel budget (chart bbox waste included): big surfaces
	// at high density blow past any sane atlas, so the baker lowers the
	// density until the region fits this budget. 0 = no budget.
	uint64_t maxRectTexels = 1536ull * 1024;
};

struct SgLightmapChartResult {
	// Expanded run: 3 vertices per triangle (per-triangle charts), trivial
	// indices 0..3n. texCoord2 = lightmap UV in the mesh's atlas rect
	// (chunk-local [0,1] mapped over rectMin/rectMax), texCoord/normal/
	// tangent/color copied from the source vertex of that corner.
	std::vector<StaticVertex> vertices;
	std::vector<uint32_t> indices;
	float rectMin[2] = {};    // atlas UV rect (includes gutter)
	float rectMax[2] = {};
	uint32_t rectWidth = 0;   // rect size in texels (incl. gutter)
	uint32_t rectHeight = 0;
	// Bake points parallel to expanded triangles: world position/normal at
	// each texel center of the rect (deduplicated by texel hash).
	std::vector<SgLightmapTexel> texels;
};

bool GenerateLightmapCharts(const StaticMeshUpload& upload,
	const SgLightmapChartOptions& opts, SgLightmapChartResult& out,
	std::string& err);

} // namespace smvk::gi
