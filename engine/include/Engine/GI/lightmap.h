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
//    + density). Static/dynamic separation is per-draw via the chunk id
//    riding the gfx draw ring (GfxDraw.misc.w; 0xFFFFFFFF = none).
//
// Ownership: the COOK bakes charts, fills StaticVertex::texCoord2 with
// the per-vertex lightmap UVs, stamps each mesh's atlas chunk id, and
// writes the .sglm. The runtime only uploads the atlas layers + chunk
// descriptors — it never mutates geometry (no swaps, no rebuilds).
//
// File format: .sglm (SGLM v1, LZ4-compressed half payload, see .cpp).
// GPU layout: 9 RGBA16F 2D textures (36 half slots = 12 lobes x RGB);
// chunks with fewer lobes use the prefix subset. Chunk rects address one
// shared UV space; lightmap UVs ride in StaticVertex::texCoord2 (filled
// at cook time).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace smvk {
struct StaticMeshUpload; // Engine/Renderer/mesh.h
}

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

// UV2 chart generation (no external unwrapper): greedy planar-chart
// unwrap (face-normal planes, region-grow within ~37deg agreement),
// uniform texel density, shelf-packed chart rects with shared inner
// margins. The result is ONE lightmap UV per ORIGINAL mesh vertex
// (no vertex duplication, no geometry replacement): each vertex is
// claimed by the first chart to touch it and maps through that
// chart's projection. Vertices shared across chart SEAMS approximate
// (the seam-side triangle samples the neighbor chart's margin — a
// mild, dilated artifact confined to seams).
//
// The COOK fills StaticVertex::texCoord2 with these UVs and stamps the
// mesh's atlas chunk; the runtime never touches geometry. Transmissive
// (Blend) meshes get no charts (they stay runtime-lit).
struct SgLightmapChartOptions {
	uint32_t texelsPerMeter = 32; // lightmap density
	uint32_t gutterTexels = 1;    // inner margin per chart (1..4)
	uint32_t maxRectWidth = 1024;  // shelf row width for the mesh region
	uint32_t maxRectHeight = 8192; // mesh region height cap
	// Mesh-region texel budget (chart bbox waste included): big surfaces
	// at high density blow past any sane atlas, so the cook lowers the
	// density until the region fits this budget. 0 = no budget.
	uint64_t maxRectTexels = 1536ull * 1024;
};

// StaticMeshUpload is forward-declared at smvk:: scope above (the gi
// namespace deliberately does NOT redeclare it — a local forward would
// shadow smvk::StaticMeshUpload inside this namespace).

struct SgLightmapChartResult {
	// One lightmap UV per ORIGINAL mesh vertex (parallel to
	// upload.vertices; the cook writes these into texCoord2).
	std::vector<glm::vec2> uv2;
	float rectMin[2] = {};    // atlas UV rect (includes margin)
	float rectMax[2] = {};
	uint32_t rectWidth = 0;   // rect size in texels (incl. margin)
	uint32_t rectHeight = 0;
	// Bake points: world position/normal at each covered texel center.
	std::vector<SgLightmapTexel> texels;
};

bool GenerateLightmapCharts(const StaticMeshUpload& upload,
	const SgLightmapChartOptions& opts, SgLightmapChartResult& out,
	std::string& err);

} // namespace smvk::gi
