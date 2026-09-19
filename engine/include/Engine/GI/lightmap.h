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
	// Diagonal projection (stable, but double-counts shared lobe energy:
	// flat/ambient fields overshoot by 1.6-2.4x depending on the count —
	// measured, see tests/TestLmSelectorDomain).
	void Solve(glm::vec3* outAmps) const; // outAmps[lobeCount]
	// Production lightmap solve: projected coordinate descent (NNLS) on
	// the basis's ANALYTIC Gram matrix, warm-started from the diagonal
	// projection. Monotone in the least-squares objective with bounded
	// updates (no unconstrained-solver blow-up class), and exact on
	// constant fields at every lobe count — the property the per-mesh
	// lobe-count selection relies on.
	void SolveGram(glm::vec3* outAmps) const;

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

// One chunk = one CHART rect (charts are the atlas packing unit — mesh
// regions are not; a mesh owns many chunks). Chunks carry no runtime GPU
// mirror: the cook bakes each rect's atlas-space transform into the
// mesh's texCoord2, and the lobe count rides the gfx draw ring, so the
// shader only needs the 9 atlas layers.
struct SgLightmapChunk {
	uint32_t lobeCount = 9;   // 5/6/9/12
	uint32_t baseLayer = 0;   // first of the 9 RGBA16F atlas textures (shared atlas: 0)
	float rectMin[2] = {};    // atlas texel rect origin (includes margin)
	float rectMax[2] = {};    // atlas texel rect end
	uint32_t meshKey = 0;     // owning node id (tool/debug)
};

class SgLightmapAtlas {
public:
	uint32_t width = 0;   // texels (shared across the 9 layers)
	uint32_t height = 0;
	std::vector<SgLightmapChunk> chunks;
	// Per-chunk payload: coeffs[(y * width + x)][lobe] in chunk-rect
	// coords; bake fills covered texels, the dilation fills margins.
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
	// each rect. Returns false when the rects do not fit maxHeight.
	struct RectRequest {
		glm::uvec2 size{ 0, 0 };
		uint32_t lobeCount = 9;
		uint32_t meshKey = 0;
	};
	bool PackRects(const std::vector<RectRequest>& requests,
		uint32_t atlasWidth, uint32_t maxHeight);

	// Atlas-space uv for a chart-local uv: rect origin + inner mapping,
	// normalized over the atlas (what the cook bakes into texCoord2).
	glm::vec2 AtlasUv(uint32_t chunk, uint32_t margin,
		const glm::vec2& chartLocalUv) const;

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
	// Per-mesh lobe-count selection (Order:1886-style): each MESH keeps
	// the smallest candidate count (5/6/9/12, up to the caller's
	// preferred count) whose fitted diffuse irradiance response degrades
	// less than lobeRelError (relative RMS) from the PREFERRED-count
	// reconstruction — both are smooth fits of the same samples, so the
	// Monte-Carlo noise of the traced ground truth cancels and the number
	// measures pure representation loss. 15% keeps hard-bounce/contrast
	// content at 9-12 (sponza: sunlit walls measure 20-35% at 9 lobes,
	// interiors 8-14%) and lets smooth fields drop to 5-6.
	bool chooseLobeCount = true;
	float lobeRelError = 0.15f;   // relative RMS degradation budget
};

struct SgLightmapBakeStats {
	uint64_t texelsBaked = 0;
	uint64_t pathsTraced = 0;
	uint64_t samplesClamped = 0;
	double seconds = 0.0;
	// Per-mesh lobe-selection diagnostics (batch path with selection on):
	// the candidate counts, each one's relative irradiance error against
	// the traced probes (Monte-Carlo noise dominated — diagnostics only)
	// and against the preferred-count reconstruction (degradation — the
	// selection criterion), plus the count the mesh kept. Empty when the
	// selection did not run.
	struct LobePick {
		uint32_t meshKey = 0;
		uint32_t chosen = 0;
		uint32_t candCount = 0;
		uint32_t candCounts[4] = {};
		double relErr[4] = {}; // vs traced probes (noisy, diagnostics)
		double degErr[4] = {}; // vs preferred-count fit (the criterion)
	};
	std::vector<LobePick> lobePicks;
};

class GiTraceScene;
using GiBakeProgress = void (*)(const char* line);

// Bakes one chunk: traces every texel at its surface point (offset along
// the normal), fits the chunk's fixed basis with the diagonal projection,
// 8x-mean firefly clamp (same as the probe bakers). Creates its own
// thread pool — fine for single-chunk callers/tests.
bool BakeSgLightmapChunk(const GiTraceScene& scene, uint32_t lobeCount,
	const std::vector<SgLightmapTexel>& texels, const SgLightmapBakeConfig& config,
	SgLightmapAtlas::ChunkPayload& out, SgLightmapBakeStats& stats,
	GiBakeProgress progress = nullptr);

// Batch variant: bakes many chunks with ONE shared thread pool (the cook
// bakes thousands of chart chunks; per-call pool churn would dominate).
// meshKey groups chunks by owning mesh for the per-mesh lobe selection;
// chunkMeta receives the chosen lobe count (it must outlive the call —
// for the cook it points into the atlas chunk table). Either absent
// falls back to the caller's fixed lobe count.
struct SgLightmapChunkBatch {
	SgLightmapAtlas::ChunkPayload* payload = nullptr;
	const std::vector<SgLightmapTexel>* texels = nullptr;
	uint32_t meshKey = 0;
	SgLightmapChunk* chunkMeta = nullptr;
};
bool BakeSgLightmapChunks(const GiTraceScene& scene, uint32_t lobeCount,
	std::span<const SgLightmapChunkBatch> batches,
	const SgLightmapBakeConfig& config, SgLightmapBakeStats& stats,
	GiBakeProgress progress = nullptr);

// UV2 chart generation (no external unwrapper): greedy planar-chart
// unwrap (face-normal planes, region-grow within ~37deg agreement),
// uniform texel density, per-chart rects with inner margins. The result
// is per-CHART data: rect sizes, per-vertex chart ownership, chart-local
// UVs, and per-chart bake texels. The ATLAS packs charts (not mesh
// regions — a mesh owns many chart rects), so no mesh can fail packing:
// every chart is small (capped). Each vertex is claimed by the first
// chart to touch it; vertices shared across chart SEAMS approximate
// (the seam-side triangle samples the neighbor chart's margin — a
// mild, dilated artifact confined to seams).
//
// The COOK converts chart-local UVs to atlas-space (SgLightmapAtlas::
// AtlasUv) and writes them into StaticVertex::texCoord2; the lobe count
// rides the draw ring. Transmissive (Blend) meshes get no charts (they
// stay runtime-lit).
struct SgLightmapChartOptions {
	// Chart density in texels per unit of the UPLOAD's position space.
	// Callers with scaled nodes (node scale != 1, e.g. Sponza's 0.008)
	// must pre-scale a world-space density by the node's world scale
	// (see PakCooker), otherwise charts come out 1/scale too dense and
	// the per-mesh/atlas budgets drop the biggest meshes.
	float texelsPerMeter = 32.0f; // lightmap density
	uint32_t gutterTexels = 1;    // inner margin per chart (1..4)
	// Chart size cap (texels): region growth stops here, so every chart
	// rect is small and atlas-packable regardless of mesh size.
	uint32_t maxChartTexels = 256u * 256;
	// Per-mesh TOTAL budget (sum of chart rect areas, checked before
	// rasterization): a mesh with thousands of chart rects produces an
	// unbounded texel count otherwise (the sponza arches hit ~20M texels
	// = GBs of bake points). Exceeding it fails the call so the cook can
	// halve the density and retry; the atlas rounds bound the scene
	// total. 0 = no budget.
	uint64_t maxChartTexelsTotal = 1536ull * 1024;
	// Authored-UV2 charts (external unwrapper output, e.g. RizomUV-packed
	// glTF TEXCOORD_1): when true and the mesh carries non-degenerate
	// StaticVertex::texCoord2, charts are the UV2 ISLANDS (uv-continuous
	// triangle groups) instead of the greedy planar unwrap: chart rect =
	// the island's uv2 bbox; chart-local uv = the authored uv2 rescaled
	// into its island box (resolution-independent, so the atlas budget
	// search re-sizes rects by density WITHOUT re-unwrapping). The
	// layout's own uv-to-world scale (sqrt(Auv/A3d)) maps texelsPerMeter
	// to texels across the [0,1] uv range. Meshes without authored uv2
	// (texCoord2 without spread) fall back to the planar path.
	bool authoredUv2 = false;
	// Authored layouts that are PRE-PACKED per mesh (RizomUV Pack +
	// normalize: the mesh's whole [0,1]^2 uv2 IS the island packing)
	// chart as ONE rect per layout instead of one per island: re-packing
	// islands individually pays ~4 gutter texels per side on every
	// island, and unwrapper-dense scenes carry millions of islands
	// (bistro: >1M islands = 37M gutter texels — over any atlas budget
	// before content). The whole-layout rect amortizes the gutter into
	// the layout's own inter-island padding; chart-local uv is the
	// authored uv2 verbatim. Requires authoredUv2.
	bool authoredOneChartPerLayout = false;
};

// StaticMeshUpload is forward-declared at smvk:: scope above (the gi
// namespace deliberately does NOT redeclare it — a local forward would
// shadow smvk::StaticMeshUpload inside this namespace).

struct SgLightmapChartResult {
	// Per chart: rect size in texels (incl. margins) and the bake points
	// (world position/normal + chart-rect-local uv per covered texel).
	struct Chart {
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<SgLightmapTexel> texels;
	};
	std::vector<Chart> charts;
	// Per ORIGINAL mesh vertex (parallel to upload.vertices): owning
	// chart index (0xFFFFFFFF = unclaimed/degenerate) and the
	// chart-local uv in the chart's inner rect, [0,1].
	// WARNING: this is the FIRST chart to touch the vertex. Where two
	// charts share a vertex (a seam corner), a per-vertex uv alone
	// makes the seam triangle interpolate between two unrelated atlas
	// rects — long streaks across the atlas. Use triChart/cornerChartUv
	// (the per-triangle view below) when stamping; it gives every
	// triangle corners inside its OWN chart.
	std::vector<uint32_t> vertexChart;
	std::vector<glm::vec2> chartLocalUv;
	// Per-triangle chart data (the seam-correct view): chart of each
	// triangle (0xFFFFFFFF = degenerate/uncharted) and per-corner
	// chart-local uv, triCount*3 in original triangle order. The cook
	// stamps uv2 per (triangle corner, chart), duplicating seam corners
	// so no triangle ever spans two atlas rects.
	std::vector<uint32_t> triChart;
	std::vector<glm::vec2> cornerChartUv; // triCount * 3, chart-local [0,1]
	// True when the charts came from authored texCoord2 islands (see
	// SgLightmapChartOptions::authoredUv2) — lets the cook log which
	// meshes used the external unwrapper vs the planar fallback.
	bool usedAuthoredUv2 = false;
};

bool GenerateLightmapCharts(const StaticMeshUpload& upload,
	const SgLightmapChartOptions& opts, SgLightmapChartResult& out,
	std::string& err);

// ---------------------------------------------------------------------------
// Xatlas brute-force chart + pack path (external/xatlas, MIT).
//
// Full-scene alternative to the planar/authored chart loop + PackRects
// shelf pack: every input mesh is fed to ONE xatlas atlas (positions
// pre-scaled by densityScale so a single texelsPerUnit means world-uniform
// density), ComputeCharts + PackCharts run with bruteForce=true ALWAYS
// (best packing, slower cook — the cook-time/quality trade this path
// exists for), and each xatlas chart becomes one SgLightmapChartResult
// chart with chart-local UVs + an already-packed atlas rect.
//
// The outputs reuse the standard bake + stamp flow unchanged: chart texels
// rasterize from the ORIGINAL (unscaled) positions/normals, AtlasUv maps
// chart-local UVs through the placed rects, and seam-corner duplication
// applies as usual. No second pack is needed (rects are final).
//
// Per-LOD atlases: call once per LOD level (LOD0 meshes, then each
// sibling level's uploads) and stack the levels into one shared
// SgLightmapAtlas via AppendXatlasLevelToAtlas — every level keeps exact
// UVs for its own topology, the runtime is untouched (texCoord2 addresses
// the shared atlas, lmChunk still carries the lobe count). Meshlets stay
// decoupled: they are (re)built once from the final stamped topology
// after ALL levels stamp, never before.
// ---------------------------------------------------------------------------

struct SgLightmapXatlasOptions {
	float texelsPerMeter = 32.0f; // world-space density (upload units after densityScale)
	uint32_t gutterTexels = 1;    // inner margin per chart rect (1..4)
};

struct SgLightmapXatlasMeshInput {
	const StaticMeshUpload* upload = nullptr;
	uint32_t meshKey = 0;      // owning node id (tool/debug, feeds chunk meshKey)
	float densityScale = 1.0f; // worldScale * curveScale: positions are
	                           // pre-scaled by this before AddMesh so one
	                           // texelsPerUnit is world-uniform
};

struct SgLightmapXatlasMeshOutput {
	bool ok = false;
	SgLightmapChartResult chart; // charts[j] parallel to rects[j]
	struct Rect {
		float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
	};
	std::vector<Rect> rects; // packed atlas-texel rects (include margins)
};

struct SgLightmapXatlasLevel {
	// Parallel to the inputs (ok=false entries hold empty charts).
	std::vector<SgLightmapXatlasMeshOutput> meshes;
	uint32_t atlasWidth = 0;  // packed extent (+1 sampling pad, like PackRects)
	uint32_t atlasHeight = 0;
};

bool GenerateLightmapChartsXatlas(
	const std::vector<SgLightmapXatlasMeshInput>& meshes,
	const SgLightmapXatlasOptions& opts, SgLightmapXatlasLevel& out,
	std::string& err);

// Stacks one xatlas level into a shared atlas at the next free row
// (2-texel inter-rect gap, same separator as PackRects). Fresh atlases
// and per-LOD appends share this helper: for a fresh build call it once
// on an empty atlas; for per-LOD levels call it per level with the same
// atlasWidthCap/maxHeight. Creates zeroed chunk/payload shells at the
// preferred lobeCount (the bake resizes to the chosen count).
// outChunkBase[i] = atlas chunk index of meshes[i]'s first chart.
// Returns false with err set on overflow (caller lowers the density or
// drops the level).
bool AppendXatlasLevelToAtlas(SgLightmapAtlas& atlas,
	const SgLightmapXatlasLevel& level, const std::vector<uint32_t>& meshKeys,
	uint32_t preferredLobes, uint32_t atlasWidthCap, uint32_t maxHeight,
	std::vector<uint32_t>& outChunkBase, std::string& err);

// Curvature-driven density scale for lightmap charting (cook side):
// estimates the mesh's SMOOTH bending rate from shared-edge dihedral
// angles — curvature = dihedral / BEND STEP, the step being the mean
// height of the two triangles over the edge (tessellation-independent;
// the edge's own length is irrelevant: cylinder seam edges run along
// the bend axis, where dihedral/length would measure wall height).
// The scale brings the worst smooth bending under
// targetDegreesPerTexel at localTexelsPerUnit. Dihedrals under 1 deg
// (flat) and over 45 deg (authored creases — density cannot fix an
// edge, chart seams own those) are excluded; the statistic is the
// area-weighted 90th percentile of the remaining edge curvatures (the
// sharpest smoothly-bending tenth of the mesh by edge-governed area).
// Returns a value in [1, maxScale]; flat or crease-only meshes return
// exactly 1.
// Rationale: uniform density starves curved surfaces (a column at ~20
// texels/m turns ~18 deg per texel — per-texel SG fits + bilinear then
// read as faceted/hard-normal shading on smooth geometry); spending the
// same atlas budget where normals turn fast fixes the banding without
// raising the total texel count.
float LightmapCurvatureScale(const StaticMeshUpload& upload,
	float localTexelsPerUnit, float targetDegreesPerTexel,
	float maxScale);

} // namespace smvk::gi
