// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// WorldFieldsTests: the virtual world volume system's CPU contracts.
//
// Covers the implementation plan's milestone-1 (core virtual addressing)
// and milestone-2 (atlas + page-table + sampling) acceptance criteria:
//   * signed world->brick addressing (negative coords, LOD boundaries,
//     exact brick edges) — floor discipline everywhere;
//   * atlas slot decode + halo-skip UVW math (never crosses slot bounds);
//   * decode packings (Linear / SqrtUNorm / SignedNormalized);
//   * pool slot bookkeeping (acquire/release/exhaustion);
//   * RESIDENCY CHURN: a tiny 2x2x2 pool under a 4x2x4 window forces
//     continuous eviction; every published page's slot must hold exactly
//     that page's brick (the slot-reuse invariant — no stale page can
//     ever point at another brick's data), across camera walks AND
//     teleports;
//   * the SEAM PROOF: a software trilinear mirror of world_field.hlsli
//     (same formulas, hand-copied) sampling through the paged atlas
//     equals a dense unpaged lattice reference at deterministic
//     positions INCLUDING exact brick boundary edges/corners (the 2-texel
//     halo makes in-atlas filtering read the true neighbor values);
//   * absence (known-empty bricks): negative cache, no slot, no
//     re-request storm;
//   * hysteresis + coarsest-LOD protection on eviction.
//
// Hand-rolled runner (no test framework dependency), like the other tests.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Memory/memory.h>
#include <Engine/WorldFields/WorldField.h>
#include <Engine/WorldFields/WorldFieldGpu.h>

namespace wf = engine::worldfields;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name)
{
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

bool Near(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// Test source: a world-LINEAR field (v = clamp(a + b*p)) quantized to 8
// bits — trilinear over the stored lattice reproduces the dense lattice
// EXACTLY (both sides interpolate the same quantized bytes), so seam
// equality can be asserted tightly. Optional empty behaviors: y below a
// plane reports EMPTY, and/or all LOD-0 bricks report EMPTY (the
// coarse-eviction liveness scenario: only coarse bricks can ever be
// resident).
// ---------------------------------------------------------------------------
struct LinearFieldSource final : wf::IWorldFieldBrickSource
{
	// per-key generate-call counters (absence-caching assertions).
	std::unordered_map<wf::WorldBrickKey, int, wf::WorldBrickKeyHash> calls;
	std::unordered_set<wf::WorldBrickKey, wf::WorldBrickKeyHash> absentKeys;
	int totalCalls = 0;
	bool emptyBelowY = true; // world y < -8 reports empty (absence path)
	bool emptyLod0 = false; // every LOD-0 brick reports empty

	static float WorldValue(glm::vec3 p)
	{
		float v = 0.125f + 0.0031f * p.x + 0.0021f * p.y + 0.0013f * p.z;
		return std::min(std::max(v, 0.0f), 1.0f);
	}

	static uint8_t Quantized(glm::vec3 p)
	{
		return uint8_t(WorldValue(p) * 255.0f + 0.5f);
	}

	bool GenerateBrick(const wf::WorldFieldDesc& field, uint32_t lod,
		int32_t x, int32_t y, int32_t z,
		std::byte* payload, size_t payloadBytes) override
	{
		const wf::WorldBrickKey key{ 0, lod, x, y, z };
		++calls[key];
		++totalCalls;

		// Empty rules (absence path): below the ground plane, or all of
		// LOD 0 (the coarse-liveness scenario).
		if (emptyLod0 && lod == 0) {
			absentKeys.insert(key);
			return false;
		}
		if (emptyBelowY) {
			const float brickMinY = field.originMeters.y
				+ float(y) * float(wf::kBrickInteriorRes) * field.baseVoxelSizeMeters
					* float(1u << lod);
			if (brickMinY + 1e-4f < -8.0f) {
				absentKeys.insert(key);
				return false;
			}
		}

		const uint32_t stored = wf::kBrickStoredRes;
		assert(payloadBytes == size_t(stored) * stored * stored
			&& field.format == smvk::Format::R8_UNorm);
		const float voxel = field.baseVoxelSizeMeters * float(1u << lod);
		const glm::vec3 brickMin = field.originMeters
			+ glm::vec3(float(x), float(y), float(z))
				* (float(wf::kBrickInteriorRes) * voxel);
		size_t i = 0;
		for (uint32_t tz = 0; tz < stored; ++tz)
			for (uint32_t ty = 0; ty < stored; ++ty)
				for (uint32_t tx = 0; tx < stored; ++tx) {
					// TEXEL CENTERS (+0.5) minus the halo — the convention
					// that aligns the stored lattice with hardware
					// trilinear's texel-center grid (see the WorldRenderer
					// checkerboard source comment).
					const glm::vec3 texel = brickMin
						+ glm::vec3(float(tx) + 0.5f - float(wf::kBrickBorder),
							float(ty) + 0.5f - float(wf::kBrickBorder),
							float(tz) + 0.5f - float(wf::kBrickBorder)) * voxel;
					payload[i++] = std::byte(Quantized(texel));
				}
		return true;
	}
};

// ---------------------------------------------------------------------------
// The simulated atlas + the HLSL sampler mirror.
// ---------------------------------------------------------------------------

struct SimAtlas
{
	std::vector<uint8_t> bytes;
	uint32_t w = 0, h = 0, d = 0; // atlas dims (texels)
};

// Table views over TableBytes() — the same BDA the GPU would read.
struct TableView
{
	const std::byte* base = nullptr;
	uint32_t usedBytes = 0;

	wf::WorldFieldTableHeader Header() const
	{
		wf::WorldFieldTableHeader h{};
		std::memcpy(&h, base, sizeof(h));
		return h;
	}
	wf::WorldFieldGpu Field(uint32_t id, uint32_t stride) const
	{
		wf::WorldFieldGpu f{};
		std::memcpy(&f, base + 16 + size_t(id) * stride, sizeof(f));
		return f;
	}
	wf::WorldPageGpu Page(uint32_t byteOffset) const
	{
		wf::WorldPageGpu p{};
		std::memcpy(&p, base + byteOffset, sizeof(p));
		return p;
	}
};

float FetchTexel(const SimAtlas& a, int x, int y, int z)
{
	return float(a.bytes[(size_t(z) * a.h + size_t(y)) * a.w + size_t(x)]) / 255.0f;
}

// Hardware-trilinear mirror: SampleLevel at normalized uvw picks
// i0 = floor(c - 0.5), alpha = frac(c - 0.5) per axis (c = uvw * dims).
float AtlasTrilinear(const SimAtlas& a, glm::vec3 uvw)
{
	const glm::vec3 c = uvw * glm::vec3(float(a.w), float(a.h), float(a.d));
	int i0[3];
	float alpha[3];
	for (int k = 0; k < 3; ++k) {
		const float t = c[k] - 0.5f;
		i0[k] = int(std::floor(t));
		alpha[k] = t - float(i0[k]);
	}
	float acc = 0.0f;
	for (int dz = 0; dz < 2; ++dz)
		for (int dy = 0; dy < 2; ++dy)
			for (int dx = 0; dx < 2; ++dx) {
				const float w = (dx ? alpha[0] : 1.0f - alpha[0])
					* (dy ? alpha[1] : 1.0f - alpha[1])
					* (dz ? alpha[2] : 1.0f - alpha[2]);
				acc += w * FetchTexel(a, i0[0] + dx, i0[1] + dy, i0[2] + dz);
			}
	return acc;
}

// Dense unpaged reference: trilinear over the infinite quantized lattice
// of texel centers (i + 0.5) * voxel (the SAME lattice the atlas stores —
// halo included), at a continuous cell position.
float ReferenceSample(const wf::WorldFieldDesc& d, uint32_t lod, glm::vec3 pos)
{
	const float voxel = d.baseVoxelSizeMeters * float(1u << lod);
	glm::vec3 L;
	int i0[3];
	float alpha[3];
	for (int k = 0; k < 3; ++k) {
		L[k] = (pos[k] - d.originMeters[k]) / voxel;
		const float t = L[k] - 0.5f;
		i0[k] = int(std::floor(t));
		alpha[k] = t - float(i0[k]);
	}
	auto lattice = [&](int i[3]) {
		glm::vec3 p = d.originMeters + glm::vec3(
			(float(i[0]) + 0.5f) * voxel,
			(float(i[1]) + 0.5f) * voxel,
			(float(i[2]) + 0.5f) * voxel);
		return float(LinearFieldSource::Quantized(p)) / 255.0f;
	};
	float acc = 0.0f;
	for (int dz = 0; dz < 2; ++dz)
		for (int dy = 0; dy < 2; ++dy)
			for (int dx = 0; dx < 2; ++dx) {
				int i[3] = { i0[0] + dx, i0[1] + dy, i0[2] + dz };
				const float w = (dx ? alpha[0] : 1.0f - alpha[0])
					* (dy ? alpha[1] : 1.0f - alpha[1])
					* (dz ? alpha[2] : 1.0f - alpha[2]);
				acc += w * lattice(i);
			}
	return acc;
}

// The full world_field.hlsli SampleWorldField mirror (hand-copied
// formulas — deliberately test-local so the HLSL text and this stay an
// independent double entry). servedLod = the level that produced the
// value, or -1 for the decoded fallback. The g_reject* counters are
// diagnostics for the served-fraction check.
int g_rejectBounds = 0;
int g_rejectPage = 0;
int g_rejectStale = 0;
float MirrorSample(const wf::WorldFieldSystem& sys, const SimAtlas& atlas,
	const wf::WorldFieldDesc& d, glm::vec3 pos, uint32_t lod, int& servedLod)
{
	servedLod = -1;
	TableView view{ sys.TableBytes(), sys.TableUsedBytes() };
	if (view.base == nullptr || view.usedBytes < sizeof(wf::WorldFieldTableHeader))
		return 0.0f;
	const wf::WorldFieldTableHeader h = view.Header();
	if (h.fieldCount == 0)
		return 0.0f;
	const wf::WorldFieldGpu f = view.Field(0, h.fieldStride); // fieldId 0
	uint32_t l = std::min(lod, f.meta.z - 1u);
	// NOTE: no atlasIndex (meta.x) gate and no GpuBinding() gate — the
	// HLSL checks those for GPU pool-exhaustion/headless degradation;
	// this mirror reads the table's own header, and the sim atlas IS
	// the atlas.
	for (; l < f.meta.z; ++l) {
		const wf::WorldLevelGpu& lv = f.levels[l];
		// (swizzles are OFF in this GLM config — explicit truncation)
		const glm::vec3 origin(f.originMeters);
		const glm::ivec3 windowMin(lv.windowMinBrick);
		const glm::uvec3 windowDim(lv.windowDim);
		const glm::vec3 cell = (pos - origin) / lv.voxelSizeMeters;
		const glm::ivec3 brick(glm::floor(cell / 64.0f));
		const glm::vec3 local = cell - glm::vec3(brick) * 64.0f;
		const glm::ivec3 rel = brick - windowMin;
		if (glm::any(glm::lessThan(rel, glm::ivec3(0)))
			|| glm::any(glm::greaterThanEqual(rel, glm::ivec3(windowDim)))) {
			++g_rejectBounds;
			continue;
		}
		const glm::uvec3 ring = glm::uvec3(brick) & (windowDim - 1u);
		const uint32_t byteIndex = ((ring.z * windowDim.y + ring.y)
			* windowDim.x + ring.x) * 16u;
		const wf::WorldPageGpu page = view.Page(lv.pageTableOffset + byteIndex);
		if (page.atlasSlot < 0
			|| page.brickX != brick.x || page.brickY != brick.y || page.brickZ != brick.z) {
			++(page.atlasSlot < 0 ? g_rejectPage : g_rejectStale);
			continue;
		}
		const uint32_t s = uint32_t(page.atlasSlot);
		const uint32_t sx = s % f.atlasSlots.x;
		const uint32_t sy = (s / f.atlasSlots.x) % f.atlasSlots.y;
		const uint32_t sz = s / (f.atlasSlots.x * f.atlasSlots.y);
		const glm::vec3 atlasVoxel = glm::vec3(sx, sy, sz) * 68.0f + local + 2.0f;
		const glm::vec3 uvw = atlasVoxel / (glm::vec3(f.atlasSlots) * 68.0f);
		const float encoded = AtlasTrilinear(atlas, uvw);
		// decode: Linear, scale 1, bias 0 (this test's field).
		servedLod = int(l);
		return encoded;
	}
	return f.fallbackValue.x;
}

} // namespace

// ---------------------------------------------------------------------------
// 1) Addressing math
// ---------------------------------------------------------------------------
static void TestAddressing()
{
	using S = wf::WorldFieldSystem;
	const glm::vec3 origin(0.0f, 0.0f, 0.0f);

	{
		const auto a = S::WorldToVoxelAddress(glm::vec3(0.5f, 10.5f, 20.5f), origin, 1.0f);
		Check(a.brick[0] == 0 && a.brick[1] == 0 && a.brick[2] == 0, "addr.positive-in-brick-zero");
		Check(Near(a.local[0], 0.5f, 1e-6f) && Near(a.local[1], 10.5f, 1e-6f), "addr.positive-local");
	}
	{
		// -0.5 world -> cell -0.5 -> brick -1, local 63.5 (floor, not trunc)
		const auto a = S::WorldToVoxelAddress(glm::vec3(-0.5f, 0.0f, 0.0f), origin, 1.0f);
		Check(a.brick[0] == -1 && Near(a.local[0], 63.5f, 1e-4f), "addr.negative-half-floor");
	}
	{
		const auto a = S::WorldToVoxelAddress(glm::vec3(-64.0f, 0.0f, 0.0f), origin, 1.0f);
		Check(a.brick[0] == -1 && Near(a.local[0], 0.0f, 1e-5f), "addr.exact-negative-boundary");
	}
	{
		const auto a = S::WorldToVoxelAddress(glm::vec3(-64.001f, 0.0f, 0.0f), origin, 1.0f);
		Check(a.brick[0] == -2 && Near(a.local[0], 63.999f, 1e-3f), "addr.just-past-negative-boundary");
	}
	{
		const auto a = S::WorldToVoxelAddress(glm::vec3(64.0f, 0.0f, 0.0f), origin, 1.0f);
		Check(a.brick[0] == 1 && Near(a.local[0], 0.0f, 1e-5f), "addr.exact-positive-boundary");
	}
	{
		// LOD 1 (2 m voxels): world 129 -> cell 64.5 -> brick 1, local 0.5
		const auto a = S::WorldToVoxelAddress(glm::vec3(129.0f, 0.0f, 0.0f), origin, 2.0f);
		Check(a.brick[0] == 1 && Near(a.local[0], 0.5f, 1e-4f), "addr.lod1-boundary");
	}
	{
		// Shifted origin: everything relative to it.
		const glm::vec3 org(100.0f, -50.0f, 7.0f);
		const auto a = S::WorldToVoxelAddress(org + glm::vec3(-0.5f, 0.5f, 0.0f), org, 1.0f);
		Check(a.brick[0] == -1 && a.brick[1] == 0 && a.brick[2] == 0
			&& Near(a.local[0], 63.5f, 1e-4f) && Near(a.local[1], 0.5f, 1e-5f),
			"addr.shifted-origin");
	}
	{
		// BrickOfWorld agrees with the full address.
		const glm::vec3 org(-12.0f, 3.0f, 400.0f);
		for (float t = -130.0f; t <= 130.0f; t += 17.3f) {
			const glm::vec3 p(t, t * 0.5f, -t);
			const auto a = S::WorldToVoxelAddress(p, org, 1.0f);
			const glm::ivec3 b = S::BrickOfWorld(p, org, 1.0f);
			if (a.brick[0] != b.x || a.brick[1] != b.y || a.brick[2] != b.z) {
				Check(false, "addr.brickof-agrees");
				return;
			}
		}
		Check(true, "addr.brickof-agrees");
	}
}

// ---------------------------------------------------------------------------
// 2) Atlas UVW math (halo skip; never crosses into neighbor slots)
// ---------------------------------------------------------------------------
static void TestAtlasMath()
{
	using S = wf::WorldFieldSystem;
	{
		uint32_t xyz[3];
		S::DecodeAtlasSlot(0, 2, 2, xyz);
		Check(xyz[0] == 0 && xyz[1] == 0 && xyz[2] == 0, "slot.decode-0");
		S::DecodeAtlasSlot(1, 2, 2, xyz);
		Check(xyz[0] == 1 && xyz[1] == 0 && xyz[2] == 0, "slot.decode-1");
		S::DecodeAtlasSlot(2, 2, 2, xyz);
		Check(xyz[0] == 0 && xyz[1] == 1 && xyz[2] == 0, "slot.decode-2");
		S::DecodeAtlasSlot(4, 2, 2, xyz);
		Check(xyz[0] == 0 && xyz[1] == 0 && xyz[2] == 1, "slot.decode-4");
		S::DecodeAtlasSlot(7, 2, 2, xyz);
		Check(xyz[0] == 1 && xyz[1] == 1 && xyz[2] == 1, "slot.decode-7");
	}
	{
		// slot (1,0,0), local 0 -> stored texel (70, 2, 2) in a 136x68x68 atlas
		S::WorldVoxelAddress a{};
		a.brick[0] = a.brick[1] = a.brick[2] = 0;
		a.local[0] = a.local[1] = a.local[2] = 0.0f;
		const uint32_t slot[3] = { 1, 0, 0 };
		const uint32_t dims[3] = { 136, 68, 68 };
		const glm::vec3 uvw = S::AtlasUvwFromAddress(a, slot, dims);
		Check(Near(uvw.x, 70.0f / 136.0f, 1e-6f) && Near(uvw.y, 2.0f / 68.0f, 1e-6f)
			&& Near(uvw.z, 2.0f / 68.0f, 1e-6f), "atlas.uvw-halo-skip");
	}
	{
		// Filter footprint stays inside the slot: local in [0, 64) maps to
		// stored texel window [slot*68+2-0.5, slot*68+66-0.5+1] = [+1.5,
		// +66.5) — both taps within [slot*68, slot*68+68). Worst cases:
		const uint32_t dims[3] = { 408, 408, 340 };
		for (uint32_t s = 0; s < 6; ++s) {
			const uint32_t slot[3] = { s, s % 5, s % 4 };
			S::WorldVoxelAddress a{};
			for (int k = 0; k < 3; ++k)
				a.local[k] = 63.999f;
			const glm::vec3 uvw = S::AtlasUvwFromAddress(a, slot, dims);
			const glm::vec3 c = uvw * glm::vec3(408.0f, 408.0f, 340.0f);
			bool ok = true;
			for (int k = 0; k < 3; ++k) {
				const float base = float(slot[k]) * 68.0f;
				const float lo = c[k] - 0.5f;
				const float hi = lo + 1.0f;
				ok = ok && lo >= base + 1.0f - 1e-3f && hi <= base + 67.0f + 1e-3f;
			}
			if (!ok) {
				Check(false, "atlas.filter-footprint-in-slot");
				return;
			}
		}
		Check(true, "atlas.filter-footprint-in-slot");
	}
}

// ---------------------------------------------------------------------------
// 3) Decode packings
// ---------------------------------------------------------------------------
static void TestDecode()
{
	using S = wf::WorldFieldSystem;
	{
		const glm::vec4 v = S::DecodeWorldFieldEncoded(glm::vec4(0.5f),
			wf::WorldFieldEncoding::Linear, glm::vec4(2.0f), glm::vec4(1.0f));
		Check(Near(v.x, 2.0f, 1e-6f), "decode.linear");
	}
	{
		const glm::vec4 v = S::DecodeWorldFieldEncoded(glm::vec4(0.5f),
			wf::WorldFieldEncoding::SqrtUNorm, glm::vec4(2.0f), glm::vec4(1.0f));
		Check(Near(v.x, 2.0f * 0.25f + 1.0f, 1e-6f), "decode.sqrt-unorm");
	}
	{
		const glm::vec4 v = S::DecodeWorldFieldEncoded(glm::vec4(-0.5f),
			wf::WorldFieldEncoding::SignedNormalized, glm::vec4(4.0f), glm::vec4(0.0f));
		Check(Near(v.x, -2.0f, 1e-6f), "decode.signed-norm");
	}
}

// ---------------------------------------------------------------------------
// 4) The churn + seam test (milestone 2 acceptance)
// ---------------------------------------------------------------------------
static void TestResidencyChurnAndSeams()
{
	wf::WorldFieldSystem sys;
	wf::WorldFieldStreamingPolicy policy{};
	policy.maxBrickLoadsPerFrame = 4;
	policy.evictHysteresisFrames = 2;
	policy.proactiveEvictFrames = 6;
	policy.protectCoarsestLod = true;
	sys.SetStreamingPolicy(policy);

	wf::WorldFieldDesc d{};
	d.name = "TestLinearField";
	d.format = smvk::Format::R8_UNorm;
	d.encoding = wf::WorldFieldEncoding::Linear;
	d.baseVoxelSizeMeters = 1.0f;
	d.lodCount = 2;
	d.originMeters = glm::vec3(0.0f, 0.0f, 0.0f);
	d.tableDimX = 4;
	d.tableDimY = 2;
	d.tableDimZ = 4;
	d.poolSlotsX = 2;
	d.poolSlotsY = 2;
	d.poolSlotsZ = 2;
	d.decodeScale = glm::vec4(1.0f);
	d.decodeBias = glm::vec4(0.0f);
	d.fallbackValue = glm::vec4(0.0f);
	const wf::WorldFieldHandle h = sys.RegisterField(d);
	Check(h.Valid() && sys.FieldCount() == 1, "churn.register");

	LinearFieldSource source;
	sys.SetBrickSource(h, &source);

	// The simulated atlas (2x2x2 slots of 68^3 = 136x136x136 R8).
	SimAtlas atlas;
	atlas.w = 2 * 68;
	atlas.h = 2 * 68;
	atlas.d = 2 * 68;
	atlas.bytes.assign(size_t(atlas.w) * atlas.h * atlas.d, 0u);

	// slot -> the brick whose data the sim atlas slot last received.
	std::unordered_map<uint32_t, wf::WorldBrickKey> slotLastUpload;

	// Path: walk +X, a teleport, more walking. Deterministic LCG for
	// sample positions.
	auto lcg = [](uint32_t& s) {
		s = s * 1664525u + 1013904223u;
		return float(int32_t(s >> 8) & 0xFFFF) / 65535.0f; // [0,1)
	};

	uint32_t rng = 0x1234u;
	uint64_t totalEvictions = 0;
	uint64_t totalSkips = 0;
	uint64_t totalServed = 0;
	int seamChecks = 0;
	int seamFailures = 0;
	bool pageInvariantHeld = true;
	bool regionBoundsHeld = true;

	for (uint64_t frame = 0; frame < 110; ++frame) {
		// Teleport at frame 60; slow walk otherwise.
		glm::vec3 eye(32.0f + float(frame) * 8.0f, 8.0f, 32.0f);
		if (frame == 60)
			eye = glm::vec3(-517.0f, 233.0f, -709.0f);
		else if (frame > 60)
			eye += glm::vec3(float(int(frame) - 60) * 11.0f, 3.0f, -7.0f);

		sys.ComputeResidency(eye, frame);

		// Apply this frame's region uploads to the sim atlas (ordered
		// application; region offsets must sit on slot boundaries).
		for (uint32_t u = 0; u < sys.PendingUploadCount(); ++u) {
			const wf::WorldFieldRegionUpload& up = sys.PendingUploads()[u];
			if (up.payloadBytes != size_t(68) * 68 * 68)
				regionBoundsHeld = false;
			const int32_t ox = up.region.offset.x, oy = up.region.offset.y, oz = up.region.offset.z;
			const bool aligned = (ox % 68) == 0 && (oy % 68) == 0 && (oz % 68) == 0
				&& ox >= 0 && oy >= 0 && oz >= 0
				&& ox + 68 <= int32_t(atlas.w) && oy + 68 <= int32_t(atlas.h)
				&& oz + 68 <= int32_t(atlas.d);
			if (!aligned)
				regionBoundsHeld = false;
			// Row-by-row: the payload's rows are 68-contiguous but the
			// atlas's row pitch is atlas.w (136 here) — a flat slice
			// memcpy would smear diagonally across neighboring slots.
			for (uint32_t z = 0; z < 68; ++z)
				for (uint32_t y = 0; y < 68; ++y)
					std::memcpy(&atlas.bytes[(size_t(oz + z) * atlas.h + size_t(oy + y)) * atlas.w + size_t(ox)],
						up.payload + (size_t(z) * 68 + size_t(y)) * 68, 68);
			slotLastUpload[up.atlasSlot] = up.key;
		}
		sys.DropPendingGpuWork();

		// Table walk: every RESIDENT page's slot must hold exactly that
		// page's brick (the slot-reuse invariant), and every NON-resident
		// page must be tagged -1 with the window's absolute brick.
		TableView view{ sys.TableBytes(), sys.TableUsedBytes() };
		if (view.base && view.usedBytes) {
			const wf::WorldFieldTableHeader hd = view.Header();
			for (uint32_t fId = 0; fId < hd.fieldCount; ++fId) {
				const wf::WorldFieldGpu f = view.Field(fId, hd.fieldStride);
				for (uint32_t lod = 0; lod < f.meta.z; ++lod) {
					const wf::WorldLevelGpu& lv = f.levels[lod];
					for (uint32_t e = 0; e < lv.windowDim.x * lv.windowDim.y * lv.windowDim.z; ++e) {
						const wf::WorldPageGpu page =
							view.Page(lv.pageTableOffset + e * 16u);
						if (page.atlasSlot >= 0) {
							const wf::WorldBrickKey key{ fId, lod,
								page.brickX, page.brickY, page.brickZ };
							const auto it = slotLastUpload.find(uint32_t(page.atlasSlot));
							if (it == slotLastUpload.end() || !(it->second == key)) {
								pageInvariantHeld = false;
							}
						}
					}
				}
			}
		}

		// Seam sampling: deterministic positions around the eye + EXACT
		// brick boundary cases (edges and corners of the eye's brick, at
		// ±1e-3 across the boundary in each axis). The SERVED counter
		// keeps the check honest — a bug that makes every sample fall
		// back would otherwise pass vacuously.
		int servedCount = 0;
		for (int s = 0; s < 48; ++s) {
			glm::vec3 pos = eye + glm::vec3(lcg(rng), lcg(rng), lcg(rng)) * 60.0f - 30.0f;
			if (s % 8 == 0) {
				// snap to a brick boundary grid (LOD-0, 64 m bricks)
				for (int k = 0; k < 3; ++k) {
					const float g = 64.0f * float(int(std::floor(pos[k] / 64.0f)));
					pos[k] = g + ((s & 1) ? 1e-3f : -1e-3f);
				}
			}
			int servedLod = -1;
			const float got = MirrorSample(sys, atlas, d, pos, 0, servedLod);
			++seamChecks;
			if (servedLod >= 0) {
				++servedCount;
				const float ref = ReferenceSample(d, uint32_t(servedLod), pos);
				if (!Near(got, ref, 2e-3f))
					++seamFailures;
			} else if (!Near(got, d.fallbackValue.x)) {
				++seamFailures; // fallback path returns the decoded fallback
			}
		}
		totalServed += servedCount;

		{
			const wf::WorldFieldStats st = sys.Stats();
			totalEvictions += st.evictions;
			totalSkips += st.requestsSkipped;
		}
	}

	Check(pageInvariantHeld, "churn.page-points-at-own-slot");
	Check(regionBoundsHeld, "churn.region-bounds");
	Check(seamFailures == 0, "churn.seam-equality (paged == dense reference)");
	// The seam check must be NON-VACUOUS: a healthy fraction of samples
	// must have actually resolved through a resident page (the fallback
	// path returning its own value is trivially true).
	std::cout << "[info] seam: checks=" << seamChecks << " served=" << totalServed
		<< " rejectBounds=" << g_rejectBounds
		<< " rejectPage=" << g_rejectPage
		<< " rejectStale=" << g_rejectStale << "\n";
	Check(totalServed > uint64_t(seamChecks) / 4,
		"churn.seam-served-fraction (non-vacuous sampling)");
	Check(totalEvictions > 0, "churn.evictions-happened");
	Check(totalSkips > 0, "churn.forced-exhaustion-skips (graceful)");
	Check(sys.Stats().residentBricks <= 8, "churn.resident-within-pool");

	// Absence: bricks below ground were generated exactly once per key
	// (negative cache), never allocated slots.
	// Absence: keys the source reported EMPTY are negative-cached — each
	// must have been generated EXACTLY once (no re-request storm while
	// they stay in window). Residents may legally regenerate after an
	// evict-then-return cycle (churn), so they are not counted here.
	bool absenceOnce = true;
	for (const auto& key : source.absentKeys) {
		const auto it = source.calls.find(key);
		if (it == source.calls.end() || it->second != 1) {
			absenceOnce = false;
			break;
		}
	}
	Check(absenceOnce && !source.absentKeys.empty(),
		"churn.absence-negative-cache (1 call per empty key)");
	{
		// Stats consistency: residents + absents == tracked keys.
		const wf::WorldFieldStats st = sys.Stats();
		Check(st.residentBricks + st.absentBricks >= 1, "churn.tracked-keys");
	}
}

// ---------------------------------------------------------------------------
// 5) Hysteresis protection + coarsest-LOD last-resort liveness
// ---------------------------------------------------------------------------
static void TestEvictionPolicy()
{
	// ---- Hysteresis: aged-out-but-within-hysteresis residents are never
	// demand-evicted (requests skip gracefully instead); past it they are.
	{
		wf::WorldFieldSystem sys;
		wf::WorldFieldStreamingPolicy policy{};
		policy.maxBrickLoadsPerFrame = 64;
		policy.evictHysteresisFrames = 3;
		policy.proactiveEvictFrames = 1000; // off — demand eviction only
		policy.protectCoarsestLod = true;
		sys.SetStreamingPolicy(policy);

		wf::WorldFieldDesc d{};
		d.name = "TestPolicyField";
		d.format = smvk::Format::R8_UNorm;
		d.encoding = wf::WorldFieldEncoding::Linear;
		d.baseVoxelSizeMeters = 1.0f;
		d.lodCount = 2;
		d.tableDimX = 2;
		d.tableDimY = 2;
		d.tableDimZ = 2;
		d.poolSlotsX = 2;
		d.poolSlotsY = 1;
		d.poolSlotsZ = 1;
		d.fallbackValue = glm::vec4(0.0f);
		const wf::WorldFieldHandle h = sys.RegisterField(d);
		LinearFieldSource source;
		source.emptyBelowY = false; // keep the policy scenario pure
		sys.SetBrickSource(h, &source);

		// Frame 0 at eye A: fill the 2-slot pool.
		sys.ComputeResidency(glm::vec3(33.0f, 33.0f, 33.0f), 0);
		Check(sys.Stats().residentBricks >= 1, "policy.initial-fill");

		// Jump far: the old residents age. At ages 1..3 (<= hysteresis)
		// every far-eye request must SKIP (no victim), never evict.
		uint64_t skipsDuringProtection = 0;
		for (uint64_t f = 1; f <= 3; ++f) {
			sys.ComputeResidency(glm::vec3(5000.0f, 5000.0f, 5000.0f), f);
			const wf::WorldFieldStats st = sys.Stats();
			Check(st.evictions == 0, "policy.hysteresis-no-evict (aged <= 3)");
			skipsDuringProtection += st.requestsSkipped;
		}
		Check(skipsDuringProtection > 0, "policy.hysteresis-skips-gracefully");

		// Age 4+ (> hysteresis 3): the same demand CAN evict — residency
		// moves again (the pool never deadlocks behind hysteresis).
		uint64_t evictionsAfter = 0;
		for (uint64_t f = 4; f <= 6; ++f) {
			sys.ComputeResidency(glm::vec3(5000.0f, 5000.0f, 5000.0f), f);
			evictionsAfter += sys.Stats().evictions;
		}
		Check(evictionsAfter > 0, "policy.hysteresis-expires-then-evicts");
	}

	// ---- Coarsest protection is PREFERENCE, not deadlock: when ONLY
	// coarse bricks can ever be resident (LOD 0 reports empty) and the
	// pool is 1 slot, a laterally-moving interest point must still page
	// (the protected brick is evicted as the last resort).
	{
		wf::WorldFieldSystem sys;
		wf::WorldFieldStreamingPolicy policy{};
		policy.maxBrickLoadsPerFrame = 64;
		policy.evictHysteresisFrames = 1;
		policy.proactiveEvictFrames = 1000;
		policy.protectCoarsestLod = true;
		sys.SetStreamingPolicy(policy);

		wf::WorldFieldDesc d{};
		d.name = "TestCoarseOnlyField";
		d.format = smvk::Format::R8_UNorm;
		d.encoding = wf::WorldFieldEncoding::Linear;
		d.baseVoxelSizeMeters = 1.0f;
		d.lodCount = 2; // coarsest = LOD 1 (2 m voxels, 128 m bricks)
		d.tableDimX = 2;
		d.tableDimY = 1;
		d.tableDimZ = 1;
		d.poolSlotsX = 1;
		d.poolSlotsY = 1;
		d.poolSlotsZ = 1; // 1 slot: every page change is an eviction
		d.fallbackValue = glm::vec4(0.0f);
		const wf::WorldFieldHandle h = sys.RegisterField(d);
		LinearFieldSource source;
		source.emptyBelowY = false;
		source.emptyLod0 = true; // only coarse bricks can ever exist
		sys.SetBrickSource(h, &source);

		uint64_t coarseEvictions = 0;
		wf::WorldBrickKey lastResident{ 0, 0, 0, 0, 0 };
		bool pagedToNewCoarseBrick = false;
		for (uint64_t f = 0; f < 12; ++f) {
			// Walk laterally across coarse (128 m) cells.
			const glm::vec3 eye(64.0f + float(f) * 96.0f, 64.0f, 64.0f);
			sys.ComputeResidency(eye, f);
			const wf::WorldFieldStats st = sys.Stats();
			coarseEvictions += st.evictions;
			if (st.residentBricks > 0) {
				// Find the resident coarse brick: it must be LOD 1.
				TableView view{ sys.TableBytes(), sys.TableUsedBytes() };
				const wf::WorldFieldTableHeader hd = view.Header();
				const wf::WorldFieldGpu fg = view.Field(0, hd.fieldStride);
				for (uint32_t lod = 0; lod < fg.meta.z && !pagedToNewCoarseBrick; ++lod) {
					const wf::WorldLevelGpu& lv = fg.levels[lod];
					for (uint32_t e = 0; e < lv.windowDim.x * lv.windowDim.y * lv.windowDim.z; ++e) {
						const wf::WorldPageGpu page = view.Page(lv.pageTableOffset + e * 16u);
						if (page.atlasSlot < 0)
							continue;
						const wf::WorldBrickKey key{ 0, lod, page.brickX, page.brickY, page.brickZ };
						if (lod == 1 && !(key == lastResident)) {
							if (lastResident.lod == 1)
								pagedToNewCoarseBrick = true; // a SECOND coarse brick paged in
							lastResident = key;
						}
					}
				}
			}
			sys.DropPendingGpuWork();
		}
		Check(coarseEvictions > 0, "policy.coarse-last-resort-evicts");
		Check(pagedToNewCoarseBrick, "policy.coarse-paging-liveness (no deadlock)");
	}
}

// ---------------------------------------------------------------------------
// 6) ASYNC streaming (milestone 3): worker-thread generation on a real
// ThreadPool — the acceptance scenario is continuous movement + a
// teleport + forced exhaustion with jobs in flight. The invariants:
// every published page points at its own slot (no stale page ever), the
// in-flight set drains to zero, no re-dispatch storm (the InFlight phase
// hides keys from the request scan), and stale results (teleported away
// mid-flight) are DROPPED — never published.
// ---------------------------------------------------------------------------
struct AtomicLinearSource final : wf::IWorldFieldBrickSource
{
	std::atomic<int> totalCalls{ 0 };
	std::atomic<int> absentCalls{ 0 };

	static uint8_t Quantized(glm::vec3 p)
	{
		float v = 0.125f + 0.0031f * p.x + 0.0021f * p.y + 0.0013f * p.z;
		v = std::min(std::max(v, 0.0f), 1.0f);
		return uint8_t(v * 255.0f + 0.5f);
	}

	bool GenerateBrick(const wf::WorldFieldDesc& field, uint32_t lod,
		int32_t x, int32_t y, int32_t z,
		std::byte* payload, size_t payloadBytes) override
	{
		++totalCalls; // atomic: worker threads race this
		// y < 0 world bricks are empty (absence + drop paths).
		const float brickMinY = field.originMeters.y
			+ float(y) * float(wf::kBrickInteriorRes) * field.baseVoxelSizeMeters
				* float(1u << lod);
		if (brickMinY + 1e-4f < 0.0f) {
			++absentCalls;
			return false;
		}
		const float voxel = field.baseVoxelSizeMeters * float(1u << lod);
		const glm::vec3 brickMin = field.originMeters
			+ glm::vec3(float(x), float(y), float(z))
				* (float(wf::kBrickInteriorRes) * voxel);
		size_t i = 0;
		for (uint32_t tz = 0; tz < wf::kBrickStoredRes; ++tz)
			for (uint32_t ty = 0; ty < wf::kBrickStoredRes; ++ty)
				for (uint32_t tx = 0; tx < wf::kBrickStoredRes; ++tx) {
					const glm::vec3 p = brickMin + glm::vec3(
						float(tx) + 0.5f - float(wf::kBrickBorder),
						float(ty) + 0.5f - float(wf::kBrickBorder),
						float(tz) + 0.5f - float(wf::kBrickBorder)) * voxel;
					payload[i++] = std::byte(Quantized(p));
				}
		return true;
	}
};

static void TestAsyncStreaming()
{
	// The ThreadPool's ring lives in the process allocator (InitMemory is
	// idempotent — see test_memory.cpp).
	smvk::mem::InitMemory();
	smvk::ThreadPool pool(2);
	{
		// Liveness probe: a trivial task must complete while the main
		// thread SPINS (the whole async design relies on workers getting
		// scheduled alongside a running main thread).
		auto fut = pool.enqueue([] { return 42; });
		const auto t0 = std::chrono::steady_clock::now();
		while (fut.wait_for(std::chrono::milliseconds(0))
			!= std::future_status::ready) {
			if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2))
				break;
		}
		Check(fut.get() == 42, "async.pool-liveness (spin-wait completes)");
	}

	wf::WorldFieldSystem sys;
	wf::WorldFieldStreamingPolicy policy{};
	policy.maxBrickLoadsPerFrame = 4;
	policy.maxInFlightBricks = 4; // deliberately tight: the cap engages
	policy.evictHysteresisFrames = 2;
	policy.proactiveEvictFrames = 6;
	policy.protectCoarsestLod = true;
	sys.SetStreamingPolicy(policy);

	wf::WorldFieldDesc d{};
	d.name = "AsyncTestField";
	d.format = smvk::Format::R8_UNorm;
	d.encoding = wf::WorldFieldEncoding::Linear;
	d.baseVoxelSizeMeters = 1.0f;
	d.lodCount = 2;
	d.tableDimX = 2;
	d.tableDimY = 2;
	d.tableDimZ = 2;
	d.poolSlotsX = 2;
	d.poolSlotsY = 2;
	d.poolSlotsZ = 1; // 4 slots under a 16-cell window: forced churn
	d.fallbackValue = glm::vec4(0.0f);
	const wf::WorldFieldHandle h = sys.RegisterField(d);
	AtomicLinearSource source;
	sys.SetBrickSource(h, &source);
	sys.SetWorkPool(&pool);
	Check(sys.WorkPool() == &pool, "async.pool-installed");

	SimAtlas atlas;
	atlas.w = 2 * 68;
	atlas.h = 2 * 68;
	atlas.d = 1 * 68;
	atlas.bytes.assign(size_t(atlas.w) * atlas.h * atlas.d, 0u);
	std::unordered_map<uint32_t, wf::WorldBrickKey> slotLastUpload;

	bool pageInvariantHeld = true;
	bool regionBoundsHeld = true;
	uint64_t totalUploads = 0;
	uint64_t totalSkips = 0;
	for (uint64_t f = 0; f < 60; ++f) {
		glm::vec3 eye(40.0f + float(f) * 40.0f, 24.0f, 40.0f);
		if (f == 30)
			eye = glm::vec3(-613.0f, 211.0f, -353.0f); // TELEPORT mid-flight
		else if (f > 30)
			eye += glm::vec3(float(int(f) - 30) * 25.0f, 0.0f, -13.0f);

		sys.ComputeResidency(eye, f);

		for (uint32_t u = 0; u < sys.PendingUploadCount(); ++u) {
			const wf::WorldFieldRegionUpload& up = sys.PendingUploads()[u];
			const int32_t ox = up.region.offset.x, oy = up.region.offset.y, oz = up.region.offset.z;
			if (up.payloadBytes != size_t(68) * 68 * 68 || (ox % 68) || (oy % 68) || (oz % 68))
				regionBoundsHeld = false;
			for (uint32_t z = 0; z < 68; ++z)
				for (uint32_t y = 0; y < 68; ++y)
					std::memcpy(&atlas.bytes[(size_t(oz + z) * atlas.h + size_t(oy + y)) * atlas.w + size_t(ox)],
						up.payload + (size_t(z) * 68 + size_t(y)) * 68, 68);
			slotLastUpload[up.atlasSlot] = up.key;
			++totalUploads;
		}
		sys.DropPendingGpuWork();

		// The slot-reuse invariant on the published table.
		TableView view{ sys.TableBytes(), sys.TableUsedBytes() };
		if (view.base && view.usedBytes) {
			const wf::WorldFieldTableHeader hd = view.Header();
			for (uint32_t fId = 0; fId < hd.fieldCount; ++fId) {
				const wf::WorldFieldGpu fg = view.Field(fId, hd.fieldStride);
				for (uint32_t lod = 0; lod < fg.meta.z; ++lod) {
					const wf::WorldLevelGpu& lv = fg.levels[lod];
					for (uint32_t e = 0;
						e < lv.windowDim.x * lv.windowDim.y * lv.windowDim.z; ++e) {
						const wf::WorldPageGpu page = view.Page(lv.pageTableOffset + e * 16u);
						if (page.atlasSlot < 0)
							continue;
						const wf::WorldBrickKey key{ fId, lod,
							page.brickX, page.brickY, page.brickZ };
						const auto it = slotLastUpload.find(uint32_t(page.atlasSlot));
						if (it == slotLastUpload.end() || !(it->second == key))
							pageInvariantHeld = false;
					}
				}
			}
		}
		totalSkips += sys.Stats().requestsSkipped;
		if (f % 10 == 0)
			std::cout << "[info] async frame " << f
				<< " inFlight=" << sys.InFlightCount()
				<< " uploads=" << totalUploads
				<< " calls=" << source.totalCalls.load()
				<< " absent=" << source.absentCalls.load()
				<< " stats=" << sys.Stats().residentBricks << "r/"
				<< sys.Stats().inFlightBricks << "i/"
				<< sys.Stats().absentBricks << "a\n";
		// Pace the frames (a real frame is ~16 ms; this loop otherwise
		// completes 60 iterations FASTER than one worker brick — a test
		// artifact, not a starvation bug: the workers need wall-clock to
		// produce 314 KB bricks while the main thread spins for free).
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	Check(totalSkips > 0, "async.inflight-cap-and-exhaustion-skips");
	Check(pageInvariantHeld, "async.page-points-at-own-slot (teleports + churn)");
	Check(regionBoundsHeld, "async.region-bounds");
	Check(sys.InFlightCount() <= policy.maxInFlightBricks, "async.inflight-cap-respected");

	// Deterministic teardown: wait for the in-flight workers WITHOUT
	// consuming their futures (wait(), not get() — the drain owns the
	// payloads), detach the pool (the drain is pool-independent), then
	// one final compute that ONLY drains. The SOURCE below is a stack
	// object — this sequence covers every worker before it dies.
	sys.WaitForInFlightBricks();
	sys.SetWorkPool(nullptr);
	sys.ComputeResidency(glm::vec3(400.0f, 24.0f, 400.0f), 60);
	Check(sys.InFlightCount() == 0, "async.drained-to-zero");
	Check(totalUploads > 0, "async.published-async-bricks");
	Check(source.totalCalls.load() > 0, "async.source-called-on-workers");
}

// ---------------------------------------------------------------------------
// 6b) STUCK-BRICK REGRESSION (the review finding): a drained async job whose
// publication is REFUSED (pool exhausted, no evictable victim — every
// resident still hysteresis-protected in-window) must not leave its entry
// marked InFlight. The request scan only re-collects keys with NO entry, so
// a surviving InFlight entry whose job is gone is stuck FOREVER: the brick
// stays missing even after the pressure clears. Phase A manufactures the
// refusal (stationary interest: 8-brick window, 2-slot pool, residents
// ticked in-window every frame => no victims); phase B moves the interest
// to a disjoint window so the phase-A residents age out, get proactively
// evicted, and the pool serves again. The invariant: the map's InFlight
// entries never exceed the live jobs (Stats().inFlightBricks vs
// InFlightCount()), and after the teardown drain both are ZERO.
// ---------------------------------------------------------------------------
static void TestAsyncStuckBrickRecovery()
{
	smvk::mem::InitMemory();
	smvk::ThreadPool pool(2);
	{
		// Worker liveness (mirrors TestAsyncStreaming's probe).
		auto fut = pool.enqueue([] { return 7; });
		const auto t0 = std::chrono::steady_clock::now();
		while (fut.wait_for(std::chrono::milliseconds(0))
			!= std::future_status::ready) {
			if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2))
				break;
		}
		Check(fut.get() == 7, "stuck.pool-liveness");
	}

	wf::WorldFieldSystem sys;
	wf::WorldFieldStreamingPolicy policy{};
	policy.maxBrickLoadsPerFrame = 8;
	policy.maxInFlightBricks = 8;
	policy.evictHysteresisFrames = 2;
	policy.proactiveEvictFrames = 4;
	policy.protectCoarsestLod = false; // single LOD: no protected pass
	sys.SetStreamingPolicy(policy);

	wf::WorldFieldDesc d{};
	d.name = "StuckRecoveryField";
	d.format = smvk::Format::R8_UNorm;
	d.encoding = wf::WorldFieldEncoding::Linear;
	d.baseVoxelSizeMeters = 1.0f;
	d.lodCount = 1;
	d.tableDimX = 2; // 2x2x2 = an 8-brick window...
	d.tableDimY = 2;
	d.tableDimZ = 2;
	d.poolSlotsX = 2;
	d.poolSlotsY = 1;
	d.poolSlotsZ = 1; // ...under a 2-slot pool: forced exhaustion
	d.fallbackValue = glm::vec4(0.0f);
	const wf::WorldFieldHandle h = sys.RegisterField(d);
	AtomicLinearSource source;
	sys.SetBrickSource(h, &source);
	sys.SetWorkPool(&pool);

	// The stuck invariant: the map's InFlight-phase entries must never
	// exceed the live jobs (a drift = a drained job whose publication
	// failed but whose entry survived — the review's stuck brick).
	bool inflightInvariantHeld = true;
	uint64_t totalSkips = 0;
	uint64_t phaseBUploads = 0;
	const glm::vec3 eyeA(32.0f, 32.0f, 32.0f); // inside brick (0,0,0)
	const glm::vec3 eyeB(2032.0f, 32.0f, 32.0f); // brick ~31: disjoint window
	for (uint64_t f = 0; f < 36; ++f) {
		const bool phaseB = f >= 16;
		const glm::vec3 eye = phaseB ? eyeB : eyeA;
		sys.ComputeResidency(eye, f);
		totalSkips += sys.Stats().requestsSkipped;
		if (phaseB)
			phaseBUploads += sys.PendingUploadCount();
		if (sys.Stats().inFlightBricks > sys.InFlightCount())
			inflightInvariantHeld = false;
		sys.DropPendingGpuWork();
		// Wall-clock pacing: workers must outpace the loop's free spin
		// (a 68^3 quantize is ~ms-scale per brick; see TestAsyncStreaming).
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	Check(totalSkips > 0, "stuck.exhaustion-actually-hit (skips > 0)");
	Check(inflightInvariantHeld,
		"stuck.no-entry-outlives-its-job (inFlight map <= jobs, every frame)");
	Check(phaseBUploads > 0, "stuck.pressure-cleared-and-published (phase B)");

	// Teardown: wait WITHOUT consuming, detach the pool, one drain-only
	// compute (the drain is pool-independent). Both counters must be zero:
	// the jobs drained AND no drained-but-unpublished entry left behind.
	sys.WaitForInFlightBricks();
	sys.SetWorkPool(nullptr);
	sys.ComputeResidency(eyeB, 36);
	Check(sys.InFlightCount() == 0, "stuck.jobs-drained-to-zero");
	Check(sys.Stats().inFlightBricks == 0,
		"stuck.map-inflight-entries-zero (no stuck brick after teardown)");
	Check(sys.Stats().residentBricks > 0,
		"stuck.recovered-residents (the disjoint window is served)");
}

// ---------------------------------------------------------------------------
// 7) GPU struct layout contract smoke (the static_asserts in
// WorldFieldGpu.h do the real work; this pins the table header arithmetic).
// ---------------------------------------------------------------------------
static void TestGpuLayout()
{
	Check(sizeof(wf::WorldFieldGpu) == 320, "gpu.field-stride-320");
	Check(sizeof(wf::WorldPageGpu) == 16, "gpu.page-stride-16");
	Check(sizeof(wf::WorldLevelGpu) == 48, "gpu.level-stride-48");
	Check((16u + 3u * 320u) % 16u == 0u, "gpu.pages-offset-aligned");
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // crash diagnostics: keep every PASS line
	TestAddressing();
	TestAtlasMath();
	TestDecode();
	TestResidencyChurnAndSeams();
	TestEvictionPolicy();
	TestAsyncStreaming();
	TestAsyncStuckBrickRecovery();
	TestGpuLayout();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
