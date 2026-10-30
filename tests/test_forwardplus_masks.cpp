// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// forwardplus_masks_tests: the NO-MISSING-CONTRIBUTOR contract of the
// DEFAULT forward+ path's hierarchical light culling (FFXVI tiled/zBin
// phase-1 artifacts consumed by forwardplus.cs.hlsl). Pure CPU, no GPU:
// the phase-1 builders are math (deferredshadow.h) and the tile-side
// reference below mirrors the shader's exact tests.
//
// The contract under test (the paper's own invariant: "no missing
// lights; false positives are acceptable"):
//
//   1. zBin windows are conservative supersets of the shader's exact
//      depth cuts: every light passing the exact plane + slab tests (the
//      surface track) must lie in the slab window, and every light
//      passing plane + march-range (the fog track) must lie in the
//      [0, fogMax] window. Both sides use the same weakly-monotone bin
//      mapping, so this is exact — a violation means the mask path
//      skips a light the linear fallback keeps.
//
//   2. The coarse tile mask never drops a CONTRIBUTING light: a light
//      within range of any point a shaded surface / march sample can
//      occupy for the tile (sampled over the tile's pixel rays x its
//      depth interval) must carry its bit in the tile's group-rect OR.
//      This is provable per reject: a group-plane reject (dot < -range)
//      puts every point of the group's pyramid beyond the light's
//      range, and a projected-rect miss means no sphere point projects
//      into the group — either way zero radiance reaches any pixel of
//      the tile. What the mask MAY drop (and the linear scan keeps) is
//      the plane test's own zero-contribution false positives: the
//      center-based -range side-plane test does not compose across
//      nested wedges for far-side (typically behind-camera) centers —
//      a marginal such light passes the TILE's planes while failing its
//      GROUP's, and it lights nothing (see the failing-case analysis in
//      the change history: tile plane dot -13.98 vs group -14.39 at
//      range 14.19, center 13 m BEHIND the camera).
//
// Edge coverage forced by the sweeps: light counts 0/1/31/32/33/255/256/
// 1023/1024 (group-word boundaries + bit 31 + the last word), uneven and
// tiny viewports (partial groups), near-plane straddling + infinite
// ranges (the full-grid mask fallback), fully-behind cameras, sky slabs
// (farthest ~ 1e6), and degenerate slabs (nearest == farthest).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// rendergraph.h's Pool members are not self-contained: the includer must
// pull Engine/Memory/pool.h in first (same pattern as
// test_deferredshadow.cpp).
#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/deferredshadow.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

// ---------------------------------------------------------------------------
// Camera / light synthesis (same conventions as test_deferredshadow.cpp:
// the app's Y-flipped projection, view looking down -z).
// ---------------------------------------------------------------------------

glm::mat4 TestView(const glm::vec3& eye, float yaw, float pitch) {
	const glm::vec3 f(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
		-std::cos(pitch) * std::cos(yaw));
	const glm::vec3 r = normalize(cross(f, glm::vec3(0, 1, 0)));
	const glm::vec3 u = normalize(cross(r, f));
	glm::mat4 v(1.0f);
	v[0] = glm::vec4(r, -dot(r, eye));
	v[1] = glm::vec4(u, -dot(u, eye));
	v[2] = glm::vec4(-f, dot(f, eye));
	return v;
}

glm::mat4 TestProj(float fovY, float aspect, float nearZ, float farZ) {
	const float fy = 1.0f / std::tan(fovY * 0.5f);
	glm::mat4 p(0.0f);
	p[0][0] = fy / aspect;
	p[1][1] = -fy; // framebuffer y down (matches the app's Y-flipped proj)
	p[2][2] = farZ / (nearZ - farZ);
	p[2][3] = -1.0f;
	p[3][2] = -(farZ * nearZ) / (farZ - nearZ);
	return p;
}

std::vector<smvk::ForwardPlusLight> RandomLights(std::mt19937& rng,
	uint32_t count, const glm::vec3& lo, const glm::vec3& hi)
{
	std::uniform_real_distribution<float> px(lo.x, hi.x);
	std::uniform_real_distribution<float> py(lo.y, hi.y);
	std::uniform_real_distribution<float> pz(lo.z, hi.z);
	// range picks: 0 (infinite sentinel), small, large, huge (spheres
	// that straddle the near plane -> the mask builder's full-grid
	// fallback path).
	std::uniform_int_distribution<int> rangeKind(0, 3);
	std::uniform_real_distribution<float> rangeVal(0.5f, 40.0f);
	std::vector<smvk::ForwardPlusLight> lights;
	lights.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		smvk::ForwardPlusLight l{};
		l.position = { px(rng), py(rng), pz(rng) };
		switch (rangeKind(rng)) {
		case 0: l.range = 0.0f; break;
		case 1: l.range = rangeVal(rng); break;
		case 2: l.range = 80.0f + rangeVal(rng) * 4.0f; break;
		default: l.range = 1e7f; break;
		}
		l.intensity = 1.0f;
		l.type = (i & 1) ? smvk::kForwardPlusSpot : smvk::kForwardPlusPoint;
		lights.push_back(l);
	}
	return lights;
}

// ---------------------------------------------------------------------------
// Tile-side reference: verbatim mirrors of forwardplus.cs.hlsl's exact
// per-light tests + the shader's zBin window lookup (DsZBinRange).
// ---------------------------------------------------------------------------

glm::vec3 RefUnprojectView(const glm::vec2& ndc, const glm::mat4& invProj) {
	const glm::vec4 v = invProj * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
	return glm::vec3(v) / std::max(v.w, 1e-9f);
}

// The 4 inward-facing origin planes of a pixel rect (the shader's cross
// order, verified there by hand: top/bottom/left/right).
struct TilePlanes {
	glm::vec3 n[4];
};

TilePlanes RefTilePlanes(uint32_t px0, uint32_t py0, uint32_t px1, uint32_t py1,
	uint32_t w, uint32_t h, const glm::mat4& invProj)
{
	const float wf = std::max(float(w), 1.0f);
	const float hf = std::max(float(h), 1.0f);
	const float x0 = (float(px0) / wf) * 2.0f - 1.0f;
	const float x1 = (float(px1) / wf) * 2.0f - 1.0f;
	const float y0 = (float(py0) / hf) * 2.0f - 1.0f;
	const float y1 = (float(py1) / hf) * 2.0f - 1.0f;
	const glm::vec3 c00 = RefUnprojectView(glm::vec2(x0, y0), invProj);
	const glm::vec3 c10 = RefUnprojectView(glm::vec2(x1, y0), invProj);
	const glm::vec3 c11 = RefUnprojectView(glm::vec2(x1, y1), invProj);
	const glm::vec3 c01 = RefUnprojectView(glm::vec2(x0, y1), invProj);
	TilePlanes p;
	p.n[0] = normalize(cross(c00, c10)); // top
	p.n[1] = normalize(cross(c11, c01)); // bottom
	p.n[2] = normalize(cross(c01, c00)); // left
	p.n[3] = normalize(cross(c10, c11)); // right
	return p;
}

// DsBinFromDist mirror (the shader computes tile bins with
// nearP = max(nearPlane, 1e-4); the builder used the raw near — the
// engine's near planes are >> 1e-4 so the two agree, and so do these).
uint32_t RefBinFromDist(float dist, float nearP, float farP) {
	const float span = std::max(farP - nearP, 1e-4f);
	const float t = std::clamp((dist - nearP) / span, 0.0f, 1.0f);
	return (uint32_t)(t * float(smvk::kFpZBinCount - 1));
}

// DsZBinRange mirror over the built bin quadlets {min,max,rangedMin,
// rangedMax}. Empty windows encode lo > hi.
void RefZBinWindow(const std::vector<uint32_t>& bins, uint32_t bin0,
	uint32_t bin1, uint32_t lightCount, uint32_t& lo, uint32_t& hi)
{
	bin0 = std::min(bin0, smvk::kFpZBinCount - 1u);
	bin1 = std::min(bin1, smvk::kFpZBinCount - 1u);
	if (bin0 > bin1)
		std::swap(bin0, bin1);
	const uint32_t ax = bins[bin0 * 4 + 0], ay = bins[bin0 * 4 + 1];
	const uint32_t aw = bins[bin0 * 4 + 3];
	const uint32_t bx = bins[bin1 * 4 + 0], by = bins[bin1 * 4 + 1];
	const uint32_t bz = bins[bin1 * 4 + 2];
	if (bin1 - bin0 <= 1u) {
		lo = std::min(ax, bx);
		hi = std::max(ay, by);
	}
	else {
		lo = bz; // rangedMin at the far bin (prefix running min)
		hi = aw; // rangedMax at the near bin (suffix running max)
	}
	if (lo > hi || hi >= lightCount) {
		lo = 1u;
		hi = 0u;
	}
}

// ---------------------------------------------------------------------------
// The invariant sweep.
// ---------------------------------------------------------------------------

struct SweepStats {
	uint64_t tiles = 0;
	uint64_t surfaceKept = 0;   // lights passing the exact plane+slab tests
	uint64_t surfaceContrib = 0; // of those-adjacent: sampled true contributors
	uint64_t fogKept = 0;       // lights passing the exact plane+march tests
	uint64_t fogContrib = 0;    // sampled march-sample contributors
};

// One trial: a camera, an extent, a light set. Returns true when the
// invariant holds for every sampled tile / slab / fog range.
bool SweepTrial(std::mt19937& rng, uint32_t w, uint32_t h,
	uint32_t lightCount, uint32_t tileSize, uint32_t maxTiles,
	SweepStats& stats, std::string& firstFailure)
{
	const float nearZ = 0.1f + float(rng() % 90) * 0.01f;   // 0.1 .. 1.0
	const float farZ = 120.0f + float(rng() % 1800);        // 120 .. 1920
	std::uniform_real_distribution<float> yawDist(-3.1f, 3.1f);
	std::uniform_real_distribution<float> pitchDist(-1.2f, 1.2f);
	const glm::vec3 eye(float(rng() % 40) - 20.0f, float(rng() % 12) - 6.0f,
		float(rng() % 40) - 20.0f);
	const glm::mat4 view = TestView(eye, yawDist(rng), pitchDist(rng));
	const float aspect = float(w) / float(std::max(h, 1u));
	const glm::mat4 proj = TestProj(0.6f + float(rng() % 100) * 0.008f,
		aspect, nearZ, farZ);
	const glm::mat4 invProj = glm::inverse(proj);

	// Lights around the camera (in front, behind, straddling).
	std::vector<smvk::ForwardPlusLight> lights = RandomLights(rng, lightCount,
		eye - glm::vec3(60.0f, 30.0f, 60.0f), eye + glm::vec3(60.0f, 30.0f, 60.0f));
	// One light exactly AT the camera (crosses every plane boundary).
	if (!lights.empty()) {
		smvk::ForwardPlusLight atEye = lights[0];
		atEye.position = eye;
		atEye.range = 8.0f;
		lights[0] = atEye;
	}

	// Phase 1, exactly as UpdateForwardPlusLights runs it.
	smvk::DsSortLightsByNearDistance(lights, view);
	std::vector<uint32_t> zbins;
	smvk::DsBuildZBins(lights, view, nearZ, farZ, zbins);
	std::vector<uint32_t> masks;
	uint32_t gcx = 0, gcy = 0;
	smvk::DsBuildTileMasks(lights, view, proj, w, h, masks, gcx, gcy);
	const uint32_t n = uint32_t(lights.size());
	const float nearP = std::max(nearZ, 1e-4f);

	// Tile sampling: all tiles when few, otherwise a random sample plus
	// the border rows/columns (partial-group edge tiles).
	const uint32_t tcx = (w + tileSize - 1) / tileSize;
	const uint32_t tcy = (h + tileSize - 1) / tileSize;
	std::vector<std::pair<uint32_t, uint32_t>> tiles;
	for (uint32_t ty = 0; ty < tcy; ++ty)
		for (uint32_t tx = 0; tx < tcx; ++tx)
			if (tx == 0 || ty == 0 || tx == tcx - 1 || ty == tcy - 1)
				tiles.push_back({ tx, ty });
	if (tiles.size() < maxTiles) {
		std::uniform_int_distribution<uint32_t> txDist(0, tcx - 1);
		std::uniform_int_distribution<uint32_t> tyDist(0, tcy - 1);
		while (tiles.size() < maxTiles)
			tiles.push_back({ txDist(rng), tyDist(rng) });
		std::sort(tiles.begin(), tiles.end());
		tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
	}

	// Precompute per-light view-space data (sorted order).
	struct LightView {
		glm::vec3 cView;
		float range;
	};
	std::vector<LightView> lv(n);
	for (uint32_t i = 0; i < n; ++i) {
		lv[i].cView = view * glm::vec4(
			glm::vec3(lights[i].position[0], lights[i].position[1],
				lights[i].position[2]), 1.0f);
		lv[i].range = (lights[i].range <= 0.0f)
			? smvk::kDsInfiniteRange : lights[i].range;
	}

	// Slab intervals per tile: random pairs + the directed edges (sky
	// tile with farthest ~ 1e6, degenerate nearest == farthest, slab
	// pinned at the near plane).
	std::uniform_real_distribution<float> slabDist(nearZ, farZ);
	const float fogMaxs[4] = { 5.0f, 40.0f, 80.0f, 500.0f };

	for (const auto& [tx, ty] : tiles) {
		const uint32_t px0 = tx * tileSize;
		const uint32_t py0 = ty * tileSize;
		const uint32_t px1 = std::min(px0 + tileSize, w);
		const uint32_t py1 = std::min(py0 + tileSize, h);
		const TilePlanes planes = RefTilePlanes(px0, py0, px1, py1, w, h, invProj);

		// The tile's coarse-mask group rect + the OR of its words.
		const uint32_t mgx0 = px0 / smvk::kFpTileGroupPx;
		const uint32_t mgx1 = (px1 - 1u) / smvk::kFpTileGroupPx;
		const uint32_t mgy0 = py0 / smvk::kFpTileGroupPx;
		const uint32_t mgy1 = (py1 - 1u) / smvk::kFpTileGroupPx;
		std::vector<uint32_t> orWords(smvk::kFpMaxLightGroups, 0u);
		for (uint32_t gy = mgy0; gy <= mgy1; ++gy) {
			for (uint32_t gx = mgx0; gx <= mgx1; ++gx) {
				const size_t base =
					(size_t(gy) * gcx + gx) * (1u + smvk::kFpMaxLightGroups);
				for (uint32_t g = 0; g < smvk::kFpMaxLightGroups; ++g)
					orWords[g] |= masks[base + 1u + g];
			}
		}
		const auto maskHas = [&](uint32_t i) -> bool {
			return (orWords[i >> 5] >> (i & 31u)) & 1u;
		};

		float slabs[8][2];
		slabs[0][0] = nearZ;              slabs[0][1] = 1e6f;   // sky-adjacent
		slabs[1][0] = nearZ;              slabs[1][1] = nearZ;  // degenerate
		slabs[2][0] = slabDist(rng);      slabs[2][1] = 1e6f;   // sky far
		for (int s = 3; s < 6; ++s) {
			slabs[s][0] = slabDist(rng);
			slabs[s][1] = slabs[s][0] + slabDist(rng) * 3.0f;
		}
		slabs[6][0] = nearZ;              slabs[6][1] = farZ;   // full span
		slabs[7][0] = farZ * 0.5f;        slabs[7][1] = farZ;   // deep only

		const float wf = std::max(float(w), 1.0f);
		const float hf = std::max(float(h), 1.0f);
		// Pixel-ray directions for the tile's sample grid (4x4 positions;
		// unprojected near points double as unit-ish ray directions).
		glm::vec3 rayDirs[16];
		for (uint32_t sy = 0; sy < 4u; ++sy) {
			for (uint32_t sx = 0; sx < 4u; ++sx) {
				const float u = px0 + (px1 - px0) * ((float(sx) + 0.5f) / 4.0f);
				const float v = py0 + (py1 - py0) * ((float(sy) + 0.5f) / 4.0f);
				const glm::vec2 ndc((u / wf) * 2.0f - 1.0f, (v / hf) * 2.0f - 1.0f);
				rayDirs[sy * 4u + sx] = RefUnprojectView(ndc, invProj);
			}
		}
		const auto rayPoint = [&](uint32_t k, float depth) -> glm::vec3 {
			const glm::vec3 d = rayDirs[k];
			return d * (depth / std::max(-d.z, 1e-6f));
		};

		for (int s = 0; s < 8; ++s) {
			const float nearest = slabs[s][0];
			const float farthest = slabs[s][1];
			uint32_t lo = 1u, hi = 0u;
			RefZBinWindow(zbins,
				RefBinFromDist(nearest, nearP, farZ),
				RefBinFromDist(farthest, nearP, farZ),
				n, lo, hi);
			// Sampled contributor points: positions a shaded surface could
			// occupy for THIS tile (pixel rays x slab depths). A light
			// within range of any of them is a true contributor — the
			// mask path must never drop one (the group-plane and
			// projected-rect rejects each imply zero contribution to the
			// group's pixels; see the file header).
			glm::vec3 samples[64];
			uint32_t sampleCount = 0;
			for (uint32_t k = 0; k < 16u; ++k)
				for (uint32_t sd = 0; sd < 4u; ++sd)
					samples[sampleCount++] = rayPoint(k,
						nearest + (farthest - nearest) * ((float(sd) + 0.5f) / 4.0f));
			for (uint32_t i = 0; i < n; ++i) {
				const glm::vec3 c = lv[i].cView;
				const float r = lv[i].range;
				if (c.z > r)
					continue; // fully behind the camera plane
				bool outside = false;
				for (int p = 0; p < 4; ++p) {
					if (dot(planes.n[p], c) < -r) {
						outside = true;
						break;
					}
				}
				if (outside)
					continue;
				// (a) EXACT contract: every plane+slab passer (the tile
				// cull's own exact tests, which the mask path runs on
				// every surfaced candidate) must lie inside the zBin
				// window — the window is a conservative superset of the
				// slab test (same monotone binning both sides).
				if (-c.z + r >= nearest && -c.z - r <= farthest) {
					++stats.surfaceKept;
					if (!(lo <= i && i <= hi)) {
						firstFailure = "surface: light " + std::to_string(i)
							+ " passes the exact plane+slab tests but is "
							+ "outside the zBin window [" + std::to_string(lo)
							+ "," + std::to_string(hi) + "] (tile "
							+ std::to_string(tx) + "," + std::to_string(ty)
							+ ", slab s" + std::to_string(s) + ")";
						return false;
					}
				}
				// (b) Contributor contract: within range of any sampled
				// surface point (or the center itself sits inside the
				// tile pyramid + slab) -> the coarse-mask bit MUST be set.
				bool contributor = false;
				if (-c.z >= nearest && -c.z <= farthest) {
					bool centerInside = true;
					for (int p = 0; p < 4; ++p)
						centerInside = centerInside && dot(planes.n[p], c) >= 0.0f;
					contributor = centerInside;
				}
				for (uint32_t sm = 0; !contributor && sm < sampleCount; ++sm) {
					const glm::vec3 d = samples[sm] - c;
					contributor = dot(d, d) <= r * r;
				}
				if (contributor) {
					++stats.surfaceContrib;
					if (!maskHas(i)) {
						firstFailure = "surface: light " + std::to_string(i)
							+ " CONTRIBUTES to tile " + std::to_string(tx)
							+ "," + std::to_string(ty) + " (slab s"
							+ std::to_string(s) + ") but has no coarse-mask "
							+ "bit — a missing-light regression";
						return false;
					}
				}
			}
		}

		// Fog track: window over [0, fogMax], NOT the slab window.
		for (const float fogMax : fogMaxs) {
			uint32_t lo = 1u, hi = 0u;
			RefZBinWindow(zbins, 0u,
				RefBinFromDist(fogMax, nearP, farZ), n, lo, hi);
			// March-sample contributor points: pixel rays x depths across
			// [0, fogMax] (the march starts at the camera).
			glm::vec3 samples[64];
			uint32_t sampleCount = 0;
			for (uint32_t k = 0; k < 16u; ++k)
				for (uint32_t sd = 0; sd < 4u; ++sd)
					samples[sampleCount++] = rayPoint(k,
						std::max(0.05f, fogMax * ((float(sd) + 0.5f) / 4.0f)));
			for (uint32_t i = 0; i < n; ++i) {
				const glm::vec3 c = lv[i].cView;
				const float r = lv[i].range;
				if (c.z > r)
					continue;
				bool outside = false;
				for (int p = 0; p < 4; ++p) {
					if (dot(planes.n[p], c) < -r) {
						outside = true;
						break;
					}
				}
				if (outside)
					continue;
				const float d = -c.z;
				// (a) EXACT: plane + march-range passers inside the fog
				// window.
				if (d - r <= fogMax) {
					++stats.fogKept;
					if (!(lo <= i && i <= hi)) {
						firstFailure = "fog: light " + std::to_string(i)
							+ " passes the exact plane+march tests but is "
							+ "outside the fog zBin window ["
							+ std::to_string(lo) + "," + std::to_string(hi)
							+ "] (tile " + std::to_string(tx) + ","
							+ std::to_string(ty) + ", fogMax "
							+ std::to_string(fogMax) + ")";
						return false;
					}
				}
				// (b) Contributor: within range of any march sample point.
				bool contributor = false;
				for (uint32_t sm = 0; !contributor && sm < sampleCount; ++sm) {
					const glm::vec3 dd = samples[sm] - c;
					contributor = dot(dd, dd) <= r * r;
				}
				if (contributor) {
					++stats.fogContrib;
					if (!maskHas(i)) {
						firstFailure = "fog: light " + std::to_string(i)
							+ " CONTRIBUTES to tile " + std::to_string(tx)
							+ "," + std::to_string(ty) + " (fogMax "
							+ std::to_string(fogMax) + ") but has no "
							+ "coarse-mask bit — a missing-light regression";
						return false;
					}
				}
			}
		}
		++stats.tiles;
	}
	return true;
}

} // namespace

int main() {
	std::mt19937 rng(20261030u);

	// ---- Directed edge counts (bit 31, word 1, the last word, caps) ----
	{
		SweepStats stats;
		bool ok = true;
		std::string first;
		const uint32_t counts[] = { 0, 1, 31, 32, 33, 255, 256, 1023, 1024 };
		for (uint32_t count : counts) {
			if (!SweepTrial(rng, 1920, 1080, count, 16, 96, stats, first)) {
				ok = false;
				std::cout << "  (count=" << count << ") " << first << "\n";
				break;
			}
		}
		Check(ok, "Edge light counts (0/1/31/32/33/255/256/1023/1024): "
			"zBin windows conservative + no missing contributors");
	}

	// ---- Randomized sweep: extents (incl. uneven + single-group),
	//      tile sizes, cameras, light sets ----
	{
		SweepStats stats;
		bool ok = true;
		std::string first;
		const uint32_t extents[][2] = {
			{ 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
			{ 1361, 837 },   // uneven (partial groups everywhere)
			{ 64, 64 },      // exactly one mask group
			{ 65, 129 },     // 2x3 groups, 1px partials
			{ 100, 100 },
		};
		const uint32_t tileSizes[] = { 16, 16, 8, 32 };
		for (int trial = 0; trial < 48; ++trial) {
			const auto& e = extents[trial % 7];
			const uint32_t tileSize = tileSizes[trial % 4];
			// Mostly small counts (fast), a few dense crowds.
			const uint32_t count = (trial % 11 == 0)
				? 512u + rng() % 512u : 1u + rng() % 96u;
			if (!SweepTrial(rng, e[0], e[1], count, tileSize, 64, stats, first)) {
				ok = false;
				std::cout << "  (trial=" << trial << " extent=" << e[0] << "x"
					<< e[1] << " lights=" << count << ") " << first << "\n";
				break;
			}
		}
		Check(ok, "Randomized sweep (48 trials, 7 extents, 3 tile sizes): "
			"zBin windows conservative + no missing contributors");
		std::cout << "  sweep stats: " << stats.tiles << " tiles, "
			<< stats.surfaceKept << " exact surface passers (window-checked), "
			<< stats.surfaceContrib << " surface contributors (bit-checked), "
			<< stats.fogKept << " exact fog passers, "
			<< stats.fogContrib << " fog contributors\n";
	}

	// ---- Fully-behind camera: masks all zero, windows empty ----
	{
		glm::mat4 view = TestView(glm::vec3(0, 0, 0), 0.0f, 0.0f); // looks -z
		std::vector<smvk::ForwardPlusLight> behind;
		for (int i = 0; i < 64; ++i) {
			smvk::ForwardPlusLight l{};
			l.position = { float(i), 0.0f, 40.0f + float(i) }; // all +z: behind
			l.range = 10.0f;
			behind.push_back(l);
		}
		smvk::DsSortLightsByNearDistance(behind, view);
		std::vector<uint32_t> zbins;
		smvk::DsBuildZBins(behind, view, 0.5f, 90.0f, zbins);
		std::vector<uint32_t> masks;
		uint32_t gx = 0, gy = 0;
		smvk::DsBuildTileMasks(behind, view, TestProj(1.0f, 16.0f / 9.0f, 0.5f, 90.0f),
			1920, 1080, masks, gx, gy);
		bool masksZero = true;
		for (uint32_t m : masks)
			masksZero = masksZero && m == 0u;
		bool binsEmpty = true;
		for (uint32_t b = 0; b < smvk::kFpZBinCount; ++b) {
			// every bin: min sentinel (= n) > max (= 0)
			binsEmpty = binsEmpty && zbins[b * 4 + 0] >= 64u
				&& zbins[b * 4 + 1] == 0u;
		}
		uint32_t lo = 1u, hi = 0u;
		RefZBinWindow(zbins, 0u, 1023u, 64u, lo, hi);
		Check(masksZero && binsEmpty && lo > hi,
			"All-behind camera: zero masks + empty zBin windows");
	}

	// ---- Zero lights: fresh-zero artifacts (the upload-every-frame
	//      discipline — the cull's mask path files only the probe tail) ----
	{
		glm::mat4 view = TestView(glm::vec3(0, 0, 5), 0.3f, -0.1f);
		std::vector<smvk::ForwardPlusLight> none;
		smvk::DsSortLightsByNearDistance(none, view);
		std::vector<uint32_t> zbins;
		smvk::DsBuildZBins(none, view, 0.5f, 90.0f, zbins);
		std::vector<uint32_t> masks;
		uint32_t gx = 0, gy = 0;
		smvk::DsBuildTileMasks(none, view, TestProj(1.0f, 16.0f / 9.0f, 0.5f, 90.0f),
			1361, 837, masks, gx, gy);
		bool zero = !masks.empty();
		for (uint32_t m : masks)
			zero = zero && m == 0u;
		uint32_t lo = 1u, hi = 0u;
		RefZBinWindow(zbins, 0u, 100u, 0u, lo, hi);
		Check(zero && lo > hi && zbins.size() == 4 * smvk::kFpZBinCount,
			"Zero lights: all-zero masks + empty windows (fresh-zero upload)");
	}

	// ---- Push-struct mirror: the C++ side asserts its own size; pin the
	//      phase-1 tail field offsets (the HLSL mirror must land zbins at
	//      96, tileMasks at 104, counts 112/116 — the 8-byte alignment
	//      after fogListsWanted at 88 pads 92..96, matching HLSL uint64
	//      packing). ----
	{
		const smvk::ForwardPlusCullPush pc{};
		const auto off = [&pc](const void* member) -> size_t {
			return size_t(reinterpret_cast<const unsigned char*>(member)
				- reinterpret_cast<const unsigned char*>(&pc));
		};
		Check(off(&pc.zbins) == 96 && off(&pc.tileMasks) == 104
			&& off(&pc.maskLightCount) == 112
			&& off(&pc.maskGroupCountX) == 116,
			"ForwardPlusCullPush phase-1 field offsets (96/104/112/116)");
	}

	std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << " ("
		<< (g_checks - g_failures) << "/" << g_checks << ")\n";
	return g_failures == 0 ? 0 : 1;
}
