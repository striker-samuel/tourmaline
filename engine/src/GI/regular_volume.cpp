// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/GI/regular_volume.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

#include <Engine/Asset/compression.h>
#include <Engine/GI/bake.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/platform.h>

namespace smvk::gi {

namespace {

constexpr uint32_t kSgrvVersion = 1;
constexpr float kPi = 3.14159265358979323846f;

struct SgrvFileHeader {
	char magic[4] = { 'S', 'G', 'R', 'V' };
	uint32_t version = kSgrvVersion;
	uint32_t dims[3] = {};
	uint32_t lobeCount = kSgLobeCount;
	float sharpness = kSgSharpness;
	float center[3] = {};
	float axes[3][3] = {};
	float halfExtents[3] = {};
	uint32_t payloadBytes = 0; // uncompressed size
	uint32_t compressed = 0;   // 1 = LZ4 block
};
static_assert(sizeof(SgrvFileHeader) == 4 + 4 * 5 + 4 + 12 + 36 + 12 + 4 * 2);

inline float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

} // namespace

glm::vec3 SgRegularObb::LatticePos(uint32_t ix, uint32_t iy, uint32_t iz) const {
	const float u = (dims[0] > 0) ? ((float(ix) + 0.5f) / float(dims[0]) * 2.0f - 1.0f) : 0.0f;
	const float v = (dims[1] > 0) ? ((float(iy) + 0.5f) / float(dims[1]) * 2.0f - 1.0f) : 0.0f;
	const float w = (dims[2] > 0) ? ((float(iz) + 0.5f) / float(dims[2]) * 2.0f - 1.0f) : 0.0f;
	return center + axisX * (u * halfExtents.x)
		+ axisY * (v * halfExtents.y)
		+ axisZ * (w * halfExtents.z);
}

glm::vec3 SgRegularObb::WorldToLocal(const glm::vec3& p) const {
	const glm::vec3 d = p - center;
	return glm::vec3(
		glm::dot(d, axisX) / std::max(halfExtents.x, 1e-6f),
		glm::dot(d, axisY) / std::max(halfExtents.y, 1e-6f),
		glm::dot(d, axisZ) / std::max(halfExtents.z, 1e-6f));
}

bool SgRegularObb::Contains(const glm::vec3& p) const {
	const glm::vec3 l = WorldToLocal(p);
	return std::abs(l.x) <= 1.0f && std::abs(l.y) <= 1.0f && std::abs(l.z) <= 1.0f;
}

glm::vec3 SgRegularObb::VoxelSpacing() const {
	return glm::vec3(
		dims[0] > 0 ? 2.0f * halfExtents.x / float(dims[0]) : 0.0f,
		dims[1] > 0 ? 2.0f * halfExtents.y / float(dims[1]) : 0.0f,
		dims[2] > 0 ? 2.0f * halfExtents.z / float(dims[2]) : 0.0f);
}

uint64_t SgRegularVolume::VoxelCount() const {
	return uint64_t(obb.dims[0]) * obb.dims[1] * obb.dims[2];
}

void SgRegularVolume::Resize() {
	coeffs.assign(size_t(VoxelCount()) * kSgLobeCount, glm::vec3(0.0f));
}

bool SgRegularVolume::Sample(const glm::vec3& p, glm::vec3 outAmps[kSgLobeCount]) const {
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		outAmps[i] = glm::vec3(0.0f);
	if (coeffs.empty())
		return false;
	const glm::vec3 l = obb.WorldToLocal(p);
	if (std::abs(l.x) > 1.0f || std::abs(l.y) > 1.0f || std::abs(l.z) > 1.0f)
		return false;
	// Voxel-center convention matches LatticePos: voxel i spans
	// [(i/N)*2-1, ((i+1)/N)*2-1] in local coords. Grid position of the
	// sample in index space, then trilinear over the 8 neighbors with
	// clamp-to-edge (== hardware Linear + Clamp addressing).
	const float gx = (l.x * 0.5f + 0.5f) * float(obb.dims[0]) - 0.5f;
	const float gy = (l.y * 0.5f + 0.5f) * float(obb.dims[1]) - 0.5f;
	const float gz = (l.z * 0.5f + 0.5f) * float(obb.dims[2]) - 0.5f;
	const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy)), z0 = int(std::floor(gz));
	const float fx = std::clamp(gx - float(x0), 0.0f, 1.0f);
	const float fy = std::clamp(gy - float(y0), 0.0f, 1.0f);
	const float fz = std::clamp(gz - float(z0), 0.0f, 1.0f);
	const auto idx = [&](int x, int y, int z) {
		x = std::clamp(x, 0, int(obb.dims[0]) - 1);
		y = std::clamp(y, 0, int(obb.dims[1]) - 1);
		z = std::clamp(z, 0, int(obb.dims[2]) - 1);
		return size_t((uint64_t(z) * obb.dims[1] + uint64_t(y)) * obb.dims[0] + uint64_t(x));
	};
	const size_t c000 = idx(x0, y0, z0) * kSgLobeCount;
	const size_t c100 = idx(x0 + 1, y0, z0) * kSgLobeCount;
	const size_t c010 = idx(x0, y0 + 1, z0) * kSgLobeCount;
	const size_t c110 = idx(x0 + 1, y0 + 1, z0) * kSgLobeCount;
	const size_t c001 = idx(x0, y0, z0 + 1) * kSgLobeCount;
	const size_t c101 = idx(x0 + 1, y0, z0 + 1) * kSgLobeCount;
	const size_t c011 = idx(x0, y0 + 1, z0 + 1) * kSgLobeCount;
	const size_t c111 = idx(x0 + 1, y0 + 1, z0 + 1) * kSgLobeCount;
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		const glm::vec3 v00 = glm::mix(coeffs[c000 + i], coeffs[c100 + i], fx);
		const glm::vec3 v10 = glm::mix(coeffs[c010 + i], coeffs[c110 + i], fx);
		const glm::vec3 v01 = glm::mix(coeffs[c001 + i], coeffs[c101 + i], fx);
		const glm::vec3 v11 = glm::mix(coeffs[c011 + i], coeffs[c111 + i], fx);
		outAmps[i] = glm::mix(glm::mix(v00, v10, fy), glm::mix(v01, v11, fy), fz);
	}
	return true;
}

void SgRegularVolume::PackGpuLayers(std::vector<uint16_t> out[7]) const {
	const uint64_t n = VoxelCount();
	for (int t = 0; t < 7; ++t) {
		out[t].assign(size_t(n) * 4, uint16_t(0));
	}
	for (uint64_t v = 0; v < n; ++v) {
		uint16_t halves[28] = {};
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe) {
			const glm::vec3& a = coeffs[size_t(v) * kSgLobeCount + lobe];
			halves[lobe * 3 + 0] = PackHalf(a.r);
			halves[lobe * 3 + 1] = PackHalf(a.g);
			halves[lobe * 3 + 2] = PackHalf(a.b);
		}
		halves[27] = uint16_t(0);
		for (int t = 0; t < 7; ++t)
			for (int c = 0; c < 4; ++c)
				out[t][size_t(v) * 4 + c] = halves[t * 4 + c];
	}
}

size_t SgRegularVolume::GpuBytes() const {
	return size_t(VoxelCount()) * 7 * 8; // 7 RGBA16F texels
}

bool SgRegularVolume::WriteFile(const std::string& path, std::string& err) const {
	SgrvFileHeader hdr{};
	hdr.dims[0] = obb.dims[0];
	hdr.dims[1] = obb.dims[1];
	hdr.dims[2] = obb.dims[2];
	hdr.center[0] = obb.center.x;
	hdr.center[1] = obb.center.y;
	hdr.center[2] = obb.center.z;
	hdr.axes[0][0] = obb.axisX.x; hdr.axes[0][1] = obb.axisX.y; hdr.axes[0][2] = obb.axisX.z;
	hdr.axes[1][0] = obb.axisY.x; hdr.axes[1][1] = obb.axisY.y; hdr.axes[1][2] = obb.axisY.z;
	hdr.axes[2][0] = obb.axisZ.x; hdr.axes[2][1] = obb.axisZ.y; hdr.axes[2][2] = obb.axisZ.z;
	hdr.halfExtents[0] = obb.halfExtents.x;
	hdr.halfExtents[1] = obb.halfExtents.y;
	hdr.halfExtents[2] = obb.halfExtents.z;

	const uint64_t n = VoxelCount();
	std::vector<uint8_t> payload;
	payload.resize(size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t));
	auto* dst = reinterpret_cast<uint16_t*>(payload.data());
	for (uint64_t v = 0; v < n; ++v)
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe) {
			const glm::vec3& a = coeffs[size_t(v) * kSgLobeCount + lobe];
			dst[(size_t(v) * kSgLobeCount + lobe) * 3 + 0] = PackHalf(a.r);
			dst[(size_t(v) * kSgLobeCount + lobe) * 3 + 1] = PackHalf(a.g);
			dst[(size_t(v) * kSgLobeCount + lobe) * 3 + 2] = PackHalf(a.b);
		}

	std::vector<uint8_t> compressed;
	hdr.payloadBytes = uint32_t(payload.size());
	hdr.compressed = 1;
	if (!Lz4Compress(payload, compressed, 1)) {
		err = "SgRegularVolume: LZ4 compression failed";
		return false;
	}

	std::vector<uint8_t> file(sizeof(SgrvFileHeader) + compressed.size());
	std::memcpy(file.data(), &hdr, sizeof(hdr));
	std::memcpy(file.data() + sizeof(hdr), compressed.data(), compressed.size());

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "SgRegularVolume: cannot open output " + path;
		return false;
	}
	const size_t written = std::fwrite(file.data(), 1, file.size(), f);
	std::fclose(f);
	if (written != file.size()) {
		err = "SgRegularVolume: short write to " + path;
		return false;
	}
	return true;
}

bool SgRegularVolume::ReadFromMemory(std::span<const uint8_t> bytes, std::string& err) {
	SgrvFileHeader hdr{};
	if (bytes.size() < sizeof(hdr)) {
		err = "sgrv: file too small";
		return false;
	}
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	if (std::memcmp(hdr.magic, "SGRV", 4) != 0) {
		err = "sgrv: bad magic";
		return false;
	}
	if (hdr.version != kSgrvVersion) {
		err = "sgrv: unsupported version " + std::to_string(hdr.version);
		return false;
	}
	if (hdr.lobeCount != kSgLobeCount || hdr.sharpness != kSgSharpness) {
		err = "sgrv: basis mismatch (re-bake required)";
		return false;
	}
	if (hdr.dims[0] == 0 || hdr.dims[1] == 0 || hdr.dims[2] == 0
		|| uint64_t(hdr.dims[0]) * hdr.dims[1] * hdr.dims[2] > uint64_t(256) * 256 * 256) {
		err = "sgrv: bad dims";
		return false;
	}

	std::vector<uint8_t> payload;
	const std::span<const uint8_t> stored(bytes.data() + sizeof(hdr), bytes.size() - sizeof(hdr));
	if (hdr.compressed) {
		if (!Lz4Decompress(stored, hdr.payloadBytes, payload)) {
			err = "sgrv: LZ4 decompression failed";
			return false;
		}
	} else {
		payload.assign(stored.begin(), stored.end());
	}
	const uint64_t n = uint64_t(hdr.dims[0]) * hdr.dims[1] * hdr.dims[2];
	if (payload.size() != size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t)) {
		err = "sgrv: payload size mismatch";
		return false;
	}

	obb.center = glm::vec3(hdr.center[0], hdr.center[1], hdr.center[2]);
	obb.axisX = glm::vec3(hdr.axes[0][0], hdr.axes[0][1], hdr.axes[0][2]);
	obb.axisY = glm::vec3(hdr.axes[1][0], hdr.axes[1][1], hdr.axes[1][2]);
	obb.axisZ = glm::vec3(hdr.axes[2][0], hdr.axes[2][1], hdr.axes[2][2]);
	obb.halfExtents = glm::vec3(hdr.halfExtents[0], hdr.halfExtents[1], hdr.halfExtents[2]);
	obb.dims[0] = hdr.dims[0];
	obb.dims[1] = hdr.dims[1];
	obb.dims[2] = hdr.dims[2];

	coeffs.assign(size_t(n) * kSgLobeCount, glm::vec3(0.0f));
	const auto* src = reinterpret_cast<const uint16_t*>(payload.data());
	for (uint64_t v = 0; v < n; ++v)
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe) {
			coeffs[size_t(v) * kSgLobeCount + lobe] = glm::vec3(
				UnpackHalf(src[(size_t(v) * kSgLobeCount + lobe) * 3 + 0]),
				UnpackHalf(src[(size_t(v) * kSgLobeCount + lobe) * 3 + 1]),
				UnpackHalf(src[(size_t(v) * kSgLobeCount + lobe) * 3 + 2]));
		}
	return true;
}

bool SgRegularVolume::ReadFile(const std::string& path, SgRegularVolume& out, std::string& err) {
	try {
		const std::vector<char> raw = readFile(path);
		const auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
		return out.ReadFromMemory(std::span<const uint8_t>(begin, raw.size()), err);
	} catch (const std::exception& e) {
		err = e.what();
		return false;
	}
}

bool BakeSgRegularVolume(const GiTraceScene& scene, const SgRegularObb& obb,
	const SgRegularBakeConfig& configIn, SgRegularVolume& outVolume,
	SgRegularBakeStats& stats, GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	SgRegularBakeConfig cfg = configIn;
	cfg.raysPerProbe = std::max(cfg.raysPerProbe, 16u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);

	auto say = [&](const char* line) {
		if (progress)
			progress(line);
	};

	const uint64_t n = uint64_t(obb.dims[0]) * obb.dims[1] * obb.dims[2];
	if (n == 0 || n > uint64_t(256) * 256 * 256) {
		if (progress)
			progress("regular bake: bad dims\n");
		return false;
	}
	outVolume.obb = obb;
	outVolume.Resize();

	// Shared deterministic Fibonacci sphere (same rationale as the
	// adaptive baker: correlated error reads as smooth bias, not dots).
	std::vector<glm::vec3> fibDirs;
	fibDirs.reserve(cfg.raysPerProbe);
	for (uint32_t i = 0; i < cfg.raysPerProbe; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(cfg.raysPerProbe);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		fibDirs.push_back(glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r));
	}

	const uint32_t threadCount = cfg.threadCount ? cfg.threadCount : std::thread::hardware_concurrency();
	ThreadPool pool(threadCount ? threadCount : 1);
	std::atomic<uint64_t> clamped{ 0 };

	// Fixed lattice positions: NEVER relocated. Voxels inside solid
	// geometry bake whatever the tracer sees from there (padding); volume
	// boundaries — not per-voxel moves — are the leak control.
	constexpr size_t kBatch = 256;
	const float weight = 4.0f * kPi / float(cfg.raysPerProbe);
	for (uint64_t base = 0; base < n; base += kBatch) {
		const uint64_t end = std::min(base + kBatch, n);
		std::vector<std::future<void>> futures;
		futures.reserve(size_t(end - base));
		for (uint64_t v = base; v < end; ++v) {
			futures.push_back(pool.enqueue([&, v] {
				const uint32_t nx = obb.dims[0], ny = obb.dims[1];
				const uint32_t ix = uint32_t(v % nx);
				const uint32_t iy = uint32_t((v / nx) % ny);
				const uint32_t iz = uint32_t(v / (uint64_t(nx) * ny));
				const glm::vec3 pos = obb.LatticePos(ix, iy, iz);
				const uint64_t seed = (uint64_t(cfg.seed) << 32)
					^ (uint64_t(v) * 0x9e3779b97f4a7c15ull);
				Pcg32 rng(seed);
				struct RaySample { glm::vec3 dir; glm::vec3 L; };
				std::vector<RaySample> samples;
				samples.reserve(fibDirs.size());
				double meanLum = 0.0;
				for (const glm::vec3& d : fibDirs) {
					const glm::vec3 L = scene.TraceIncoming(pos, d, rng, cfg.maxBounces);
					samples.push_back({ d, L });
					meanLum += Luminance(L);
				}
				meanLum /= double(std::max<size_t>(fibDirs.size(), 1));
				const float cap = std::max(float(meanLum) * 8.0f, 1e-6f);
				SgFitAccumulator acc;
				uint64_t localClamped = 0;
				for (const RaySample& s : samples) {
					glm::vec3 L = s.L;
					const float l = Luminance(L);
					if (l > cap && l > 0.0f) {
						L *= cap / l;
						++localClamped;
					}
					acc.AddSample(s.dir, L, weight);
				}
				clamped += localClamped;
				glm::vec3 amps[kSgLobeCount];
				acc.Solve(amps); // diagonal projection (production path)
				for (uint32_t i = 0; i < kSgLobeCount; ++i)
					outVolume.coeffs[size_t(v) * kSgLobeCount + i] = amps[i];
			}));
		}
		for (auto& f : futures)
			f.get();
		if (progress && ((base / kBatch) & 63u) == 0) {
			char line[128];
			std::snprintf(line, sizeof(line), "regular bake: %llu/%llu voxels\n",
				(unsigned long long)end, (unsigned long long)n);
			say(line);
		}
	}

	// Validity padding: voxels deep inside solid geometry baked dark
	// (rays start inside walls/furniture). Trilinear filtering would
	// smear those dark holes into the room, so dilate the nearest VALID
	// voxels over them (standard volume-padding; the lattice stays
	// fixed — only the stored coefficients are filled). A voxel is
	// valid when its center clears surfaces by 5 cm.
	{
		const uint32_t nx = obb.dims[0], ny = obb.dims[1], nz = obb.dims[2];
		std::vector<char> valid(size_t(n), 0);
		for (uint64_t v = 0; v < n; ++v) {
			const uint32_t ix = uint32_t(v % nx);
			const uint32_t iy = uint32_t((v / nx) % ny);
			const uint32_t iz = uint32_t(v / (uint64_t(nx) * ny));
			valid[size_t(v)] =
				scene.Bvh().ClosestDistance(obb.LatticePos(ix, iy, iz)) >= 0.05f ? 1 : 0;
		}
		// 6-neighbour Jacobi dilation (64 passes: every solid voxel
		// takes the nearest-valid value, like texture border padding.
		// Nobody samples deep inside walls; facade-adjacent trilinear
		// taps then see facade-like values instead of black holes).
		for (int pass = 0; pass < 64; ++pass) {
			bool anyFill = false;
			std::vector<glm::vec3> fill(size_t(n) * kSgLobeCount);
			std::vector<char> fillMask(size_t(n), 0);
			static const int kDx[6] = { 1, -1, 0, 0, 0, 0 };
			static const int kDy[6] = { 0, 0, 1, -1, 0, 0 };
			static const int kDz[6] = { 0, 0, 0, 0, 1, -1 };
			for (uint32_t iz = 0; iz < nz; ++iz)
				for (uint32_t iy = 0; iy < ny; ++iy)
					for (uint32_t ix = 0; ix < nx; ++ix) {
						const uint64_t v = (uint64_t(iz) * ny + iy) * nx + ix;
						if (valid[size_t(v)])
							continue;
						glm::vec3 acc[kSgLobeCount] = {};
						int cnt = 0;
						for (int k = 0; k < 6; ++k) {
							const int jx = int(ix) + kDx[k];
							const int jy = int(iy) + kDy[k];
							const int jz = int(iz) + kDz[k];
							if (jx < 0 || jy < 0 || jz < 0
								|| jx >= int(nx) || jy >= int(ny) || jz >= int(nz))
								continue;
							const uint64_t u = (uint64_t(jz) * ny + uint64_t(jy)) * nx + uint64_t(jx);
							if (!valid[size_t(u)])
								continue;
							for (uint32_t i = 0; i < kSgLobeCount; ++i)
								acc[i] += outVolume.coeffs[size_t(u) * kSgLobeCount + i];
							++cnt;
						}
						if (cnt > 0) {
							for (uint32_t i = 0; i < kSgLobeCount; ++i)
								fill[size_t(v) * kSgLobeCount + i] = acc[i] / float(cnt);
							fillMask[size_t(v)] = 1;
							anyFill = true;
						}
					}
			for (uint64_t v = 0; v < n; ++v)
				if (fillMask[size_t(v)]) {
					for (uint32_t i = 0; i < kSgLobeCount; ++i)
						outVolume.coeffs[size_t(v) * kSgLobeCount + i] =
							fill[size_t(v) * kSgLobeCount + i];
					valid[size_t(v)] = 1;
				}
			if (!anyFill)
				break;
		}
	}

	stats.voxelsBaked = n;
	stats.pathsTraced = n * cfg.raysPerProbe;
	stats.samplesClamped = clamped.load();
	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	{
		char line[192];
		std::snprintf(line, sizeof(line),
			"regular bake: done: %llu voxels in %.1fs (%.1f MiB GPU, %llu clamped)\n",
			(unsigned long long)n, stats.seconds,
			double(outVolume.GpuBytes()) / (1024.0 * 1024.0),
			(unsigned long long)stats.samplesClamped);
		say(line);
	}
	return true;
}

} // namespace smvk::gi
