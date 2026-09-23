#include <Engine/GI/cubemap_volume.h>

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

constexpr uint32_t kCvolVersion = 1;
constexpr float kPi = 3.14159265358979323846f;
// Sky sentinel for capture depths: open-sky directions carry no surface
// point; reprojection keeps their direction unshifted.
constexpr float kCvSkyDepth = 1.0e6f;

struct CvolFileHeader {
	char magic[4] = { 'C', 'V', 'O', 'L' };
	uint32_t version = kCvolVersion;
	uint32_t planeCount = 0;
	uint32_t priority = 0;
	uint32_t dims[3] = {};
	uint32_t lobeCount = kSgLobeCount;
	float sharpness = kSgSharpness;
	float gridMin[3] = {};
	float spacing = 0.0f;
	uint32_t payloadBytes = 0; // uncompressed size
	uint32_t compressed = 0;  // 1 = LZ4 block
	uint32_t pad = 0;
};
static_assert(sizeof(CvolFileHeader) == 4 + 4 * 4 + 12 + 4 + 12 + 4 + 4 * 3);

inline float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

// Clip a big box by every plane; the region must close (bounded).
bool ClipBounds(const std::vector<CvPlane>& planes, glm::vec3& mn, glm::vec3& mx) {
	constexpr float kBig = 1.0e5f;
	glm::vec3 lo(-kBig), hi(kBig);	for (const CvPlane& p : planes) {
		// Current box corners; keep the inside ones plus the 12 edge /
		// plane intersections — the AABB of those points is exact for
		// box ∩ halfspace and a mild superset once several planes
		// interact (harmless: border texels bake like any other).
		glm::vec3 keepMin(1e30f), keepMax(-1e30f);
		int kept = 0;
		auto addPoint = [&](const glm::vec3& q) {
			keepMin = glm::min(keepMin, q);
			keepMax = glm::max(keepMax, q);
			++kept;
		};
		for (int c = 0; c < 8; ++c) {
			const glm::vec3 corner(
				(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
			if (glm::dot(p.normal, corner) + p.d <= 0.0f)
				addPoint(corner);
		}
		static const int kEdges[12][6] = {
			{ 0, 0, 0, 1, 0, 0 }, { 0, 0, 1, 1, 0, 1 }, { 0, 1, 0, 1, 1, 0 },
			{ 0, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 1, 0 }, { 1, 0, 0, 1, 1, 0 },
			{ 0, 0, 1, 0, 1, 1 }, { 1, 0, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 1 },
			{ 1, 0, 0, 1, 0, 1 }, { 0, 1, 0, 0, 1, 1 }, { 1, 1, 0, 1, 1, 1 },
		};
		for (int e = 0; e < 12; ++e) {
			const glm::vec3 a(
				kEdges[e][0] ? hi.x : lo.x, kEdges[e][1] ? hi.y : lo.y,
				kEdges[e][2] ? hi.z : lo.z);
			const glm::vec3 b(
				kEdges[e][3] ? hi.x : lo.x, kEdges[e][4] ? hi.y : lo.y,
				kEdges[e][5] ? hi.z : lo.z);
			const float da = glm::dot(p.normal, a) + p.d;
			const float db = glm::dot(p.normal, b) + p.d;
			if ((da > 0.0f) == (db > 0.0f))
				continue; // both in or both out
			const float t = da / (da - db);
			addPoint(glm::mix(a, b, std::clamp(t, 0.0f, 1.0f)));
		}
		if (kept == 0)
			return false; // plane cut the region empty
		lo = keepMin;
		hi = keepMax;
	}
	if (!(hi.x > lo.x && hi.y > lo.y && hi.z > lo.z))
		return false;
	// Boundedness: a face no plane ever constrained is still at the
	// horizon — the plane set does not close the region on that side.
	// (Successive clips tighten faces independently, so "still at the
	// horizon" is detectable per face.)
	const float horizon = kBig * 0.9f;
	if (lo.x <= -horizon || hi.x >= horizon || lo.y <= -horizon
		|| hi.y >= horizon || lo.z <= -horizon || hi.z >= horizon)
		return false;
	mn = lo;
	mx = hi;
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Definition
// ---------------------------------------------------------------------------

bool CvVolumeDef::Bounds(glm::vec3& mn, glm::vec3& mx) const {
	return ClipBounds(planes, mn, mx);
}

std::vector<CvPlane> CvBoxPlanes(const glm::vec3& mn, const glm::vec3& mx,
	float insideFade, float outsideFade) {
	std::vector<CvPlane> planes;
	planes.reserve(6);
	auto add = [&](const glm::vec3& n, float d) {
		CvPlane p{};
		p.normal = n;
		p.d = d;
		p.insideFade = insideFade;
		p.outsideFade = outsideFade;
		planes.push_back(p);
	};
	// Face order: -x, +x, -y, +y, -z, +z. Plane convention (inside:
	// n.P + d <= 0): -x face is n = (-1,0,0), d = mn.x (x >= mn.x);
	// +x face is n = (1,0,0), d = -mx.x (x <= mx.x), etc.
	add(glm::vec3(-1, 0, 0), mn.x);  // inside: x >= mn.x
	add(glm::vec3(1, 0, 0), -mx.x);  // inside: x <= mx.x
	add(glm::vec3(0, -1, 0), mn.y);  // inside: y >= mn.y
	add(glm::vec3(0, 1, 0), -mx.y);  // inside: y <= mx.y
	add(glm::vec3(0, 0, -1), mn.z);  // inside: z >= mn.z
	add(glm::vec3(0, 0, 1), -mx.z);  // inside: z <= mx.z
	return planes;
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

uint64_t CvVolume::VoxelCount() const {
	return uint64_t(dims[0]) * dims[1] * dims[2];
}

void CvVolume::Resize() {
	coeffs.assign(size_t(VoxelCount()) * kSgLobeCount, glm::vec3(0.0f));
}

bool CvVolume::Sample(const glm::vec3& p, glm::vec3 outAmps[kSgLobeCount]) const {
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		outAmps[i] = glm::vec3(0.0f);
	if (coeffs.empty() || spacing <= 0.0f)
		return false;
	const glm::vec3 rel = (p - gridMin) / spacing;
	if (rel.x < 0.0f || rel.y < 0.0f || rel.z < 0.0f
		|| rel.x > float(dims[0]) || rel.y > float(dims[1])
		|| rel.z > float(dims[2]))
		return false;
	// Voxel-center convention: node (i,j,k) sits at gridMin +
	// (ijk + 0.5) * spacing, so the continuous grid coordinate is
	// rel - 0.5 (hardware Linear/Clamp mirror; == SgRegularVolume::Sample).
	const float gx = rel.x - 0.5f, gy = rel.y - 0.5f, gz = rel.z - 0.5f;
	const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy)),
		z0 = int(std::floor(gz));
	const float fx = std::clamp(gx - float(x0), 0.0f, 1.0f);
	const float fy = std::clamp(gy - float(y0), 0.0f, 1.0f);
	const float fz = std::clamp(gz - float(z0), 0.0f, 1.0f);
	const auto idx = [&](int x, int y, int z) {
		x = std::clamp(x, 0, int(dims[0]) - 1);
		y = std::clamp(y, 0, int(dims[1]) - 1);
		z = std::clamp(z, 0, int(dims[2]) - 1);
		return size_t((uint64_t(z) * dims[1] + uint64_t(y)) * dims[0] + uint64_t(x));
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

void CvVolume::PackGpuLayers(std::vector<uint16_t> out[7]) const {
	const uint64_t n = VoxelCount();
	for (int t = 0; t < 7; ++t)
		out[t].assign(size_t(n) * 4, uint16_t(0));
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

size_t CvVolume::GpuBytes() const {
	return size_t(VoxelCount()) * 7 * 8;
}

bool CvVolume::WriteFile(const std::string& path, std::string& err) const {
	CvolFileHeader hdr{};
	hdr.planeCount = uint32_t(def.planes.size());
	hdr.priority = def.priority;
	hdr.dims[0] = dims[0];
	hdr.dims[1] = dims[1];
	hdr.dims[2] = dims[2];
	hdr.gridMin[0] = gridMin.x;
	hdr.gridMin[1] = gridMin.y;
	hdr.gridMin[2] = gridMin.z;
	hdr.spacing = spacing;

	std::vector<uint8_t> planeBytes(def.planes.size() * 8 * sizeof(float));
	auto* pf = reinterpret_cast<float*>(planeBytes.data());
	for (size_t i = 0; i < def.planes.size(); ++i) {
		const CvPlane& p = def.planes[i];
		pf[i * 8 + 0] = p.normal.x;
		pf[i * 8 + 1] = p.normal.y;
		pf[i * 8 + 2] = p.normal.z;
		pf[i * 8 + 3] = p.d;
		pf[i * 8 + 4] = p.insideFade;
		pf[i * 8 + 5] = p.outsideFade;
		pf[i * 8 + 6] = 0.0f;
		pf[i * 8 + 7] = 0.0f;
	}

	const uint64_t n = VoxelCount();
	std::vector<uint8_t> payload;
	payload.resize(planeBytes.size() + size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t));
	std::memcpy(payload.data(), planeBytes.data(), planeBytes.size());
	auto* dst = reinterpret_cast<uint16_t*>(payload.data() + planeBytes.size());
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
		err = "CvVolume: LZ4 compression failed";
		return false;
	}

	std::vector<uint8_t> file(sizeof(CvolFileHeader) + compressed.size());
	std::memcpy(file.data(), &hdr, sizeof(hdr));
	std::memcpy(file.data() + sizeof(hdr), compressed.data(), compressed.size());

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "CvVolume: cannot open output " + path;
		return false;
	}
	const size_t written = std::fwrite(file.data(), 1, file.size(), f);
	std::fclose(f);
	if (written != file.size()) {
		err = "CvVolume: short write to " + path;
		return false;
	}
	return true;
}

bool CvVolume::ReadFromMemory(std::span<const uint8_t> bytes, std::string& err) {
	CvolFileHeader hdr{};
	if (bytes.size() < sizeof(hdr)) {
		err = "cvol: file too small";
		return false;
	}
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	if (std::memcmp(hdr.magic, "CVOL", 4) != 0) {
		err = "cvol: bad magic";
		return false;
	}
	if (hdr.version != kCvolVersion) {
		err = "cvol: unsupported version " + std::to_string(hdr.version);
		return false;
	}
	if (hdr.lobeCount != kSgLobeCount || hdr.sharpness != kSgSharpness) {
		err = "cvol: basis mismatch (re-bake required)";
		return false;
	}
	if (hdr.planeCount == 0 || hdr.planeCount > kCvMaxPlanes) {
		err = "cvol: bad plane count";
		return false;
	}
	if (hdr.dims[0] == 0 || hdr.dims[1] == 0 || hdr.dims[2] == 0
		|| hdr.dims[0] > kCvMaxDim || hdr.dims[1] > kCvMaxDim
		|| hdr.dims[2] > kCvMaxDim) {
		err = "cvol: bad dims";
		return false;
	}
	if (hdr.spacing <= 0.0f) {
		err = "cvol: bad spacing";
		return false;
	}

	std::vector<uint8_t> payload;
	const std::span<const uint8_t> stored(bytes.data() + sizeof(hdr),
		bytes.size() - sizeof(hdr));
	if (hdr.compressed) {
		if (!Lz4Decompress(stored, hdr.payloadBytes, payload)) {
			err = "cvol: LZ4 decompression failed";
			return false;
		}
	} else {
		payload.assign(stored.begin(), stored.end());
	}

	const size_t planeBytes = size_t(hdr.planeCount) * 8 * sizeof(float);
	const uint64_t n = uint64_t(hdr.dims[0]) * hdr.dims[1] * hdr.dims[2];
	if (payload.size() != planeBytes + size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t)) {
		err = "cvol: payload size mismatch";
		return false;
	}

	def.planes.resize(hdr.planeCount);
	const auto* pf = reinterpret_cast<const float*>(payload.data());
	for (uint32_t i = 0; i < hdr.planeCount; ++i) {
		CvPlane& p = def.planes[i];
		p.normal = glm::vec3(pf[i * 8 + 0], pf[i * 8 + 1], pf[i * 8 + 2]);
		p.d = pf[i * 8 + 3];
		p.insideFade = pf[i * 8 + 4];
		p.outsideFade = pf[i * 8 + 5];
	}
	def.priority = hdr.priority;
	def.name.clear();
	gridMin = glm::vec3(hdr.gridMin[0], hdr.gridMin[1], hdr.gridMin[2]);
	spacing = hdr.spacing;
	dims[0] = hdr.dims[0];
	dims[1] = hdr.dims[1];
	dims[2] = hdr.dims[2];

	coeffs.assign(size_t(n) * kSgLobeCount, glm::vec3(0.0f));
	const auto* src = reinterpret_cast<const uint16_t*>(payload.data() + planeBytes);
	for (uint64_t v = 0; v < n; ++v)
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe)
			coeffs[size_t(v) * kSgLobeCount + lobe] = glm::vec3(
				UnpackHalf(src[(size_t(v) * kSgLobeCount + lobe) * 3 + 0]),
				UnpackHalf(src[(size_t(v) * kSgLobeCount + lobe) * 3 + 1]),
				UnpackHalf(src[(size_t(v) * kSgLobeCount + lobe) * 3 + 2]));
	return true;
}

bool CvVolume::ReadFile(const std::string& path, CvVolume& out, std::string& err) {
	try {
		const std::vector<char> raw = readFile(path);
		const auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
		return out.ReadFromMemory(std::span<const uint8_t>(begin, raw.size()), err);
	} catch (const std::exception& e) {
		err = e.what();
		return false;
	}
}

// ---------------------------------------------------------------------------
// Bake: sparse radiance+depth captures -> cubemap reprojection -> dense grid
// ---------------------------------------------------------------------------

bool BakeCvVolume(const GiTraceScene& scene, const CvVolumeDef& def,
	float spacing, const CvBakeConfig& configIn, CvVolume& outVolume,
	CvBakeStats& stats, GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	CvBakeConfig cfg = configIn;
	auto say = [&](const char* line) {
		if (progress)
			progress(line);
	};

	if (def.planes.empty() || def.planes.size() > kCvMaxPlanes) {
		say("cvol bake: bad plane count\n");
		return false;
	}
	spacing = std::max(spacing, 0.01f);
	cfg.captureDirs = std::max(cfg.captureDirs, 64u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);
	const float captureSpacing = cfg.captureSpacing > 0.0f
		? cfg.captureSpacing : 3.0f * spacing;
	const float maxRadius = cfg.maxReprojectRadius > 0.0f
		? cfg.maxReprojectRadius : 4.0f * captureSpacing;

	glm::vec3 bMin, bMax;
	if (!def.Bounds(bMin, bMax)) {
		say("cvol bake: planes do not bound a closed region\n");
		return false;
	}

	outVolume.def = def;
	outVolume.spacing = spacing;
	outVolume.gridMin = bMin;
	const glm::vec3 ext = bMax - bMin;
	for (int a = 0; a < 3; ++a) {
		outVolume.dims[a] = std::max(1u, uint32_t(std::ceil(ext[a] / spacing)));
		outVolume.dims[a] = std::min(outVolume.dims[a], kCvMaxDim);
	}
	const uint64_t n = outVolume.VoxelCount();
	if (n > uint64_t(kCvMaxDim) * kCvMaxDim * kCvMaxDim) {
		say("cvol bake: grid too dense (cap 256^3)\n");
		return false;
	}
	outVolume.Resize();
	stats.probes = n;

	// Shared equal-solid-angle direction set (same rationale as the
	// other bakers: correlated error reads as smooth bias). These are
	// the capture "cubemap" texels: uniform solid angle 4pi/N, so the
	// reprojection weights are exact.
	const uint32_t N = cfg.captureDirs;
	std::vector<glm::vec3> dirs(N);
	for (uint32_t i = 0; i < N; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(N);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		dirs[i] = glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r);
	}
	const float texelSolidAngle = 4.0f * kPi / float(N);

	// ---- Capture lattice (coarse; the reprojection amortizes these) ----
	struct Capture {
		glm::vec3 pos;
		std::vector<glm::vec3> radiance;
		std::vector<float> depth;
	};
	glm::vec3 capGridMin = bMin;
	uint32_t capDims[3] = { 1, 1, 1 };
	for (int a = 0; a < 3; ++a)
		capDims[a] = std::max(1u, uint32_t(std::ceil(ext[a] / captureSpacing)));
	const uint64_t capCount = uint64_t(capDims[0]) * capDims[1] * capDims[2];

	// NOTE the double parens: `captures(size_t(capCount))` alone parses
	// as a function declaration (most vexing parse) and the subscripts
	// below fail with C2109.
	std::vector<Capture> captures((size_t(capCount)));
	std::vector<char> capValid(size_t(capCount), 0);
	for (uint64_t c = 0; c < capCount; ++c) {
		// Validity pass first: captures inside solid geometry are skipped
		// (their neighbors cover the field; a capture 2cm inside a wall
		// would reproject wall-interior noise over the whole cell).
		// Fade band included (weight > 0): boundary captures feed the
		// doorway bleed zone.
		const uint32_t cx = uint32_t(c % capDims[0]);
		const uint32_t cy = uint32_t((c / capDims[0]) % capDims[1]);
		const uint32_t cz = uint32_t(c / (uint64_t(capDims[0]) * capDims[1]));
		const glm::vec3 pos = capGridMin
			+ (glm::vec3(cx, cy, cz) + 0.5f) * captureSpacing;
		if (def.Weight(pos) <= 0.0f)
			continue;
		if (scene.Bvh().ClosestDistance(pos) < cfg.captureMinClearance)
			continue;
		capValid[size_t(c)] = 1;
	}
	{
		const uint32_t threadCount = cfg.threadCount
			? cfg.threadCount : std::thread::hardware_concurrency();
		ThreadPool pool(threadCount ? threadCount : 1);
		std::atomic<uint64_t> clamped{ 0 };
		constexpr size_t kCapBatch = 32;
		for (uint64_t base = 0; base < capCount; base += kCapBatch) {
			const uint64_t end = std::min(base + kCapBatch, capCount);
			std::vector<std::future<void>> futures;
			futures.reserve(size_t(end - base));
			for (uint64_t c = base; c < end; ++c) {
				if (!capValid[size_t(c)])
					continue;
				// Each task writes captures[c] exactly once (the slot is
				// pre-sized above) — no shared-container mutation.
				futures.push_back(pool.enqueue([&, c] {
					const uint32_t cx = uint32_t(c % capDims[0]);
					const uint32_t cy = uint32_t((c / capDims[0]) % capDims[1]);
					const uint32_t cz =
						uint32_t(c / (uint64_t(capDims[0]) * capDims[1]));
					const glm::vec3 pos = capGridMin
						+ (glm::vec3(cx, cy, cz) + 0.5f) * captureSpacing;
					const uint64_t seed = (uint64_t(cfg.seed) << 32)
						^ (uint64_t(c) * 0x9e3779b97f4a7c15ull + 0x1234567ull);
					Pcg32 rng(seed);
					Capture& cap = captures[size_t(c)];
					cap.pos = pos;
					cap.radiance.resize(N);
					cap.depth.assign(N, kCvSkyDepth);
					for (uint32_t i = 0; i < N; ++i) {
						float hitDist = kCvSkyDepth;
						cap.radiance[i] = scene.TraceIncoming(pos, dirs[i], rng,
							cfg.maxBounces, &hitDist);
						cap.depth[i] = hitDist;
					}
					// Two-pass firefly clamp (same policy as every other
					// bake: 8x the capture's own finite-sample mean).
					double meanLum = 0.0;
					for (const glm::vec3& L : cap.radiance)
						meanLum += Luminance(L);
					meanLum /= double(N);
					const float cap8 = std::max(float(meanLum) * 8.0f, 1e-6f);
					uint64_t localClamped = 0;
					for (glm::vec3& L : cap.radiance) {
						const float l = Luminance(L);
						if (l > cap8 && l > 0.0f) {
							L *= cap8 / l;
							++localClamped;
						}
					}
					clamped += localClamped;
				}));
			}
			for (auto& f : futures)
				f.get();
		}
		stats.captures = uint64_t(std::count(capValid.begin(), capValid.end(), 1));
		stats.captureRays = stats.captures * N;
		stats.samplesClamped = clamped.load();
	}
	if (stats.captures == 0) {
		say("cvol bake: no valid capture points (volume empty or solid?)\n");
		return false;
	}
	{
		char line[192];
		std::snprintf(line, sizeof(line),
			"cvol bake: %llu captures (%.1f%% of lattice), %llu probes, "
			"captureSpacing=%.2f maxRadius=%.2f\n",
			(unsigned long long)stats.captures,
			100.0 * double(stats.captures) / double(capCount),
			(unsigned long long)n, captureSpacing, maxRadius);
		say(line);
	}

	// Capture -> lattice index for the trilinear lookups (-1 = invalid).
	std::vector<int32_t> capIndex(size_t(capCount), -1);
	for (uint64_t c = 0; c < capCount; ++c)
		if (capValid[size_t(c)])
			capIndex[size_t(c)] = int32_t(c);

	// ---- Dense probes: reproject the enclosing captures' cubemaps ----
	{
		const uint32_t threadCount = cfg.threadCount
			? cfg.threadCount : std::thread::hardware_concurrency();
		ThreadPool pool(threadCount ? threadCount : 1);
		std::atomic<uint64_t> reprojected{ 0 };
		std::atomic<uint64_t> invalid{ 0 };
		constexpr size_t kBatch = 128;
		for (uint64_t base = 0; base < n; base += kBatch) {
			const uint64_t end = std::min(base + kBatch, n);
			std::vector<std::future<void>> futures;
			futures.reserve(size_t(end - base));
			for (uint64_t v = base; v < end; ++v) {
				futures.push_back(pool.enqueue([&, v] {
					const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1];
					const uint32_t ix = uint32_t(v % nx);
					const uint32_t iy = uint32_t((v / nx) % ny);
					const uint32_t iz = uint32_t(v / (uint64_t(nx) * ny));
					const glm::vec3 pos = outVolume.gridMin
						+ (glm::vec3(ix, iy, iz) + 0.5f) * outVolume.spacing;

					SgFitAccumulator acc;
					acc.accumulateGram = false; // diagonal solve only
					if (scene.Bvh().ClosestDistance(pos) >= cfg.probeMinClearance) {
						// Trilinear weights over the capture lattice cell
						// containing the probe (clamp-to-edge like the
						// hardware path; renormalized when captures in
						// the cell are invalid so the field keeps its
						// energy near skipped captures).
						const glm::vec3 rel = (pos - capGridMin) / captureSpacing
							- 0.5f;
						const int c0[3] = { int(std::floor(rel.x)),
							int(std::floor(rel.y)), int(std::floor(rel.z)) };
						const float fr[3] = {
							std::clamp(rel.x - float(c0[0]), 0.0f, 1.0f),
							std::clamp(rel.y - float(c0[1]), 0.0f, 1.0f),
							std::clamp(rel.z - float(c0[2]), 0.0f, 1.0f),
						};
						struct Ref { int32_t idx; float w; };
						Ref refs[8];
						int refCount = 0;
						float wSum = 0.0f;
						for (int k = 0; k < 8; ++k) {
							const int cx = c0[0] + (k & 1);
							const int cy = c0[1] + ((k >> 1) & 1);
							const int cz = c0[2] + ((k >> 2) & 1);
							if (cx < 0 || cy < 0 || cz < 0
								|| cx >= int(capDims[0]) || cy >= int(capDims[1])
								|| cz >= int(capDims[2]))
								continue;
							const int32_t cap = capIndex[
								size_t((uint64_t(cz) * capDims[1] + cy) * capDims[0] + cx)];
							if (cap < 0)
								continue;
							const float w = (k & 1 ? fr[0] : 1.0f - fr[0])
								* (((k >> 1) & 1) ? fr[1] : 1.0f - fr[1])
								* (((k >> 2) & 1) ? fr[2] : 1.0f - fr[2]);
							if (w <= 1e-6f)
								continue;
							refs[refCount].idx = cap;
							refs[refCount].w = w;
							++refCount;
							wSum += w;
						}
						if (wSum > 1e-4f) {
							const float invW = 1.0f / wSum;
							uint64_t localReproj = 0;
							for (int r = 0; r < refCount; ++r) {
								const Capture& cap = captures[size_t(refs[r].idx)];
								const float wCap = refs[r].w * invW
									* texelSolidAngle;
								const glm::vec3 toCap = cap.pos - pos;
								const float capDist2 = glm::dot(toCap, toCap);
								for (uint32_t i = 0; i < N; ++i) {
									const glm::vec3& d = dirs[i];
									const float z = cap.depth[i];
									glm::vec3 dReproj = d;
									if (z < kCvSkyDepth * 0.5f) {
										// Parallax shift: the surface point
										// the capture recorded along d,
										// seen from the probe. Sky texels
										// keep their direction (infinite
										// depth, no shift).
										const glm::vec3 surf = cap.pos + d * z;
										const glm::vec3 disp = surf - pos;
										const float dist2 = glm::dot(disp, disp);
										if (dist2 > maxRadius * maxRadius
											|| dist2 < 1e-8f)
											continue;
										dReproj = disp * (1.0f / std::sqrt(dist2));
										// Depth gate: the recorded surface
										// between capture and probe means
										// the true visible surface from
										// the probe is elsewhere — the
										// capture's own near geometry
										// shadows it (cheap BO3-style
										// visibility rejection).
										if (z * z < capDist2 * 0.25f)
											continue;
									}
									acc.AddSample(dReproj, cap.radiance[i], wCap);
									++localReproj;
								}
							}
							reprojected += localReproj;
						}
					} else {
						++invalid;
					}
					glm::vec3 amps[kSgLobeCount];
					acc.Solve(amps);
					for (uint32_t i = 0; i < kSgLobeCount; ++i)
						outVolume.coeffs[size_t(v) * kSgLobeCount + i] = amps[i];
				}));
			}
			for (auto& f : futures)
				f.get();
			if (progress && ((base / kBatch) & 15u) == 0) {
				char line[128];
				std::snprintf(line, sizeof(line),
					"cvol bake: %llu/%llu probes\n",
					(unsigned long long)end, (unsigned long long)n);
				say(line);
			}
		}
		stats.reprojected = reprojected.load();
		stats.probesInvalid = invalid.load();
	}

	// ---- Validity dilation (probes inside solid geometry; the same
	// Jacobi scheme as the regular-volume bake: nearest-valid values
	// spread over the holes so facade-adjacent trilinear taps see
	// facade-like lighting instead of black).
	{
		const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1],
			nz = outVolume.dims[2];
		std::vector<char> valid(size_t(n), 0);
		for (uint64_t v = 0; v < n; ++v) {
			const uint32_t ix = uint32_t(v % nx);
			const uint32_t iy = uint32_t((v / nx) % ny);
			const uint32_t iz = uint32_t(v / (uint64_t(nx) * ny));
			valid[size_t(v)] = scene.Bvh().ClosestDistance(
				outVolume.gridMin + (glm::vec3(ix, iy, iz) + 0.5f) * spacing)
				>= cfg.probeMinClearance ? 1 : 0;
		}
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
							if (jx < 0 || jy < 0 || jz < 0 || jx >= int(nx)
								|| jy >= int(ny) || jz >= int(nz))
								continue;
							const uint64_t u =
								(uint64_t(jz) * ny + uint64_t(jy)) * nx + uint64_t(jx);
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

	// ---- Volume smoothing (BO3 "volume smoothing"): gentle Laplacian
	// over VALID voxels only — invalid ones just dilated, smoothing them
	// against their source would undo the padding. The reprojection is
	// already smooth by construction (trilinear capture weights), so
	// this is a denoise, not a leak risk (the volume is convex-walled).
	for (uint32_t pass = 0; pass < cfg.smoothingPasses; ++pass) {
		const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1],
			nz = outVolume.dims[2];
		std::vector<char> valid(size_t(n), 0);
		for (uint64_t v = 0; v < n; ++v) {
			const uint32_t ix = uint32_t(v % nx);
			const uint32_t iy = uint32_t((v / nx) % ny);
			const uint32_t iz = uint32_t(v / (uint64_t(nx) * ny));
			valid[size_t(v)] = scene.Bvh().ClosestDistance(
				outVolume.gridMin + (glm::vec3(ix, iy, iz) + 0.5f) * spacing)
				>= cfg.probeMinClearance ? 1 : 0;
		}
		std::vector<glm::vec3> smoothed(size_t(n) * kSgLobeCount);
		static const int kDx[6] = { 1, -1, 0, 0, 0, 0 };
		static const int kDy[6] = { 0, 0, 1, -1, 0, 0 };
		static const int kDz[6] = { 0, 0, 0, 0, 1, -1 };
		const float lambda = std::clamp(cfg.smoothingLambda, 0.0f, 1.0f);
		for (uint32_t iz = 0; iz < nz; ++iz)
			for (uint32_t iy = 0; iy < ny; ++iy)
				for (uint32_t ix = 0; ix < nx; ++ix) {
					const uint64_t v = (uint64_t(iz) * ny + iy) * nx + ix;
					glm::vec3 acc[kSgLobeCount] = {};
					int cnt = 0;
					for (int k = 0; k < 6; ++k) {
						const int jx = int(ix) + kDx[k];
						const int jy = int(iy) + kDy[k];
						const int jz = int(iz) + kDz[k];
						if (jx < 0 || jy < 0 || jz < 0 || jx >= int(nx)
							|| jy >= int(ny) || jz >= int(nz))
							continue;
						const uint64_t u =
							(uint64_t(jz) * ny + uint64_t(jy)) * nx + uint64_t(jx);
						if (!valid[size_t(u)])
							continue;
						for (uint32_t i = 0; i < kSgLobeCount; ++i)
							acc[i] += outVolume.coeffs[size_t(u) * kSgLobeCount + i];
						++cnt;
					}
					for (uint32_t i = 0; i < kSgLobeCount; ++i) {
						const glm::vec3& cur =
							outVolume.coeffs[size_t(v) * kSgLobeCount + i];
						glm::vec3 next = cur;
						if (cnt > 0 && valid[size_t(v)])
							next = glm::mix(cur, acc[i] / float(cnt), lambda);
						smoothed[size_t(v) * kSgLobeCount + i] = next;
					}
				}
		outVolume.coeffs = std::move(smoothed);
	}

	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	{
		char line[192];
		std::snprintf(line, sizeof(line),
			"cvol bake: done: %llu probes from %llu captures in %.1fs "
			"(%.1f MiB GPU, %llu reprojected, %llu invalid, %llu clamped)\n",
			(unsigned long long)n, (unsigned long long)stats.captures,
			stats.seconds, double(outVolume.GpuBytes()) / (1024.0 * 1024.0),
			(unsigned long long)stats.reprojected,
			(unsigned long long)stats.probesInvalid,
			(unsigned long long)stats.samplesClamped);
		say(line);
	}
	return true;
}

} // namespace smvk::gi
