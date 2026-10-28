// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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

// File format: NO VERSION SCHEME (the repo data rule) — the MAGIC is
// the identity. "GVOL" = the post-visibility-bake shape (per-voxel
// 6-axis obstacle distances + the cell-clean flags; the predecessor
// "GIVL"/"CVOL" eras refuse at the magic — re-bake, no conversions).
constexpr float kPi = 3.14159265358979323846f;

struct GiVolumeFileHeader {
	char magic[4] = { 'G', 'V', 'O', 'L' };
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
static_assert(sizeof(GiVolumeFileHeader) == 60);

inline float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

// Clip a big box by every plane; the region must close (bounded).
bool ClipBounds(const std::vector<GiVolumePlane>& planes, glm::vec3& mn, glm::vec3& mx) {
	constexpr float kBig = 1.0e5f;
	glm::vec3 lo(-kBig), hi(kBig);	for (const GiVolumePlane& p : planes) {
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

bool GiVolumeDef::Bounds(glm::vec3& mn, glm::vec3& mx) const {
	return ClipBounds(planes, mn, mx);
}

std::vector<GiVolumePlane> GiVolumeBoxPlanes(const glm::vec3& mn, const glm::vec3& mx,
	float insideFade, float outsideFade) {
	std::vector<GiVolumePlane> planes;
	planes.reserve(6);
	auto add = [&](const glm::vec3& n, float d) {
		GiVolumePlane p{};
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

std::vector<GiVolumePlane> GiVolumeBoxPlanesRotated(const glm::vec3& center,
	const glm::mat3& axes, const glm::vec3& half,
	float insideFade, float outsideFade) {
	std::vector<GiVolumePlane> planes;
	planes.reserve(6);
	// Axis i: unit direction u (normalized column), half-extent h.
	// The two faces: u.P <= u.c + h and u.P >= u.c - h (inside:
	// n.P + d <= 0). Identity axes reproduce GiVolumeBoxPlanes exactly
	// (-x face: d = u.c - h = c.x - h = mn.x, etc).
	glm::vec3 u[3] = { axes[0], axes[1], axes[2] };
	for (glm::vec3& a : u) {
		const float len = glm::length(a);
		a = (len > 1e-6f) ? a / len : glm::vec3(0.0f);
	}
	for (int i = 0; i < 3; ++i) {
		const float h = half[i];
		const float uc = glm::dot(u[i], center);
		GiVolumePlane p{};
		p.insideFade = insideFade;
		p.outsideFade = outsideFade;
		p.normal = -u[i];
		p.d = uc - h;      // inside: u.P >= u.c - h
		planes.push_back(p);
		p.normal = u[i];
		p.d = -(uc + h);   // inside: u.P <= u.c + h
		planes.push_back(p);
	}
	return planes;
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

uint64_t GiVolume::VoxelCount() const {
	return uint64_t(dims[0]) * dims[1] * dims[2];
}

void GiVolume::Resize() {
	coeffs.assign(size_t(VoxelCount()) * kSgLobeCount, glm::vec3(0.0f));
}

bool GiVolume::Sample(
	const glm::vec3& p,
	glm::vec3 outAmps[kSgLobeCount]) const
{
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		outAmps[i] = glm::vec3(0.0f);

	if (coeffs.empty() || spacing <= 0.0f)
		return false;

	const glm::vec3 rel = (p - gridMin) / spacing;

	if (rel.x < 0.0f || rel.y < 0.0f || rel.z < 0.0f ||
		rel.x > float(dims[0]) ||
		rel.y > float(dims[1]) ||
		rel.z > float(dims[2]))
		return false;

	// Voxel-center convention: node (i,j,k) sits at gridMin +
	// (ijk + 0.5) * spacing, so the continuous grid coordinate is
	// rel - 0.5 (hardware Linear/Clamp mirror; == SgRegularVolume's
	// trilerp). THE CPU MIRROR OF SgGiVolumeSample (sg_gi_volume.hlsli):
	// the 8 cell corners each weighted by their 6-axis obstacle
	// distances vs the tap offset (the through-wall rejection), the
	// renormalized sum replaces the plain trilerp. Absent vis (a
	// hand-built volume / the pre-visibility era) degenerates to the
	// plain trilerp — every factor is 1. NOTE the runtime additionally
	// carries the per-cell clean flag fast path (the bake PROVES no
	// tap can reject and skips to the hardware trilerp); the CPU
	// always evaluates the weighted form — same values, no fast path
	// needed.
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
	const bool haveVis = vis.size() == size_t(VoxelCount()) * 6;
	// The per-axis visibility factor (SgGiVolumeVisFactor's CPU twin):
	// HYBRID REACH (matches the shader): proportional for large dist,
	// floor-clamped for small dist (wall-pixel coverage). The pure
	// proportional went to ~zero for near-surface probes — wall
	// pixels lost their nearest probes → black shadows. The floor
	// (max(dist * 1.5, 0.5)) ensures 0.5 spacing units of minimum
	// reach. The ramp width is also floor-clamped: max(0.5*dist, 0.3).
	const auto visFactor = [&](int x, int y, int z) {
		if (!haveVis)
			return 1.0f;
		const size_t c = idx(x, y, z);
		const glm::vec3 dRel(gx - (float(x) + 0.5f),
			gy - (float(y) + 0.5f), gz - (float(z) + 0.5f));
		float w = 1.0f;
		static const float kAxes[6][3] = {
			{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
			{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
		};
		for (int k = 0; k < 6; ++k) {
			const float dist = vis[c * 6 + size_t(k)] / spacing;
			const float along = dRel.x * kAxes[k][0]
				+ dRel.y * kAxes[k][1] + dRel.z * kAxes[k][2];
			const float reach = std::max(dist * 1.5f, 0.5f);
			if (along > reach) {
				w *= std::max(0.0f,
					1.0f - (along - reach)
						/ std::max(0.5f * dist, 0.3f));
				if (w <= 0.0f)
					return 0.0f;
			}
		}
		return w;
	};
	float wSum = 0.0f;
	glm::vec3 acc[kSgLobeCount] = {};
	for (int dz = 0; dz <= 1; ++dz)
		for (int dy = 0; dy <= 1; ++dy)
			for (int dx = 0; dx <= 1; ++dx) {
				const float wv = visFactor(x0 + dx, y0 + dy, z0 + dz);
				if (wv <= 0.0f)
					continue;
				const float w = wv
					* (dx ? fx : 1.0f - fx)
					* (dy ? fy : 1.0f - fy)
					* (dz ? fz : 1.0f - fz);
				const size_t c = idx(x0 + dx, y0 + dy, z0 + dz)
					* kSgLobeCount;
				for (uint32_t i = 0; i < kSgLobeCount; ++i)
					acc[i] += coeffs[c + i] * w;
				wSum += w;
			}
	if (wSum > 1e-4f)
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			outAmps[i] = acc[i] / wSum;
	// All 8 taps blocked (a point deep inside a wall): reads zero — the
	// hull weight still owns the pixel; zero beats the old both-sides
	// blend (the leak).
	return true;
}

void GiVolume::PackGpuLayers(std::vector<uint16_t> out[7]) const {
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

void GiVolume::PackGpuMeanLayer(std::vector<uint16_t>& out) const {
	// One RGBA16F texel per voxel: rgb = 0.124959 * ╬ú lobes (the
	// isotropic mean the fog consumes ΓÇö VolGiMean's constant in
	// volumetric.frag.hlsl, the fixed sharpness-4 SG mean weight
	// (1-e^-8)/8; MUST stay in sync with it), a = 1. Clamped to the
	// half max like every HDR writer (a 9-lobe sum can round to +inf
	// in half otherwise, and SanitizeHdr downstream would black the
	// texel). See PackGpuLayers for the coefficient layout.
	constexpr float kMeanWeight = 0.124959f; // (1-e^-8)/8
	constexpr float kHalfMax = 65504.0f;
	const uint64_t n = VoxelCount();
	out.assign(size_t(n) * 4, uint16_t(0));
	for (uint64_t v = 0; v < n; ++v) {
		glm::vec3 sum(0.0f);
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe)
			sum += coeffs[size_t(v) * kSgLobeCount + lobe];
		const glm::vec3 mean = glm::clamp(sum * kMeanWeight,
			glm::vec3(0.0f), glm::vec3(kHalfMax));
		for (int c = 0; c < 3; ++c)
			out[size_t(v) * 4 + c] = PackHalf(mean[c]);
		out[size_t(v) * 4 + 3] = PackHalf(1.0f);
	}
}

void GiVolume::PackGpuVisLayers(std::vector<uint8_t> out[2]) const {
	// Two RGBA8_UNORM 3D layers per volume, one texel per voxel:
	//   visA = (+x, -x, +y, -y) obstacle distances
	//   visB = (+z, -z, cellClean flag, spare)
	// Each distance is quantized unorm8 over [0, 15] SPACING UNITS
	// (dist/spacing capped at 15 — beyond any in-cell tap offset, so
	// the top of the range reads fully open to the runtime factor).
	// Absent vis (a hand-built volume / the pre-visibility era) packs
	// ALL-OPEN distances + ALL-CLEAN flags — the plain-trilerp
	// no-rejection contract, byte-identical behavior to before the
	// visibility bake existed.
	constexpr float kVisFullScale = 15.0f;
	const uint64_t n = VoxelCount();
	const bool haveVis = vis.size() == size_t(n) * 6;
	const bool haveFlags = visCellClean.size() == size_t(n);
	for (int t = 0; t < 2; ++t)
		out[t].assign(size_t(n) * 4, uint8_t(255));
	if (!haveVis) {
		// visA stays all-255 (open); visB's flag byte is channel 2 —
		// all-open means all-clean too, so leave it 255 as well.
		return;
	}
	for (uint64_t v = 0; v < n; ++v) {
		const float dist[6] = {
			std::min(vis[size_t(v) * 6 + 0] / spacing, kVisFullScale),
			std::min(vis[size_t(v) * 6 + 1] / spacing, kVisFullScale),
			std::min(vis[size_t(v) * 6 + 2] / spacing, kVisFullScale),
			std::min(vis[size_t(v) * 6 + 3] / spacing, kVisFullScale),
			std::min(vis[size_t(v) * 6 + 4] / spacing, kVisFullScale),
			std::min(vis[size_t(v) * 6 + 5] / spacing, kVisFullScale),
		};
		for (int k = 0; k < 6; ++k) {
			const uint8_t q = uint8_t(dist[k] / kVisFullScale * 255.0f + 0.5f);
			// k 0..3 ride visA channels 0..3 (+x,-x,+y,-y);
			// k 4..5 ride visB channels 0..1 (+z,-z).
			if (k < 4)
				out[0][size_t(v) * 4 + size_t(k)] = q;
			else
				out[1][size_t(v) * 4 + (size_t(k) - 4)] = q;
		}
		// visB channel 2 = the forward-cell clean flag (255 = clean).
		out[1][size_t(v) * 4 + 2] = haveFlags && visCellClean[size_t(v)]
			? uint8_t(255) : uint8_t(0);
	}
}

size_t GiVolume::GpuBytes() const {
	// 7 RGBA16F layers (56 B) + 2 RGBA8_UNORM vis layers (8 B) per voxel.
	return size_t(VoxelCount()) * 64;
}

bool GiVolume::WriteToMemory(std::vector<uint8_t>& out, std::string& err) const {
	GiVolumeFileHeader hdr{};
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
		const GiVolumePlane& p = def.planes[i];
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
	// Payload: planes + coeffs + the 6-axis visibility block (6 fp32
// per voxel, meters, capped at the bake's visibilityMaxDistance;
// 0 = blocked) + the per-voxel cell-clean flags (1 byte each, the
// tail 4-aligned). Blocks are written ONLY when the in-memory data
// is complete (a hand-built volume serializes with neither, and the
// reader sizes the payload to tell the cases apart - empty in,
// empty out: the no-rejection contract stays honest to disk).
	const size_t visFloats = vis.size() == size_t(n) * 6
		? size_t(n) * 6 : 0;
	const size_t flagBytes = visCellClean.size() == size_t(n)
		? size_t(n) : 0;
	const size_t flagTail = (flagBytes + 3u) & ~size_t(3u);
	std::vector<uint8_t> payload;
	payload.resize(planeBytes.size()
		+ size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t)
		+ visFloats * sizeof(float)
		+ flagTail);
	std::memcpy(payload.data(), planeBytes.data(), planeBytes.size());
	auto* dst = reinterpret_cast<uint16_t*>(payload.data() + planeBytes.size());
	for (uint64_t v = 0; v < n; ++v)
		for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe) {
			const glm::vec3& a = coeffs[size_t(v) * kSgLobeCount + lobe];
			dst[(size_t(v) * kSgLobeCount + lobe) * 3 + 0] = PackHalf(a.r);
			dst[(size_t(v) * kSgLobeCount + lobe) * 3 + 1] = PackHalf(a.g);
			dst[(size_t(v) * kSgLobeCount + lobe) * 3 + 2] = PackHalf(a.b);
		}
	if (visFloats) {
		float* vdst = reinterpret_cast<float*>(
			payload.data() + planeBytes.size()
			+ size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t));
		std::memcpy(vdst, vis.data(), visFloats * sizeof(float));
	}
	if (flagBytes)
		std::memcpy(payload.data() + payload.size() - flagTail,
			visCellClean.data(), flagBytes);

	std::vector<uint8_t> compressed;
	hdr.payloadBytes = uint32_t(payload.size());
	hdr.compressed = 1;
	if (!Lz4Compress(payload, compressed, 1)) {
		err = "GiVolume: LZ4 compression failed";
		return false;
	}

	out.resize(sizeof(GiVolumeFileHeader) + compressed.size());
	std::memcpy(out.data(), &hdr, sizeof(hdr));
	std::memcpy(out.data() + sizeof(hdr), compressed.data(), compressed.size());
	return true;
}

bool GiVolume::WriteFile(const std::string& path, std::string& err) const {
	std::vector<uint8_t> bytes;
	if (!WriteToMemory(bytes, err))
		return false;
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "GiVolume: cannot open output " + path;
		return false;
	}
	const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	if (written != bytes.size()) {
		err = "GiVolume: short write to " + path;
		return false;
	}
	return true;
}

bool GiVolume::ReadFromMemory(std::span<const uint8_t> bytes, std::string& err) {
	GiVolumeFileHeader hdr{};
	if (bytes.size() < sizeof(hdr)) {
		err = "givolume: file too small";
		return false;
	}
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	if (std::memcmp(hdr.magic, "GVOL", 4) != 0) {
		err = "givolume: bad magic (pre-visibility bakes refuse - re-bake)";
		return false;
	}
	if (hdr.lobeCount != kSgLobeCount || hdr.sharpness != kSgSharpness) {
		err = "givolume: basis mismatch (re-bake required)";
		return false;
	}
	if (hdr.planeCount == 0 || hdr.planeCount > kGiVolumeMaxPlanes) {
		err = "givolume: bad plane count";
		return false;
	}
	if (hdr.dims[0] == 0 || hdr.dims[1] == 0 || hdr.dims[2] == 0
		|| hdr.dims[0] > kGiVolumeMaxDim || hdr.dims[1] > kGiVolumeMaxDim
		|| hdr.dims[2] > kGiVolumeMaxDim) {
		err = "givolume: bad dims";
		return false;
	}
	if (hdr.spacing <= 0.0f) {
		err = "givolume: bad spacing";
		return false;
	}

	std::vector<uint8_t> payload;
	const std::span<const uint8_t> stored(bytes.data() + sizeof(hdr),
		bytes.size() - sizeof(hdr));
	if (hdr.compressed) {
		if (!Lz4Decompress(stored, hdr.payloadBytes, payload)) {
			err = "givolume: LZ4 decompression failed";
			return false;
		}
	} else {
		payload.assign(stored.begin(), stored.end());
	}

	const size_t planeBytes = size_t(hdr.planeCount) * 8 * sizeof(float);
	const uint64_t n = uint64_t(hdr.dims[0]) * hdr.dims[1] * hdr.dims[2];
	const size_t coeffBytes = size_t(n) * 3 * kSgLobeCount * sizeof(uint16_t);
	// Payload shapes (the writer's three cases): coeffs only (a
	// hand-built volume — no rejection data, the pre-visibility
	// behavior), coeffs + vis floats, or coeffs + vis + the 4-aligned
	// flag tail.
	const size_t visBytes = size_t(n) * 6 * sizeof(float);
	const size_t flagTail = (size_t(n) + 3u) & ~size_t(3u);
	const size_t coeffEnd = planeBytes + coeffBytes;
	const bool hasVis = payload.size() >= coeffEnd + visBytes;
	const bool hasFlags = payload.size() >= coeffEnd + visBytes + flagTail;
	if (payload.size() != coeffEnd
		&& payload.size() != coeffEnd + visBytes
		&& payload.size() != coeffEnd + visBytes + flagTail) {
		err = "givolume: payload size mismatch";
		return false;
	}

	def.planes.resize(hdr.planeCount);
	const auto* pf = reinterpret_cast<const float*>(payload.data());
	for (uint32_t i = 0; i < hdr.planeCount; ++i) {
		GiVolumePlane& p = def.planes[i];
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

	// The visibility block: 6 fp32 obstacle distances per voxel +
	// the cell-clean flags. Absent blocks stay EMPTY — the no-rejection
	// contract (Sample degenerates to the plain trilerp,
	// PackGpuVisLayers carries all-open/all-clean defaults).
	vis.clear();
	visCellClean.clear();
	if (hasVis) {
		const auto* vsrc = reinterpret_cast<const float*>(
			payload.data() + coeffEnd);
		vis.assign(vsrc, vsrc + size_t(n) * 6);
	}
	if (hasFlags) {
		visCellClean.assign(payload.data() + coeffEnd + visBytes,
			payload.data() + coeffEnd + visBytes + size_t(n));
	}
	return true;
}

bool GiVolume::ReadFile(const std::string& path, GiVolume& out, std::string& err) {
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
// Bake: per-probe direct path trace -> SG fit -> same-side dilation +
// Laplacian -> 6-axis visibility + the clean-cell flags
// ---------------------------------------------------------------------------

bool BakeGiVolume(const GiTraceScene& scene, const GiVolumeDef& def,
	float spacing, const GiVolumeBakeConfig& configIn, GiVolume& outVolume,
	GiVolumeBakeStats& stats, GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	GiVolumeBakeConfig cfg = configIn;
	auto say = [&](const char* line) {
		if (progress)
			progress(line);
	};

	if (def.planes.empty() || def.planes.size() > kGiVolumeMaxPlanes) {
		say("givolume bake: bad plane count\n");
		return false;
	}
	spacing = std::max(spacing, 0.01f);
	cfg.captureDirs = std::max(cfg.captureDirs, 64u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);

	glm::vec3 bMin, bMax;
	if (!def.Bounds(bMin, bMax)) {
		say("givolume bake: planes do not bound a closed region\n");
		return false;
	}

	outVolume.def = def;
	outVolume.spacing = spacing;
	outVolume.gridMin = bMin;
	const glm::vec3 ext = bMax - bMin;
	for (int a = 0; a < 3; ++a) {
		outVolume.dims[a] = std::max(1u, uint32_t(std::ceil(ext[a] / spacing)));
		outVolume.dims[a] = std::min(outVolume.dims[a], kGiVolumeMaxDim);
	}
	const uint64_t n = outVolume.VoxelCount();
	if (n > uint64_t(kGiVolumeMaxDim) * kGiVolumeMaxDim * kGiVolumeMaxDim) {
		say("givolume bake: grid too dense (cap 256^3)\n");
		return false;
	}
	outVolume.Resize();
	stats.probes = n;

	// ---- Shared per-voxel state: validity + 6-axis visibility ----
	// Hoisted ONCE (the trace loop, the dilation and the clean-flag
	// pass below all consume it; the trace used to recompute the
	// clearance test inline and the dilation recomputed the whole
	// array). valid = the probe center clears cfg.probeMinClearance of
	// geometry (the trace set; the dilation fills them).
	// The 6-axis visibility (the through-wall rejection data — the
	// dense-grid rebirth of the retired .sggi octree's per-probe
	// wall-awareness) rides the SAME walk: 6 raycasts per probe along
	// the lattice axes, obstacle distance capped at
	// cfg.visibilityMaxDistance (a miss = the cap = "open"); invalid
	// probes record ALL-ZERO = "blocked" (any runtime tap whose offset
	// exceeds the slack rejects, so inside-solid texels never
	// contribute). The runtime twin is SgGiVolumeVisFactor
	// (sg_gi_volume.hlsli); the CPU twin is GiVolume::Sample.
	std::vector<char> valid(size_t(n), 0);
	outVolume.vis.assign(size_t(n) * 6, 0.0f);
	const float visCap = std::max(cfg.visibilityMaxDistance, spacing);
	// Axis order +x,-x,+y,-y,+z,-z (the runtime/CPU twins read the
	// same order).
	static const glm::vec3 kVisAxes[6] = {
		glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
		glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)
	};
	{
		const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1];
		for (uint64_t v = 0; v < n; ++v) {
			const uint32_t ix = uint32_t(v % nx);
			const uint32_t iy = uint32_t((v / nx) % ny);
			const uint32_t iz =
				uint32_t(v / (uint64_t(nx) * ny));
			const glm::vec3 pos = outVolume.gridMin
				+ (glm::vec3(ix, iy, iz) + 0.5f) * outVolume.spacing;
			valid[size_t(v)] = scene.Bvh().ClosestDistance(pos)
				>= cfg.probeMinClearance ? 1 : 0;
		}
	}

	// Shared equal-solid-angle direction set (same rationale as the
	// other bakers: correlated error reads as smooth bias). These are
	// the per-probe ray directions: uniform solid angle 4pi/N, so the
	// SG fit weights are exact.
	const uint32_t N = cfg.captureDirs;
	std::vector<glm::vec3> dirs(N);
	for (uint32_t i = 0; i < N; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(N);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		dirs[i] = glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r);
	}
	const float texelSolidAngle = 4.0f * kPi / float(N);

	// ---- Dense probes: DIRECT per-probe path trace ----
	// The BO3 capture lattice + reprojection this bake shipped with
	// amortized the trace over sparse captures — at the cost of a
	// capture-support dependency: probes whose enclosing cell had no
	// valid capture (solid interiors, clearance skips) baked zero, the
	// parallax/depth gates rejected texels near walls, and coverage
	// read as holes (the sponza bake: 48k of 526k probes invalid). The
	// Embree backend traces the whole dense grid directly now: every
	// probe runs its own equal-solid-angle set through TraceIncoming
	// (same transport as every other baked source — sun/sky/bounces,
	// the white-world field convention) and feeds the SG fit with
	// exact texel solid angles. captureDirs keeps its meaning, now as
	// the per-probe direction count (the octree probes run ~1024; 1536
	// is generous — the sponza grid traces in ~2x the world octree's
	// time). The mathematical identity case of the old pipeline
	// (captureSpacing == spacing) is what this IS, minus the amortize.
	{
		const uint32_t threadCount = cfg.threadCount
			? cfg.threadCount : std::thread::hardware_concurrency();
		ThreadPool pool(threadCount ? threadCount : 1);
		std::atomic<uint64_t> invalid{ 0 };
		std::atomic<uint64_t> clamped{ 0 };
		constexpr size_t kBatch = 128;
		for (uint64_t base = 0; base < n; base += kBatch) {
			const uint64_t end = std::min(base + kBatch, n);
			std::vector<std::future<void>> futures;
			futures.reserve(size_t(end - base));
			for (uint64_t v = base; v < end; ++v) {
				futures.push_back(pool.enqueue([&, v] {
					const uint32_t nx = outVolume.dims[0],
						ny = outVolume.dims[1];
					const uint32_t ix = uint32_t(v % nx);
					const uint32_t iy = uint32_t((v / nx) % ny);
					const uint32_t iz =
						uint32_t(v / (uint64_t(nx) * ny));
					const glm::vec3 pos = outVolume.gridMin
						+ (glm::vec3(ix, iy, iz) + 0.5f)
							* outVolume.spacing;

					SgFitAccumulator acc;
					acc.accumulateGram = false; // diagonal solve only
					if (valid[size_t(v)]) {
						// Deterministic per-probe seed (the capture
						// stage's pattern): results independent of
						// task scheduling.
						const uint64_t seed = (uint64_t(cfg.seed) << 32)
							^ (uint64_t(v) * 0x9e3779b97f4a7c15ull
								+ 0x1234567ull);
						Pcg32 rng(seed);
						std::vector<glm::vec3> rad(N);
						for (uint32_t i = 0; i < N; ++i)
							rad[i] = scene.TraceIncoming(pos, dirs[i],
								rng, cfg.maxBounces, nullptr);
						// Two-pass firefly clamp (same policy as every
						// other bake: 8x the probe's own finite-sample
						// mean).
						double meanLum = 0.0;
						for (const glm::vec3& L : rad)
							meanLum += Luminance(L);
						meanLum /= double(N);
						const float p8 =
							std::max(float(meanLum) * 8.0f, 1e-6f);
						uint64_t localClamped = 0;
						for (glm::vec3& L : rad) {
							const float l = Luminance(L);
							if (l > p8 && l > 0.0f) {
								L *= p8 / l;
								++localClamped;
							}
						}
						clamped += localClamped;
						for (uint32_t i = 0; i < N; ++i)
							acc.AddSample(dirs[i], rad[i],
								texelSolidAngle);
						// 6-axis obstacle distances (the through-wall
						// rejection data — see the hoisted block above).
						// A miss = the cap = open; the pre-zeroed array
						// already carries the blocked zeros for the
						// invalid set.
						for (uint32_t k = 0; k < 6; ++k) {
							TriangleBvh::Hit vh;
							outVolume.vis[size_t(v) * 6 + k] =
								scene.Bvh().Raycast(pos, kVisAxes[k],
									visCap, vh)
								? std::min(vh.t, visCap)
								: visCap;
						}
					}
					else {
						++invalid;
					}
					glm::vec3 amps[kSgLobeCount];
					acc.Solve(amps);
					for (uint32_t i = 0; i < kSgLobeCount; ++i)
						outVolume.coeffs[size_t(v) * kSgLobeCount + i]
							= amps[i];
				}));
			}
			for (auto& f : futures)
				f.get();
			if (progress && ((base / kBatch) & 15u) == 0) {
				char line[128];
				std::snprintf(line, sizeof(line),
					"givolume bake: %llu/%llu probes\n",
					(unsigned long long)end, (unsigned long long)n);
				say(line);
			}
		}
		stats.probesInvalid = invalid.load();
		stats.samplesClamped = clamped.load();
	}

	
	// ---- Shared neighbor gates (the same-side rule) ----
	// The axis pair table: index k = 0..5 is +x,-x,+y,-y,+z,-z; the
	// neighbor at v + kAxis[k] is reached through v's axis k and looks
	// back through its OPPOSITE (k^1) — the adjacent +x/-x, +y/-y,
	// +z/-z table pairs in the vis member. visOpen(v, k): voxel v's
	// obstacle distance along axis k vs the neighbor one spacing away.
	// The 0.35-spacing gate slack is PAIRED with the runtime factor's
	// slack (SgGiVolumeVisFactor / GiVolume::Sample) — a point AT an
	// obstacle surface keeps reading its own surface's neighbors.
	const auto visOpen = [&](uint64_t v, int k) {
		return outVolume.vis[size_t(v) * 6 + size_t(k)]
			>= spacing - 0.35f * spacing;
	};
	// The gate checks the SOURCE's visibility only (u's ability to see
	// toward the target v). The TARGET's own vis is meaningless for the
	// fill decision: inside-geometry targets carry all-zero distances by
	// definition, and requiring THEIR openness too (the old mutual form)
	// meant inside-geometry probes were NEVER filled - their dilation
	// stayed zero, and the renormalized weighted path at runtime read a
	// few surviving corners' raw values as dark lattice-aligned squares
	// (the sponza_3 report: the flat irradiance view showed the grid).
	// The SOURCE's check alone is sufficient: a valid neighbor that can
	// see toward the target is on the same side; one that cannot (the
	// wall between them) is across.
	const auto sameSide = [&](uint64_t u, uint64_t v, int k) {
		// u is the SOURCE (valid), v is the TARGET (any). Check u's
		// visibility toward v: axis k^1 is u's back-axis toward v.
		return visOpen(u, k ^ 1);
	};

	// ---- Validity dilation (probes inside solid geometry; the same
	// Jacobi scheme as the regular-volume bake: nearest-valid values
	// spread over the holes so facade-adjacent trilinear taps see
	// facade-like lighting instead of black). SAME-SIDE ONLY since the
	// visibility bake: a neighbor participates only when the pair is
	// MUTUALLY OPEN, so the fill can no longer average ACROSS a wall —
	// the old unconditional fill bridged thin walls with the mean of
	// both rooms (leak path one). The valid array is the hoisted one
	// (no recomputation here).
	{
		const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1],
			nz = outVolume.dims[2];
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
							if (!sameSide(u, v, k))
								continue; // the source cannot see the target: across a wall
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
	// over ALL voxels, dilated ones included. The valid-only exclusion
	// this pass used to carry left the dilation's neighbor-average
	// islands (the near-surface clearance shells — every wall's 5 cm
	// band, dense clusters along thin geometry like the chains) as
	// NEVER-SMOOTHED sharp values against the per-probe field: the
	// runtime trilerp read them as a binary mask pattern laid over
	// every surface. A lambda Laplacian softens the seams without
	// emptying anything (the padding survives — it just stops being a
	// hard edge), so the dilated voxels participate on both sides:
	// they take neighbor averages AND contribute to their neighbors.
	// The visibility bake: the neighbor set is SAME-SIDE GATED
	// (the sameSide rule above): along-surface neighbors still
	// smooth — the seam-softening this pass exists for — while
	// across-wall pairs are excluded, so the Laplacian no longer
	// smears bright exterior values into dark interiors through thin
	// walls (leak path two).
	{
		for (uint32_t pass = 0; pass < cfg.smoothingPasses; ++pass) {
			const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1],
				nz = outVolume.dims[2];
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
							if (!sameSide(u, v, k))
								continue; // across a wall: no cross-side smear
							for (uint32_t i = 0; i < kSgLobeCount; ++i)
								acc[i] += outVolume.coeffs[size_t(u) * kSgLobeCount + i];
							++cnt;
						}
						for (uint32_t i = 0; i < kSgLobeCount; ++i) {
							const glm::vec3& cur =
								outVolume.coeffs[size_t(v) * kSgLobeCount + i];
							glm::vec3 next = cur;
							if (cnt > 0)
								next = glm::mix(cur, acc[i] / float(cnt), lambda);
							smoothed[size_t(v) * kSgLobeCount + i] = next;
						}
					}
			outVolume.coeffs = std::move(smoothed);
		}
	}

	// ---- Fast-path flags (the forward cell's clean bit) ----
	// Clean = every corner of the cell [v, v+1]^3 records every axis
	// distance >= 1.35 spacing units: the maximum in-cell tap offset
	// along any axis is 1 spacing, and rejection only starts past
	// dist + 0.35, so no tap in a clean cell can ever reject and the
	// runtime's hardware trilerp IS the visibility-weighted blend.
	// Conservative by construction (a clean-but-near-wall cell simply
	// takes the weighted path — same values, more fetches). Voxels at
	// the +1 edges have no forward cell (the runtime clamps its base
	// corner to dims-2) and stay 0.
	uint64_t visBlocked = 0, visCleanCells = 0;
	{
		const uint32_t nx = outVolume.dims[0], ny = outVolume.dims[1],
			nz = outVolume.dims[2];
		outVolume.visCellClean.assign(size_t(n), 0);
		constexpr float kCleanMin = 1.35f; // 1 spacing reach + the 0.35 slack
		for (uint64_t v = 0; v < n; ++v) {
			// Blocked = every axis zero (the inside-solid set — the
			// runtime factor kills any offset past the slack).
			bool blocked = true;
			for (uint32_t k = 0; k < 6; ++k)
				if (outVolume.vis[size_t(v) * 6 + k] > 1e-4f) {
					blocked = false;
					break;
				}
			if (blocked)
				++visBlocked;
		}
		for (uint32_t iz = 0; iz + 1 < nz; ++iz)
			for (uint32_t iy = 0; iy + 1 < ny; ++iy)
				for (uint32_t ix = 0; ix + 1 < nx; ++ix) {
					bool clean = true;
					for (uint32_t dz = 0; dz < 2 && clean; ++dz)
						for (uint32_t dy = 0; dy < 2 && clean; ++dy)
							for (uint32_t dx = 0; dx < 2 && clean; ++dx) {
								const uint64_t c = ((uint64_t(iz + dz) * ny
									+ uint64_t(iy + dy)) * nx + uint64_t(ix + dx));
								for (uint32_t k = 0; k < 6; ++k)
									if (outVolume.vis[size_t(c) * 6 + k] / spacing
											< kCleanMin) {
										clean = false;
										break;
									}
							}
					if (clean) {
						const uint64_t v = (uint64_t(iz) * ny + iy) * nx + ix;
						outVolume.visCellClean[size_t(v)] = 1;
						++visCleanCells;
					}
				}
	}	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	{
		char line[224];
		std::snprintf(line, sizeof(line),
			"givolume bake: done: %llu probes (per-probe direct trace) "
			"in %.1fs (%.1f MiB GPU, %llu invalid, %llu clamped, "
			"vis: %llu blocked / %llu clean cells)\n",
			(unsigned long long)n,
			stats.seconds, double(outVolume.GpuBytes()) / (1024.0 * 1024.0),
			(unsigned long long)stats.probesInvalid,
			(unsigned long long)stats.samplesClamped,
			(unsigned long long)visBlocked,
			(unsigned long long)visCleanCells);
		say(line);
	}
	return true;
}

} // namespace smvk::gi
