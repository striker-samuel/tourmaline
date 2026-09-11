#include <Engine/GI/probe_volume.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <Engine/Asset/compression.h>
#include <Engine/platform.h>

namespace smvk::gi {

namespace {

constexpr uint32_t kSggiVersion = 1;
constexpr uint32_t kSggiMaxCellsPerLevel = 4u * 1024u * 1024u; // 4M uint2 cells

// File layout: header (fixed) + LZ4 payload (probe stream).
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
	uint32_t payloadBytes = 0; // uncompressed size
	uint32_t compressed = 0;  // 1 = LZ4 block
	uint32_t pad = 0;
};
static_assert(sizeof(SggiFileHeader) == 4 + 4 * 4 + 4 + 12 + 12 + 32 + 4 * 3);

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
// product; the sampler falls back to distance-only blending when every
// probe of a point is visibility-rejected (coverage never collapses,
// worst case a leak in pathological spots). Must match sggi.hlsli
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
	// Primary blend: visibility-weighted partition of unity (anti-leak).
	// Safety net: when every covering probe is visibility-rejected
	// (deep clutter), fall back to the NEAREST covering probe instead of
	// blending all of them — blending occluded probes from the far side of
	// walls leaks their dark radiance (regional dark artifacts); the
	// nearest probe is almost always the same-side "own brick" one.
	glm::vec3 ampsVis[kSgLobeCount] = {};
	float weightVis = 0.0f;
	uint32_t nearestIdx = 0;
	float nearestDist2 = std::numeric_limits<float>::infinity();
	for (uint32_t lvl = 0; lvl < levelCount && lvl < levelGrids_.size(); ++lvl) {
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
			const glm::vec3 d = pos - p.position;
			const float dist2 = glm::dot(d, d);
			if (dist2 < nearestDist2) {
				nearestDist2 = dist2;
				nearestIdx = probeIdx;
			}
			const float wv = w * VisibilityFactor(pos, p);
			if (wv <= 0.0f)
				continue;
			weightVis += wv;
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				ampsVis[i] += p.amps[i] * wv;
		}
	}
	if (weightVis >= 1e-4f) {
		const float inv = 1.0f / weightVis;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			outAmps[i] = ampsVis[i] * inv;
		return true;
	}
	if (nearestDist2 < std::numeric_limits<float>::infinity()) {
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			outAmps[i] = probes[nearestIdx].amps[i];
		return true;
	}
	return false;
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

	std::vector<uint8_t> file(sizeof(SggiFileHeader) + compressed.size());
	std::memcpy(file.data(), &hdr, sizeof(hdr));
	std::memcpy(file.data() + sizeof(hdr), compressed.data(), compressed.size());

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
	if (hdr.compressed) {
		if (!Lz4Decompress(stored, hdr.payloadBytes, payload)) {
			err = "sggi: LZ4 decompression failed";
			return false;
		}
	} else {
		payload.assign(stored.begin(), stored.end());
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
	size_t bytes = probes.size() * sizeof(SgProbeGpu) + sizeof(SgGiGridGpu);
	for (const LevelStats& s : levelStats)
		bytes += size_t(s.cellCount) * 8 + size_t(s.indexCount) * 4;
	return bytes;
}

} // namespace smvk::gi
