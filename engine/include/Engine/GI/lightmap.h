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
// the per-vertex CHART-LOCAL uvs (+ chartId), records each mesh's
// per-chart atlas transforms (StaticMesh::lmChartXforms), stamps each
// mesh's atlas chunk id, and writes the .sglm. The runtime uploads the
// atlas layers + the scene-wide transform table — it never mutates
// geometry (no swaps, no rebuilds). The vertex shader unpacks the final
// atlas uv as chartUv * xform.scale + xform.offset via the draw's chart
// offset (see gfxfilter.hlsli GfxLightmapUv).
//
// File format: .sglm (SGLM v1, LZ4-compressed half payload, see .cpp).
// GPU layout: 9 RGBA16F 2D textures (36 half slots = 12 lobes x RGB);
// chunks with fewer lobes use the prefix subset. Chunk rects address one
// shared UV space; chart-local UVs ride in StaticVertex::texCoord2
// (filled at cook time).

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

// BC6H texture layout: one texture per lobe (RGB = the lobe's amplitude)
// — BC6H has no alpha, so the 36-half RGBA16F stream cannot carry over
// as-is. Uncompressed stays 9 RGBA16F layers.
inline constexpr uint32_t kLightmapBc6hTextureCount = 12;

// Payload compression of the atlas (cook option; the GPU layout switches
// with it). Bc6H vs Bc6HOpt differ only in the cook-time PREFILL: opt
// replaces invalid (uncovered) texels with the per-4x4-block average of
// the valid ones before encoding, so the zeros never expand a block's
// endpoint box (Frostbite lightmap trick — the separator/gutter texels
// would otherwise cost endpoint precision on the content).
enum class SgLightmapCompression : uint8_t {
	Uncompressed = 0,
	Bc6H = 1,
	Bc6HOpt = 2,
};

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

	// Atlas-space uv for a chart-local uv: rect origin + inner mapping,
	// normalized over the atlas (what the cook bakes into texCoord2).
	glm::vec2 AtlasUv(uint32_t chunk, uint32_t margin,
		const glm::vec2& chartLocalUv) const;
	// Same mapping in TEXEL space (no normalization): the cook stamps
	// these while per-LOD appends can still grow the atlas, then divides
	// every stamped mesh once by the final dimensions (a stamp
	// normalized mid-cook would be invalidated by later appends).
	glm::vec2 AtlasTexel(uint32_t chunk, uint32_t margin,
		const glm::vec2& chartLocalUv) const;

	// 9 RGBA16F layer payloads for GPU upload (uint16 halves, row-major).
	// Layer t texel (x,y) carries stream halves [t*4, t*4+3] of the
	// 36-half lobe-major RGB stream (h = lobe*3 + channel); chunks with
	// fewer lobes fill the prefix subset, the rest stay zero.
	void PackGpuLayers(std::vector<uint16_t> out[kLightmapLayerCount]) const;

	size_t GpuBytes() const;

	// ---- BC6H compression (cook time; see SgLightmapCompression) ----
	// Compresses the payloads into kLightmapBc6hTextureCount one-per-lobe
	// BC6H images (blocks in bc6hBlocks, lobe-major). optFill switches
	// the invalid-texel prefill on. Atlas dims must be multiples of 4
	// (BC6H 4x4 blocks; the packers round the height, the width cap is a
	// power of two). Uncompressed writers/readers are unaffected until
	// this runs; WriteFile serializes the blocks (sglm v3) when
	// compression != Uncompressed.
	bool CompressToBc6h(bool optFill, std::string& err);
	SgLightmapCompression compression = SgLightmapCompression::Uncompressed;
	uint32_t bc6hLobes = 0;          // lobe images stored (<= 12)
	std::vector<uint8_t> bc6hBlocks; // lobe-major, (w/4)*(h/4)*16 B each

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

// Chart result of the xatlas chart + pack path (GenerateLightmapChartsXatlas)
// and the Frostbite-style LOD transfer (TransferLightmapCharts). The ATLAS
// packs charts (not mesh regions — a mesh owns many chart rects). Each
// vertex is claimed by the first chart to touch it; vertices shared across
// chart SEAMS approximate (the seam-side triangle samples the neighbor
// chart's margin — a mild, dilated artifact confined to seams).
//
// The COOK stamps chart-local UVs into StaticVertex::texCoord2 (+ chart
// ids) and records the per-chart atlas-space transforms
// (SgLightmapAtlas::AtlasTexel decomposed as scale + offset) on the mesh
// for the shader unpack; the lobe count rides the draw ring.
// Transmissive (Blend) meshes get no charts (they stay runtime-lit).

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
};

// ---------------------------------------------------------------------------
// Xatlas chart + pack path (external/xatlas, MIT) — the ONLY chart source.
//
// Every input mesh is fed to ONE xatlas atlas (positions pre-scaled by
// densityScale so a single texelsPerUnit means world-uniform density),
// ComputeCharts + PackCharts run with bruteForce=true ALWAYS (best
// packing, slower cook), and each xatlas chart becomes one
// SgLightmapChartResult chart with chart-local UVs + an already-packed
// atlas rect.
//
// The outputs feed the standard bake + stamp flow: chart texels rasterize
// from the ORIGINAL (unscaled) positions/normals, AtlasTexel maps
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
	// Chart density request, METERS PER TEXEL (world space, after
	// densityScale): 0.25 = one texel per 25 cm (4 texels/m). Converted
	// to xatlas's texelsPerUnit internally (floored at 2 texels/m).
	float metersPerTexel = 0.25f;
	uint32_t gutterTexels = 1;    // inner margin per chart rect (1..4)
	// Layout-only pack: run xatlas + footprint-shrink and finalize
	// rects/cornerChartUv/triChart, but leave Chart.texels empty.
	// Convergence probes use this (rects alone decide fit); the winning
	// level is rasterized once via RasterizeLightmapPackTexels before
	// the expensive GI bake. Default false preserves the old
	// pack+rasterize behavior.
	bool deferTexelRaster = false;
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
	// Packed atlas-texel rects: the chart's UV BBOX + margin (the
	// unwrapper's own parameterization density, no footprint
	// compression). Historically these were the rasterized-footprint
	// bbox + margin — the compression it induced per sparse chart was
	// the visible texel-density seam; RepackXatlasLevel owns
	// disjointness now, so the bbox rect is safe. Includes margins.
	// After RepackXatlasLevel the rects are disjoint (see below).
	std::vector<Rect> rects;
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

// Fills Chart.texels for a layout-only level produced with
// SgLightmapXatlasOptions::deferTexelRaster. Rasterizes each chart's
// triangles directly into its FINAL footprint-tight rect (no provisional
// uv shift fixup): the layout/transform is final before the expensive GI
// bake consumes the texels. Meshes must be parallel to the inputs that
// produced the level (same order/count); ok=false entries are skipped.
// Returns false with err set on input/level mismatch.
bool RasterizeLightmapPackTexels(
	const std::vector<SgLightmapXatlasMeshInput>& meshes,
	SgLightmapXatlasLevel& level, uint32_t gutterTexels, std::string& err);

// Pairwise rectangle overlap gates. The GPU packing stage has no
// ownership check: chunks copy sequentially into the same atlas pixels,
// so a later overlapping chunk destructively overwrites the earlier one.
// Intended invariant: xatlas dilated coverage disjoint -> REPACKED
// footprint-tight rects disjoint -> vertically stacked LOD levels
// disjoint -> payload copies safe. Note the footprint-shrink yields the
// tight AABB of each chart's rasterized footprint, NOT the footprint
// itself: for concave/diagonal/sparse charts the AABBs can overlap while
// the dilated coverages are disjoint (expected xatlas behavior). Call
// RepackXatlasLevel to establish rect disjointness, the level gate on
// the repacked rects BEFORE Append, and the atlas gate on chunk
// rectMin/rectMax AFTER Append, to localize any overlap to repacking vs
// LOD stacking vs later UV stamping.
// Strict rect intersection (even margin-ring touch) fails; err details
// the first overlapping pair.
bool CheckXatlasLevelRectsForOverlap(const SgLightmapXatlasLevel& level,
	std::string* err = nullptr);
bool CheckAtlasChunksForOverlap(const SgLightmapAtlas& atlas,
	std::string* err = nullptr);

// Repack outcome: Ok fits; Infeasible means the job genuinely does
// not fit (chart wider than the cap, or no placement with the packed
// height inside maxHeight) — callers lower the density / drop meshes
// and retry. InternalError means the whole sep-inflated job is under
// HALF the atlas area yet no placement was found: beyond reasonable
// doubt a packer deficiency, not a hard instance — callers must abort
// the cook loudly instead of converging density into the ground to
// route around it.
enum class RepackStatus { Ok, Infeasible, InternalError };

// Repacks a level's post-shrink footprint-tight rects into disjoint
// placements within atlasWidthCap (tallest charts first, widest first
// within a height; circular first-fit placement with a resume marker
// per height run and the 2-texel inter-rect separator; deterministic).
// Forgets the original xatlas canvas
// placement: each chart keeps its size (footprint + 2*margin) and its
// chart-local cornerChartUv, only rect.min/max move. Updates
// level.atlasWidth/atlasHeight to the packed extent (+1 sampling pad).
// Must run after GenerateLightmapChartsXatlas and before
// RasterizeLightmapPackTexels/AppendXatlasLevelToAtlas (both are
// placement-agnostic: texel uvs are rect-local, xforms derive from the
// final rects). Returns false with err set when a chart is wider than
// the cap or the packed height exceeds maxHeight.
RepackStatus RepackXatlasLevel(SgLightmapXatlasLevel& level,
	uint32_t atlasWidthCap, uint32_t maxHeight, std::string& err);

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

// Frostbite-style LOD chart sharing: the chart+bake happened ONCE on a
// REFERENCE parameterization of the surface (LOD1) and every other
// level of the same mesh (LOD0, LOD2, ...) inherits its atlas layout
// through closest-point projection: each target vertex snaps onto the
// charted reference surface, picking the projected triangle's chart
// and the barycentric interpolation of its corner uvs. Target triangles
// keep a SINGLE chart (majority of the corners, closest projection
// when all three differ) with off-chart corners re-projected RESTRICTED
// to that chart, so no triangle ever spans two atlas rects — the same
// seam-splitting the chart unwrapper itself does, and the exact
// contract StampLightmapUvs stamps per corner. Target triangles that
// only reach uncharted reference geometry (dropped/degenerate faces)
// stay 0xFFFFFFFF like every other path. out.charts copies the
// reference's (rects parallel); triChart/cornerChartUv are target-
// sized. All positions are in the SAME space (both uploads are
// node-local: siblings and base render under one node transform).
// Transfer diagnostics (optional out-param): how each target triangle
// resolved. trisDegenerate counts triangles that STILL have zero
// atlas-uv area on real geometry after the rescue (their reference
// triangle parameterizes degenerately) — the smear/black artifact
// source; everything else is healthy bookkeeping.
struct SgLightmapTransferStats {
	uint32_t trisTotal = 0;      // target triangles
	uint32_t trisUncharted = 0;  // a corner reached no charted reference
	uint32_t trisSameChart = 0;  // all corners projected into one chart
	uint32_t trisVote = 0;       // majority/nearest vote + restricted
	                             // re-projection of off-chart corners
	uint32_t trisRescued = 0;    // degenerate uv -> re-projected onto
	                             // the closest reference triangle
	uint32_t trisDegenerate = 0; // degenerate even after the rescue
};

bool TransferLightmapCharts(const StaticMeshUpload& reference,
	const SgLightmapChartResult& refCharts,
	const StaticMeshUpload& target, SgLightmapChartResult& out,
	std::string& err, SgLightmapTransferStats* stats = nullptr);

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
