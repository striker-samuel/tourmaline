// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/GI/probe_volume.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>

#include <Engine/Asset/compression.h>
#include <Engine/GI/bc6h.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/platform.h>

namespace smvk::gi {

namespace {

// v2: the probe stream gained a length field (the dense section follows
// it in the file), so old files are rejected on version — re-bake.
constexpr uint32_t kSggiVersion = 2;
constexpr uint32_t kSggiMaxCellsPerLevel = 4u * 1024u * 1024u; // 4M uint2 cells

// File layout: probe header (fixed) + LZ4 probe payload (length below) +
// dense header + LZ4 dense payload (v2 — see SggiDenseHeader).
struct SggiFileHeader {
	char magic[4] = { 'S', 'G', 'G', 'I' };
	uint32_t version = kSggiVersion;
	uint32_t probeCount = 0;
	uint32_t levelCount = 0;
	uint32_t lobeCount = kSgLobeCount;
	float sharpness = kSgSharpness;
	float boundsMin[3] = {};
	float boundsMax[3] = {};
	float cellSize[kSgGiMaxLevels] = {};
	uint32_t payloadBytes = 0; // uncompressed probe-payload size
	uint32_t compressed = 0;  // 1 = LZ4 block
	uint32_t probeCompressedBytes = 0; // compressed probe-payload size (v2;
	                                   // the dense section follows it)
};
static_assert(sizeof(SggiFileHeader) == 4 + 4 * 4 + 4 + 12 + 12 + 32 + 4 * 3);

// Dense section header (v2): the baked runtime layers, one LZ4 payload.
struct SggiDenseHeader {
	char magic[4] = { 'S', 'G', 'G', 'D' };
	uint32_t version = 1;
	uint32_t levelCount = 0;   // entries in the payload (incl. absent)
	uint32_t payloadBytes = 0; // uncompressed dense-payload size
	uint32_t compressed = 1;   // 1 = LZ4 block
	uint32_t pad = 0;
};
static_assert(sizeof(SggiDenseHeader) == 24);

// Per-level mini-header inside the dense payload: 9 BC6H lobe payloads
// (identical stride — ceil(w/4) x ceil(h/4) x d blocks x 16 B) + the R16F
// weight halves follow.
struct SggiDenseLevelDisk {
	uint32_t dims[3];
	uint32_t present;
	uint64_t lobeStride;
	uint64_t weightBytes;
};
static_assert(sizeof(SggiDenseLevelDisk) == 32);

// BC6H-UF covers [0, 65504] and the SG fit is NNLS (non-negative), so raw
// resample sums clamp into range instead of encoding sign/overflow garbage
// (a negative residual or a > 65504 sum would decode as noise).
inline uint16_t PackHalfClamped(float v) {
	return PackHalf(std::clamp(v, 0.0f, 65504.0f));
}

// Disk probe record: 88 B (pos+radius, 27 half amplitudes, 6 half
// visibility distances, valid/level flags).
struct SggiProbeDisk {
	float pos[3];
	float radius;
	uint16_t amps[3 * kSgLobeCount];
	uint16_t visibility[6];
	uint8_t valid;
	uint8_t level;
	uint16_t pad;
};
static_assert(sizeof(SggiProbeDisk) == 88);

// Partition-of-unity probe weight: C2-smooth Wendland kernel with compact
// support, zero at the support radius. Must match sggi.hlsli SgGiProbeWeight.
inline float ProbeWeight(const glm::vec3& pos, const SgProbe& p) {
	const glm::vec3 d = pos - p.position;
	const float dist2 = glm::dot(d, d);
	const float r2 = p.radius * p.radius;
	if (dist2 >= r2)
		return 0.0f;
	const float x2 = dist2 / r2;
	const float x2m = 1.0f - x2;
	return x2m * x2m * x2m;
}

// Wall-awareness weight: probe q recorded obstacle distances along the 6
// axes; a shading point beyond an obstacle is behind a wall from the
// probe's viewpoint. The recorded distance gets a SLACK (surface points
// live AT the obstacle; fp16 storage + normal offsets push them a few cm
// past it) and the rejection ramps to zero over a 25%-of-dist margin, so
// thin obstacles do not nuke open-air coverage. Combined per-axis as a
// product; points with no visibility-valid covering probe sample as
// uncovered (coverage never silently leaks, worst case a hole the
// coverage debug view flags). Must match sggi.hlsli
// SgGiVisibilityFactor.
inline float VisibilityFactor(const glm::vec3& pos, const SgProbe& p) {
	static const float kAxes[6][3] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
	};
	const float slack = std::clamp(0.35f * p.radius, 0.05f, 0.3f);
	const glm::vec3 d = pos - p.position;
	float w = 1.0f;
	for (uint32_t k = 0; k < 6; ++k) {
		const float dist = p.visibility[k];
		if (dist <= 0.0f)
			continue;
		const float along = glm::dot(d, glm::vec3(kAxes[k][0], kAxes[k][1], kAxes[k][2]));
		if (along > dist + slack) {
			w *= std::max(0.0f, 1.0f
				- (along - dist - slack) / (0.25f * dist + 0.02f));
			if (w <= 0.0f)
				return 0.0f;
		}
	}
	return w;
}

} // namespace

void SgGiVolume::BuildGrids() {
	levelGrids_.assign(levelCount, LevelGrid{});
	levelStats.assign(levelCount, LevelStats{});

	// Stable level sort (bakes emit sorted probes; loads may not rely on it).
	std::stable_sort(probes.begin(), probes.end(),
		[](const SgProbe& a, const SgProbe& b) { return a.level < b.level; });

	uint32_t levelBegin = 0;
	for (uint32_t lvl = 0; lvl < levelCount; ++lvl) {
		uint32_t levelEnd = levelBegin;
		while (levelEnd < probes.size() && probes[levelEnd].level == lvl)
			++levelEnd;
		const uint32_t first = levelBegin;
		const uint32_t count = levelEnd - levelBegin;
		levelBegin = levelEnd; // advance even when the level is dropped below
		LevelGrid& grid = levelGrids_[lvl];
		LevelStats& stats = levelStats[lvl];
		stats.probeCount = count;

		const float cs = cellSize[lvl];
		if (count == 0 || cs <= 0.0f)
			continue;

		// Level-local grid extent: probe positions padded by their radii.
		glm::vec3 gmin(1e30f), gmax(-1e30f);
		for (uint32_t i = first; i < levelEnd; ++i) {
			const SgProbe& p = probes[i];
			gmin = glm::min(gmin, p.position - p.radius);
			gmax = glm::max(gmax, p.position + p.radius);
		}
		grid.volumeMin = gmin;
		for (uint32_t a = 0; a < 3; ++a) {
			grid.dims[a] = std::max(1u, uint32_t(std::ceil((gmax[a] - gmin[a]) / cs)));
			// Hard caps: cells stay addressable and bounded even for wild
			// inputs (a runaway level is dropped rather than OOM'ing).
			if (uint64_t(grid.dims[0]) * grid.dims[1] * grid.dims[2] > kSggiMaxCellsPerLevel) {
				for (uint32_t& d : grid.dims)
					d = 1;
				grid.volumeMin = glm::vec3(0.0f);
			}
		}
		const uint32_t cellCount = grid.dims[0] * grid.dims[1] * grid.dims[2];
		stats.cellCount = cellCount;

		// Count probe refs per cell (sphere-overlapped cells, clamped).
		grid.cellStart.assign(cellCount + 1, 0);
		auto cellCoords = [&](const glm::vec3& pos, uint32_t (&out)[3]) {
			for (uint32_t a = 0; a < 3; ++a) {
				int32_t c = int32_t(std::floor((pos[a] - grid.volumeMin[a]) / cs));
				out[a] = uint32_t(std::clamp(c, 0, int32_t(grid.dims[a]) - 1));
			}
		};
		for (uint32_t i = first; i < levelEnd; ++i) {
			const SgProbe& p = probes[i];
			uint32_t lo[3], hi[3];
			cellCoords(p.position - p.radius, lo);
			cellCoords(p.position + p.radius, hi);
			for (uint32_t x = lo[0]; x <= hi[0]; ++x)
				for (uint32_t y = lo[1]; y <= hi[1]; ++y)
					for (uint32_t z = lo[2]; z <= hi[2]; ++z)
						++grid.cellStart[x + grid.dims[0] * (y + grid.dims[1] * z)];
		}
		uint32_t sum = 0;
		for (uint32_t c = 0; c < cellCount; ++c) {
			const uint32_t n = std::min(grid.cellStart[c], kSgGiMaxProbesPerCell);
			grid.cellStart[c] = sum;
			sum += n;
		}
		grid.cellStart[cellCount] = sum;
		grid.indices.assign(sum, 0);

		// Fill index lists (per-cell cap: later probes of the cell drop).
		std::vector<uint32_t> fill(cellCount, 0);
		for (uint32_t i = first; i < levelEnd; ++i) {
			const SgProbe& p = probes[i];
			uint32_t lo[3], hi[3];
			cellCoords(p.position - p.radius, lo);
			cellCoords(p.position + p.radius, hi);
			for (uint32_t x = lo[0]; x <= hi[0]; ++x) {
				for (uint32_t y = lo[1]; y <= hi[1]; ++y) {
					for (uint32_t z = lo[2]; z <= hi[2]; ++z) {
						const uint32_t cell = x + grid.dims[0] * (y + grid.dims[1] * z);
						const uint32_t cap = std::min(
							grid.cellStart[cell + 1] - grid.cellStart[cell],
							kSgGiMaxProbesPerCell);
						if (fill[cell] < cap)
							grid.indices[grid.cellStart[cell] + fill[cell]++] = i;
					}
				}
			}
		}
		stats.indexCount = sum;
		stats.maxProbesPerCell = 0;
		for (uint32_t c = 0; c < cellCount; ++c) {
			const uint32_t n = grid.cellStart[c + 1] - grid.cellStart[c];
			stats.maxProbesPerCell = std::max(stats.maxProbesPerCell, n);
		}
	}
}

bool SgGiVolume::Sample(const glm::vec3& pos, glm::vec3 outAmps[kSgLobeCount]) const {
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		outAmps[i] = glm::vec3(0.0f);
	// Sole blend: visibility-weighted partition of unity (anti-leak).
	// When every covering probe is visibility-rejected the sample
	// returns false (uncovered) — there is deliberately NO nearest-probe
	// fallback: it hides coverage holes behind leaked across-wall GI.
	// Must mirror SgGiSample in sggi.hlsli: finest-first hierarchical
	// walk with the early-out (fine probes supersede coarse once the
	// accumulated weight is dense), primary blend, no fallback.
	glm::vec3 ampsVis[kSgLobeCount] = {};
	float weightVis = 0.0f;
	for (int lvl = int(levelCount) - 1; lvl >= 0; --lvl) {
		if (lvl >= int(levelGrids_.size()))
			continue;
		const LevelGrid& grid = levelGrids_[lvl];
		const float cs = cellSize[lvl];
		if (grid.indices.empty() || cs <= 0.0f)
			continue;
		uint32_t cell[3];
		for (uint32_t a = 0; a < 3; ++a) {
			int32_t c = int32_t(std::floor((pos[a] - grid.volumeMin[a]) / cs));
			cell[a] = uint32_t(std::clamp(c, 0, int32_t(grid.dims[a]) - 1));
		}
		const uint32_t cellIdx = cell[0] + grid.dims[0] * (cell[1] + grid.dims[1] * cell[2]);
		const uint32_t begin = grid.cellStart[cellIdx];
		const uint32_t end = grid.cellStart[cellIdx + 1];
		for (uint32_t k = begin; k < end; ++k) {
			const uint32_t probeIdx = grid.indices[k];
			const SgProbe& p = probes[probeIdx];
			if (!p.valid)
				continue;
			const float w = ProbeWeight(pos, p);
			if (w <= 0.0f)
				continue;
			const float wv = w * VisibilityFactor(pos, p);
			if (wv <= 0.0f)
				continue;
			weightVis += wv;
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				ampsVis[i] += p.amps[i] * wv;
		}
		// Early-out: fine coverage is dense enough (mirror of the
		// shader's hierarchical stop; 0.5 matches R = spacing kernels).
		if (weightVis >= 0.5f)
			break;
	}
	if (weightVis >= 1e-4f) {
		const float inv = 1.0f / weightVis;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			outAmps[i] = ampsVis[i] * inv;
		return true;
	}
	// No fallback (mirrors sggi.hlsli): uncovered stays uncovered so
	// coverage holes read as holes (magenta/black) instead of leaks.
	return false;
}

bool SgGiVolume::ResampleLevelDense(uint32_t level,
	std::vector<glm::vec3>& outAmps, std::vector<float>& outWeight,
	glm::vec3& outMin, uint32_t (&outDims)[3], float& outCell) const {
	outAmps.clear();
	outWeight.clear();
	if (level >= levelCount || level >= levelGrids_.size())
		return false;
	const LevelGrid& grid = levelGrids_[level];
	const float cs = cellSize[level];
	if (grid.indices.empty() || cs <= 0.0f)
		return false;
	outMin = grid.volumeMin;
	outDims[0] = grid.dims[0];
	outDims[1] = grid.dims[1];
	outDims[2] = grid.dims[2];
	outCell = cs;
	const uint64_t n = uint64_t(outDims[0]) * outDims[1] * outDims[2];
	outAmps.assign(size_t(n) * kSgLobeCount, glm::vec3(0.0f));
	outWeight.assign(size_t(n), 0.0f);
	// Per voxel (= the lookup grid's cell): the SAME visibility-weighted
	// partition-of-unity blend Sample applies per cell, evaluated at the
	// cell center. Raw sums (NOT normalized — the runtime accumulates
	// across levels exactly like the walk: finest-first, early-out at
	// 0.5, normalize at the end).
	for (uint32_t z = 0; z < outDims[2]; ++z) {
		for (uint32_t y = 0; y < outDims[1]; ++y) {
			for (uint32_t x = 0; x < outDims[0]; ++x) {
				const uint32_t cell =
					x + outDims[0] * (y + outDims[1] * z);
				const glm::vec3 pos = outMin + glm::vec3(
					(float(x) + 0.5f) * cs,
					(float(y) + 0.5f) * cs,
					(float(z) + 0.5f) * cs);
				const uint32_t begin = grid.cellStart[cell];
				const uint32_t end = grid.cellStart[cell + 1];
				float sumW = 0.0f;
				glm::vec3 sumA[kSgLobeCount] = {};
				for (uint32_t k = begin; k < end; ++k) {
					const uint32_t probeIdx = grid.indices[k];
					if (probeIdx >= probes.size())
						continue;
					const SgProbe& p = probes[probeIdx];
					if (!p.valid)
						continue;
					const float w = ProbeWeight(pos, p);
					if (w <= 0.0f)
						continue;
					const float wv = w * VisibilityFactor(pos, p);
					if (wv <= 0.0f)
						continue;
					sumW += wv;
					for (uint32_t i = 0; i < kSgLobeCount; ++i)
						sumA[i] += p.amps[i] * wv;
				}
				outWeight[size_t(cell)] = sumW;
				for (uint32_t i = 0; i < kSgLobeCount; ++i)
					outAmps[size_t(cell) * kSgLobeCount + i] = sumA[i];
			}
		}
	}
	return true;
}

bool SgGiVolume::BakeDenseLayers() {
	denseLevels.assign(std::min<uint32_t>(levelCount, kSgGiMaxLevels),
		DenseLevel{});
	bool any = false;
	// Encode pool (see the parallel encode note below): the BC6H block
	// codec is the dense bake's dominant cost, so it gets the whole
	// machine. Determinism is unaffected (per-task purity).
	const uint32_t hwThreads = std::thread::hardware_concurrency();
	ThreadPool pool(hwThreads ? hwThreads : 1);
	for (uint32_t lvl = 0; lvl < denseLevels.size(); ++lvl) {
		std::vector<glm::vec3> amps;
		std::vector<float> weight;
		glm::vec3 mn(0.0f);
		uint32_t dims[3] = { 1, 1, 1 };
		float cs = 0.0f;
		DenseLevel& out = denseLevels[lvl];
		if (!ResampleLevelDense(lvl, amps, weight, mn, dims, cs))
			continue; // sparse/empty level — stays absent (present = false)
		out.dims[0] = dims[0];
		out.dims[1] = dims[1];
		out.dims[2] = dims[2];
		out.present = true;
		const uint32_t w = dims[0], h = dims[1], d = dims[2];
		// BC6H blocks are 4x4x1: only the ENCODER needs 4-multiple
		// staging slices (edge-replicated — the partial edge block
		// encodes padded texels; only real texels are ever sampled).
		// The stored payloads (and the runtime VkImages) keep the REAL
		// w x h x d — Vulkan allows partial edge blocks, so nothing is
		// padded in Z or in the image; blocks per level =
		// ceil(w/4) x ceil(h/4) x d.
		const uint32_t ew = (w + 3) & ~3u;
		const uint32_t eh = (h + 3) & ~3u;
		// Parallel encode: the DirectXTex BC6H reference encoder is ~1ms
		// per 4x4 block (multi-mode search) — 200k+ blocks for a typical
		// level ran ~4 MINUTES single-threaded (the dense bake dwarfed
		// the traced bake ~30x). Each (lobe, z-slice) is an independent
		// pure function of the resampled amps (its own staging slice,
		// its own output), so a thread pool preserves determinism
		// exactly: byte-identical payloads regardless of scheduling.
		// Slices assemble in z order after the join.
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe) {
			std::vector<uint8_t>& blocks = out.lobeBlocks[lobe];
			blocks.reserve(size_t(ew / 4) * (eh / 4) * 16 * d);
			std::vector<std::future<std::vector<uint8_t>>> sliceFutures;
			sliceFutures.reserve(d);
			for (uint32_t z = 0; z < d; ++z) {
				sliceFutures.push_back(pool.enqueue([&, z, lobe] {
					std::vector<uint16_t> slice(size_t(ew) * eh * 3);
					for (uint32_t y = 0; y < eh; ++y) {
						const uint32_t sy = std::min(y, h - 1);
						for (uint32_t x = 0; x < ew; ++x) {
							const uint32_t sx = std::min(x, w - 1);
							const size_t src = (size_t(z) * h + sy) * w + sx;
							const glm::vec3& a =
								amps[src * kSgLobeCount + lobe];
							const size_t dst = (size_t(y) * ew + x) * 3;
							slice[dst + 0] = PackHalfClamped(a.r);
							slice[dst + 1] = PackHalfClamped(a.g);
							slice[dst + 2] = PackHalfClamped(a.b);
						}
					}
					std::vector<uint8_t> sliceBlocks;
					Bc6hEncodeImage(slice.data(), ew, eh, sliceBlocks);
					return sliceBlocks;
				}));
			}
			for (auto& f : sliceFutures) {
				std::vector<uint8_t> sliceBlocks = f.get();
				blocks.insert(blocks.end(), sliceBlocks.begin(),
					sliceBlocks.end());
			}
		}
		// Weight volume (R16F halves, real dims, raw partition sums).
		out.weight.resize(size_t(w) * h * d);
		for (size_t i = 0; i < out.weight.size(); ++i)
			out.weight[i] = PackHalfClamped(weight[i]);
		any = true;
	}
	return any;
}

void SgGiVolume::PackGpu(std::vector<SgProbeGpu>& outProbes, SgGiGridGpu& outGrid,
	std::vector<uint32_t>& outCells, std::vector<uint32_t>& outIndices) const {
	outProbes.clear();
	outCells.clear();
	outIndices.clear();
	outProbes.reserve(probes.size());

	for (const SgProbe& p : probes) {
		SgProbeGpu g{};
		g.posRadius = glm::vec4(p.position, p.radius);
		SgProbePackAmps(g, p.amps);
		SgProbePackVis(g, p.visibility);
		g.flags = p.valid ? kSgProbeFlagValid : 0u;
		g.flags |= uint32_t(std::min(p.level, 255u)) << 8;
		outProbes.push_back(g);
	}

	outGrid = SgGiGridGpu{};
	outGrid.volumeMin_probeCount = glm::vec4(boundsMin, float(probes.size()));
	outGrid.levelCount = std::min(levelCount, kSgGiMaxLevels);
	outGrid.maxProbesPerCell = 1;
	for (uint32_t lvl = 0; lvl < outGrid.levelCount; ++lvl) {
		const LevelGrid& grid = levelGrids_[lvl];
		SgGiLevelGpu& lg = outGrid.levels[lvl];
		lg.cellSize = glm::vec4(cellSize[lvl], 0.0f, 0.0f, 0.0f);
		lg.volumeMin = glm::vec4(grid.volumeMin, 0.0f);
		lg.dims[0] = grid.dims[0];
		lg.dims[1] = grid.dims[1];
		lg.dims[2] = grid.dims[2];
		lg.cellsOffset = uint32_t(outCells.size() / 2);
		lg.indicesOffset = uint32_t(outIndices.size());
		// Probe offset: first probe of this level in the sorted array.
		uint32_t probeOffset = 0;
		while (probeOffset < probes.size() && probes[probeOffset].level < lvl)
			++probeOffset;
		lg.probeOffset = probeOffset;
		if (lvl < levelStats.size())
			outGrid.maxProbesPerCell = std::max(outGrid.maxProbesPerCell,
				levelStats[lvl].maxProbesPerCell);

		// Empty levels own one (0, 0) cell so the shader's containing-cell
		// read never aliases the next level's data.
		if (grid.cellStart.empty()) {
			outCells.push_back(0);
			outCells.push_back(0);
			continue;
		}
		for (uint32_t c = 0; c + 1 < grid.cellStart.size(); ++c) {
			const uint32_t start = grid.cellStart[c];
			const uint32_t count = grid.cellStart[c + 1] - start;
			outCells.push_back(start);
			outCells.push_back(count);
		}
		for (uint32_t k = 0; k < grid.indices.size(); ++k)
			outIndices.push_back(grid.indices[k]);
	}
}

bool SgGiVolume::WriteFile(const std::string& path, std::string& err) const {
	SggiFileHeader hdr{};
	hdr.probeCount = uint32_t(probes.size());
	hdr.levelCount = levelCount;
	for (int a = 0; a < 3; ++a) {
		hdr.boundsMin[a] = boundsMin[a];
		hdr.boundsMax[a] = boundsMax[a];
	}
	for (uint32_t l = 0; l < kSgGiMaxLevels; ++l)
		hdr.cellSize[l] = cellSize[l];

	std::vector<uint8_t> payload;
	payload.resize(probes.size() * sizeof(SggiProbeDisk));
	auto* dst = reinterpret_cast<SggiProbeDisk*>(payload.data());
	for (size_t i = 0; i < probes.size(); ++i) {
		const SgProbe& p = probes[i];
		SggiProbeDisk& d = dst[i];
		d.pos[0] = p.position.x;
		d.pos[1] = p.position.y;
		d.pos[2] = p.position.z;
		d.radius = p.radius;
		for (uint32_t l = 0; l < kSgLobeCount; ++l) {
			d.amps[l * 3 + 0] = PackHalf(p.amps[l].r);
			d.amps[l * 3 + 1] = PackHalf(p.amps[l].g);
			d.amps[l * 3 + 2] = PackHalf(p.amps[l].b);
		}
		for (uint32_t k = 0; k < 6; ++k)
			d.visibility[k] = PackHalf(p.visibility[k]);
		d.valid = p.valid ? 1 : 0;
		d.level = uint8_t(std::min(p.level, 255u));
		d.pad = 0;
	}

	std::vector<uint8_t> compressed;
	hdr.payloadBytes = uint32_t(payload.size());
	hdr.compressed = 1;
	if (!Lz4Compress(payload, compressed, 1)) {
		err = "SgGiVolume: LZ4 compression failed";
		return false;
	}
	hdr.probeCompressedBytes = uint32_t(compressed.size());

	// ---- Dense section (v2): level mini-headers + BC6H lobe payloads +
	// R16F weight halves, one LZ4 payload. Strict gate: a .sggi without
	// dense layers is useless to the runtime (the probe-list walk is
	// gone from the shader), so a write without them fails loudly here —
	// a baker that forgot BakeDenseLayers ships nothing at all, not a
	// file the app would reject at load.
	{
		bool anyDense = false;
		for (const DenseLevel& dl : denseLevels)
			anyDense = anyDense || dl.present;
		if (!anyDense) {
			err = "SgGiVolume: no dense layers baked (call BakeDenseLayers"
				" before WriteFile)";
			return false;
		}
	}
	std::vector<uint8_t> densePayload;
	const auto append = [](std::vector<uint8_t>& dst,
			const void* p, size_t n) {
		const auto* b = static_cast<const uint8_t*>(p);
		dst.insert(dst.end(), b, b + n);
	};
	SggiDenseHeader dhdr{};
	dhdr.levelCount = uint32_t(denseLevels.size());
	for (const DenseLevel& dl : denseLevels) {
		SggiDenseLevelDisk ld{};
		ld.dims[0] = dl.dims[0];
		ld.dims[1] = dl.dims[1];
		ld.dims[2] = dl.dims[2];
		ld.present = dl.present ? 1u : 0u;
		ld.lobeStride = dl.present ? dl.LobeStride() : 0;
		ld.weightBytes = dl.present
			? size_t(dl.dims[0]) * dl.dims[1] * dl.dims[2] * 2 : 0;
		append(densePayload, &ld, sizeof(ld));
		if (!dl.present)
			continue;
		for (uint32_t l = 0; l < kSgLobeCount; ++l)
			append(densePayload, dl.lobeBlocks[l].data(),
				dl.lobeBlocks[l].size());
		append(densePayload, dl.weight.data(),
			dl.weight.size() * sizeof(uint16_t));
	}
	dhdr.payloadBytes = uint32_t(densePayload.size());
	std::vector<uint8_t> denseCompressed;
	if (!Lz4Compress(densePayload, denseCompressed, 1)) {
		err = "SgGiVolume: dense LZ4 compression failed";
		return false;
	}

	std::vector<uint8_t> file(sizeof(SggiFileHeader) + compressed.size()
		+ sizeof(SggiDenseHeader) + denseCompressed.size());
	std::memcpy(file.data(), &hdr, sizeof(hdr));
	std::memcpy(file.data() + sizeof(hdr), compressed.data(), compressed.size());
	std::memcpy(file.data() + sizeof(hdr) + compressed.size(), &dhdr,
		sizeof(dhdr));
	std::memcpy(file.data() + sizeof(hdr) + compressed.size()
		+ sizeof(dhdr), denseCompressed.data(), denseCompressed.size());

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "SgGiVolume: cannot open output " + path;
		return false;
	}
	const size_t written = std::fwrite(file.data(), 1, file.size(), f);
	std::fclose(f);
	if (written != file.size()) {
		err = "SgGiVolume: short write to " + path;
		return false;
	}
	return true;
}

bool SgGiVolume::ReadFromMemory(std::span<const uint8_t> bytes, std::string& err) {
	SggiFileHeader hdr{};
	if (bytes.size() < sizeof(hdr)) {
		err = "sggi: file too small";
		return false;
	}
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	if (std::memcmp(hdr.magic, "SGGI", 4) != 0) {
		err = "sggi: bad magic";
		return false;
	}
	if (hdr.version != kSggiVersion) {
		err = "sggi: unsupported version " + std::to_string(hdr.version);
		return false;
	}
	if (hdr.lobeCount != kSgLobeCount || hdr.sharpness != kSgSharpness) {
		err = "sggi: basis mismatch (re-bake required)";
		return false;
	}
	if (hdr.levelCount > kSgGiMaxLevels || hdr.levelCount == 0) {
		err = "sggi: bad level count";
		return false;
	}

	std::vector<uint8_t> payload;
	const std::span<const uint8_t> stored(bytes.data() + sizeof(hdr), bytes.size() - sizeof(hdr));
	// v2: the probe stream has a recorded compressed length — the dense
	// section follows it. (Uncompressed writes still carry the exact
	// payload size, so the boundary is unambiguous either way.)
	const size_t probeBytes = hdr.compressed
		? size_t(hdr.probeCompressedBytes) : size_t(hdr.payloadBytes);
	if (probeBytes > stored.size()) {
		err = "sggi: probe payload overruns file";
		return false;
	}
	const std::span<const uint8_t> probeStored = stored.first(probeBytes);
	if (hdr.compressed) {
		if (!Lz4Decompress(probeStored, hdr.payloadBytes, payload)) {
			err = "sggi: LZ4 decompression failed";
			return false;
		}
	} else {
		payload.assign(probeStored.begin(), probeStored.end());
	}
	if (payload.size() != size_t(hdr.probeCount) * sizeof(SggiProbeDisk)) {
		err = "sggi: payload size mismatch";
		return false;
	}

	probes.resize(hdr.probeCount);
	const auto* src = reinterpret_cast<const SggiProbeDisk*>(payload.data());
	for (size_t i = 0; i < probes.size(); ++i) {
		const SggiProbeDisk& d = src[i];
		SgProbe& p = probes[i];
		p.position = glm::vec3(d.pos[0], d.pos[1], d.pos[2]);
		p.radius = d.radius;
		for (uint32_t l = 0; l < kSgLobeCount; ++l) {
			p.amps[l] = glm::vec3(
				UnpackHalf(d.amps[l * 3 + 0]),
				UnpackHalf(d.amps[l * 3 + 1]),
				UnpackHalf(d.amps[l * 3 + 2]));
		}
		for (uint32_t k = 0; k < 6; ++k)
			p.visibility[k] = UnpackHalf(d.visibility[k]);
		p.valid = d.valid != 0;
		p.level = d.level;
	}
	for (int a = 0; a < 3; ++a) {
		boundsMin[a] = hdr.boundsMin[a];
		boundsMax[a] = hdr.boundsMax[a];
	}
	levelCount = hdr.levelCount;
	for (uint32_t l = 0; l < kSgGiMaxLevels; ++l)
		cellSize[l] = hdr.cellSize[l];

	// ---- Dense section (v2, required): the runtime's only probe payload —
	// the shader trilinear-filters these layers (the probe-list walk is
	// gone), so a file without them is unusable for rendering (the runtime
	// rejects it loudly: re-bake). Parse + cross-check every level's
	// stride/weight sizes against the dims the runtime will compute for
	// image creation (corruption guard).
	denseLevels.clear();
	{
		const std::span<const uint8_t> rest = stored.subspan(probeBytes);
		if (rest.size() < sizeof(SggiDenseHeader)) {
			err = "sggi: dense section missing (v2 file must carry the"
				" baked runtime layers — re-bake)";
			return false;
		}
		SggiDenseHeader dhdr{};
		std::memcpy(&dhdr, rest.data(), sizeof(dhdr));
		if (std::memcmp(dhdr.magic, "SGGD", 4) != 0) {
			err = "sggi: bad dense magic";
			return false;
		}
		if (dhdr.version != 1) {
			err = "sggi: unsupported dense version "
				+ std::to_string(dhdr.version);
			return false;
		}
		if (dhdr.levelCount > kSgGiMaxLevels) {
			err = "sggi: dense level count too large";
			return false;
		}
		std::vector<uint8_t> densePayload;
		const std::span<const uint8_t> denseStored =
			rest.subspan(sizeof(dhdr));
		if (dhdr.compressed) {
			if (!Lz4Decompress(denseStored, dhdr.payloadBytes, densePayload)) {
				err = "sggi: dense LZ4 decompression failed";
				return false;
			}
		} else {
			densePayload.assign(denseStored.begin(), denseStored.end());
		}
		if (densePayload.size() != dhdr.payloadBytes) {
			err = "sggi: dense payload size mismatch";
			return false;
		}
		denseLevels.assign(dhdr.levelCount, DenseLevel{});
		size_t off = 0;
		for (uint32_t lvl = 0; lvl < dhdr.levelCount; ++lvl) {
			SggiDenseLevelDisk ld{};
			if (off + sizeof(ld) > densePayload.size()) {
				err = "sggi: dense section truncated (level header)";
				return false;
			}
			std::memcpy(&ld, densePayload.data() + off, sizeof(ld));
			off += sizeof(ld);
			DenseLevel& dl = denseLevels[lvl];
			dl.present = ld.present != 0;
			if (!dl.present)
				continue;
			dl.dims[0] = ld.dims[0];
			dl.dims[1] = ld.dims[1];
			dl.dims[2] = ld.dims[2];
			if (dl.dims[0] == 0 || dl.dims[1] == 0 || dl.dims[2] == 0
				|| ld.lobeStride != dl.LobeStride()
				|| ld.weightBytes != size_t(dl.dims[0]) * dl.dims[1]
					* dl.dims[2] * 2) {
				err = "sggi: dense level " + std::to_string(lvl)
					+ " header inconsistent (dims/strides)";
				return false;
			}
			for (uint32_t l = 0; l < kSgLobeCount; ++l) {
				if (off + ld.lobeStride > densePayload.size()) {
					err = "sggi: dense section truncated (lobe payload)";
					return false;
				}
				dl.lobeBlocks[l].assign(densePayload.data() + off,
					densePayload.data() + off + ld.lobeStride);
				off += ld.lobeStride;
			}
			if (off + ld.weightBytes > densePayload.size()) {
				err = "sggi: dense section truncated (weight payload)";
				return false;
			}
			dl.weight.resize(ld.weightBytes / 2);
			std::memcpy(dl.weight.data(), densePayload.data() + off,
				ld.weightBytes);
			off += ld.weightBytes;
		}
		if (off != densePayload.size()) {
			err = "sggi: dense section size mismatch (trailing bytes)";
			return false;
		}
	}

	BuildGrids();
	return true;
}

bool SgGiVolume::ReadFile(const std::string& path, SgGiVolume& out, std::string& err) {
	try {
		const std::vector<char> raw = readFile(path);
		const auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
		return out.ReadFromMemory(std::span<const uint8_t>(begin, raw.size()), err);
	} catch (const std::exception& e) {
		err = e.what();
		return false;
	}
}

size_t SgGiVolume::GpuBytes() const {
	// The dense layers own the runtime probe payload; the grid header is
	// the only buffer left (the probe/cell/index arrays are gone).
	size_t bytes = sizeof(SgGiGridGpu);
	for (const DenseLevel& dl : denseLevels)
		if (dl.present)
			bytes += dl.Bytes();
	return bytes;
}

} // namespace smvk::gi
