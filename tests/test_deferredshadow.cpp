// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// deferredshadow_tests: CPU-side differential regression tests for the
// phase-1 builders (Engine/Rendergraph/deferredshadow.h). No GPU
// required: zBins/masks/sort are pure math.
//
// The optimized builders must be BIT-IDENTICAL to the straightforward
// reference implementations below (copied from the pre-optimization
// code):
//   - DsSortLightsByNearDistance: decorate-sort-undecorate with an
//     index tiebreak must reproduce the old stable_sort order (the GPU
//     light buffer and every zBin index refer to this order).
//   - DsBuildZBins: the watermark minIdx fill (O(n + bins)) must match
//     the old per-light prefix rescan (O(n * bins)).
//   - DsBuildTileMasks: the cached group planes must match a fresh
//     rebuild, including across proj/extent changes (cache invalidation)
//     and repeated calls (cache hit).
// Randomized light sets cover the edge cases: empty input, all lights
// behind the camera, infinite ranges (range <= 0), key ties, and
// > kDsMaxLightGroups * 32 lights (the mask builder's clamp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

// rendergraph.h's Pool members are not self-contained: the includer must
// pull Engine/Memory/pool.h in first (same pattern as GI/bake.h).
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
// Reference implementations (verbatim pre-optimization algorithms).
// ---------------------------------------------------------------------------

float RefNearSortKey(const smvk::ForwardPlusLight& L, const glm::mat4& view) {
	const float range = (L.range <= 0.0f) ? smvk::kDsInfiniteRange : L.range;
	const glm::vec3 cView = view * glm::vec4(
		glm::vec3(L.position[0], L.position[1], L.position[2]), 1.0f);
	const float z = -cView.z;
	if (z + range <= 0.0f)
		return std::numeric_limits<float>::max();
	return std::max(z - range, 0.0f);
}

void RefSortLightsByNearDistance(std::vector<smvk::ForwardPlusLight>& lights,
	const glm::mat4& view)
{
	std::stable_sort(lights.begin(), lights.end(),
		[&](const smvk::ForwardPlusLight& a, const smvk::ForwardPlusLight& b) {
			return RefNearSortKey(a, view) < RefNearSortKey(b, view);
		});
}

void RefBuildZBins(const std::vector<smvk::ForwardPlusLight>& sortedLights,
	const glm::mat4& view, float nearPlane, float farPlane,
	std::vector<uint32_t>& outBins)
{
	outBins.assign(4 * smvk::kDsZBinCount, 0);
	const uint32_t n = (uint32_t)sortedLights.size();
	std::vector<uint32_t> nearBin(n, smvk::kDsZBinCount), farBin(n, 0);
	const float span = std::max(farPlane - nearPlane, 1e-4f);
	const auto binOf = [&](float dist) -> uint32_t {
		const float t = std::clamp((dist - nearPlane) / span, 0.0f, 1.0f);
		return (uint32_t)(t * float(smvk::kDsZBinCount - 1));
	};
	for (uint32_t i = 0; i < n; ++i) {
		const smvk::ForwardPlusLight& L = sortedLights[i];
		const float range = (L.range <= 0.0f) ? smvk::kDsInfiniteRange : L.range;
		const glm::vec3 cView = view * glm::vec4(
			glm::vec3(L.position[0], L.position[1], L.position[2]), 1.0f);
		const float z = -cView.z;
		if (z + range <= 0.0f)
			continue;
		nearBin[i] = binOf(std::max(z - range, 0.0f));
		farBin[i] = binOf(z + range);
	}

	std::vector<uint32_t> minIdx(smvk::kDsZBinCount, n);
	for (uint32_t i = 0; i < n; ++i) {
		if (nearBin[i] == smvk::kDsZBinCount)
			continue;
		for (uint32_t b = 0; b <= farBin[i]; ++b)
			if (minIdx[b] == n)
				minIdx[b] = i;
	}
	std::vector<uint32_t> maxIdx(smvk::kDsZBinCount, 0);
	{
		uint32_t j = 0;
		for (uint32_t b = 0; b < smvk::kDsZBinCount; ++b) {
			while (j < n && nearBin[j] <= b)
				++j;
			maxIdx[b] = (j > 0) ? j - 1 : 0;
		}
	}
	for (uint32_t b = 0; b < smvk::kDsZBinCount; ++b) {
		outBins[b * 4 + 0] = minIdx[b];
		outBins[b * 4 + 1] = maxIdx[b];
	}
	uint32_t rmin = n;
	for (uint32_t b = 0; b < smvk::kDsZBinCount; ++b) {
		rmin = std::min(rmin, minIdx[b]);
		outBins[b * 4 + 2] = rmin;
	}
	uint32_t rmax = 0;
	for (uint32_t b = smvk::kDsZBinCount; b-- > 0;) {
		rmax = std::max(rmax, maxIdx[b]);
		outBins[b * 4 + 3] = rmax;
	}
}

void RefBuildTileMasks(const std::vector<smvk::ForwardPlusLight>& lights,
	const glm::mat4& view, const glm::mat4& proj,
	uint32_t vpWidth, uint32_t vpHeight,
	std::vector<uint32_t>& outMasks,
	uint32_t& outGroupCountX, uint32_t& outGroupCountY)
{
	outGroupCountX = (vpWidth + smvk::kDsTileGroupPx - 1) / smvk::kDsTileGroupPx;
	outGroupCountY = (vpHeight + smvk::kDsTileGroupPx - 1) / smvk::kDsTileGroupPx;
	outMasks.assign(size_t(outGroupCountX) * outGroupCountY * (1 + smvk::kDsMaxLightGroups), 0);
	if (outGroupCountX == 0 || outGroupCountY == 0)
		return;

	const glm::mat4 invProj = glm::inverse(proj);
	const auto unprojectNear = [&](const glm::vec2& ndc) -> glm::vec3 {
		const glm::vec4 v = invProj * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
		return glm::vec3(v) / v.w;
	};
	const float wF = std::max(float(vpWidth), 1.0f);
	const float hF = std::max(float(vpHeight), 1.0f);

	const uint32_t n = std::min<uint32_t>(
		(uint32_t)lights.size(), smvk::kDsMaxLightGroups * 32u);
	const size_t groupCount = size_t(outGroupCountX) * outGroupCountY;
	std::vector<glm::vec3> groupPlanes(groupCount * 4);
	for (uint32_t gy = 0; gy < outGroupCountY; ++gy) {
		for (uint32_t gx = 0; gx < outGroupCountX; ++gx) {
			const float x0 = (float(gx * smvk::kDsTileGroupPx) / wF) * 2.0f - 1.0f;
			const float x1 = (float((gx + 1) * smvk::kDsTileGroupPx) / wF) * 2.0f - 1.0f;
			const float y0 = (float(gy * smvk::kDsTileGroupPx) / hF) * 2.0f - 1.0f;
			const float y1 = (float((gy + 1) * smvk::kDsTileGroupPx) / hF) * 2.0f - 1.0f;
			const glm::vec3 c00 = unprojectNear(glm::vec2(x0, y0));
			const glm::vec3 c10 = unprojectNear(glm::vec2(x1, y0));
			const glm::vec3 c11 = unprojectNear(glm::vec2(x1, y1));
			const glm::vec3 c01 = unprojectNear(glm::vec2(x0, y1));
			glm::vec3* planes = &groupPlanes[(size_t(gy) * outGroupCountX + gx) * 4];
			planes[0] = glm::normalize(glm::cross(c00, c10));
			planes[1] = glm::normalize(glm::cross(c11, c01));
			planes[2] = glm::normalize(glm::cross(c01, c00));
			planes[3] = glm::normalize(glm::cross(c10, c11));
		}
	}
	for (uint32_t i = 0; i < n; ++i) {
		const smvk::ForwardPlusLight& L = lights[i];
		const float range = (L.range <= 0.0f) ? smvk::kDsInfiniteRange : L.range;
		const glm::vec3 pos(L.position[0], L.position[1], L.position[2]);
		const glm::vec3 cView = view * glm::vec4(pos, 1.0f);
		if (cView.z - range > 0.0f)
			continue;

		float ndcMinX = std::numeric_limits<float>::max();
		float ndcMaxX = std::numeric_limits<float>::lowest();
		float ndcMinY = std::numeric_limits<float>::max();
		float ndcMaxY = std::numeric_limits<float>::lowest();
		bool crossesNear = false;
		for (int sz = -1; sz <= 1; sz += 2) {
			for (int sy = -1; sy <= 1; sy += 2) {
				for (int sx = -1; sx <= 1; sx += 2) {
					const glm::vec3 corner = cView + range * glm::vec3(float(sx), float(sy), float(sz));
					const glm::vec4 clip = proj * glm::vec4(corner, 1.0f);
					if (clip.w <= 1e-6f) {
						crossesNear = true;
						break;
					}
					const glm::vec3 ndc = glm::vec3(clip) / clip.w;
					ndcMinX = std::min(ndcMinX, ndc.x);
					ndcMaxX = std::max(ndcMaxX, ndc.x);
					ndcMinY = std::min(ndcMinY, ndc.y);
					ndcMaxY = std::max(ndcMaxY, ndc.y);
				}
				if (crossesNear) break;
			}
			if (crossesNear) break;
		}

		uint32_t gx0 = 0, gy0 = 0, gx1 = outGroupCountX - 1, gy1 = outGroupCountY - 1;
		if (!crossesNear) {
			auto px = [](float ndc, float size) { return (ndc * 0.5f + 0.5f) * size; };
			const float pMinX = px(ndcMinX, wF), pMaxX = px(ndcMaxX, wF);
			const float pMinY = px(ndcMinY, hF), pMaxY = px(ndcMaxY, hF);
			const auto groupOf = [](float p, uint32_t count) -> uint32_t {
				const int32_t g = (int32_t)std::floor(p / float(smvk::kDsTileGroupPx));
				return (uint32_t)std::clamp(g, 0, int32_t(count - 1));
			};
			gx0 = groupOf(pMinX, outGroupCountX);
			gx1 = groupOf(pMaxX, outGroupCountX);
			gy0 = groupOf(pMinY, outGroupCountY);
			gy1 = groupOf(pMaxY, outGroupCountY);
		}

		const uint32_t lightGroup = i / 32u;
		const uint32_t lightBit = 1u << (i % 32u);
		for (uint32_t gy = gy0; gy <= gy1; ++gy) {
			for (uint32_t gx = gx0; gx <= gx1; ++gx) {
				const glm::vec3* planes =
					&groupPlanes[(size_t(gy) * outGroupCountX + gx) * 4];
				bool outside = false;
				for (int p = 0; p < 4; ++p) {
					if (dot(planes[p], cView) < -range) {
						outside = true;
						break;
					}
				}
				if (outside)
					continue;
				const size_t base = (size_t(gy) * outGroupCountX + gx) * (1 + smvk::kDsMaxLightGroups);
				outMasks[base] |= 1u << lightGroup;
				outMasks[base + 1 + lightGroup] |= lightBit;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Randomized light sets.
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
	// range picks: 0 (infinite sentinel), small, large, huge (lights whose
	// sphere reaches far past the far plane / the camera plane).
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

void CompareBins(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
	const char* name)
{
	bool same = a.size() == b.size();
	for (size_t i = 0; same && i < a.size(); ++i)
		same = a[i] == b[i];
	Check(same, name);
}

} // namespace

int main() {
	std::mt19937 rng(20260911);

	// ---- Sort: decorate-sort-undecorate == old stable_sort order ----
	{
		glm::mat4 view = TestView(glm::vec3(3.0f, 4.0f, 12.0f), 0.4f, -0.2f);
		bool allSame = true;
		for (int trial = 0; trial < 32; ++trial) {
			std::vector<smvk::ForwardPlusLight> ref =
				RandomLights(rng, 1 + (trial * 37) % 1024,
					glm::vec3(-30, -10, -30), glm::vec3(30, 20, 30));
			std::vector<smvk::ForwardPlusLight> got = ref;
			RefSortLightsByNearDistance(ref, view);
			smvk::DsSortLightsByNearDistance(got, view);
			if (ref.size() != got.size()) { allSame = false; break; }
			for (size_t i = 0; i < ref.size(); ++i) {
				if (std::memcmp(&ref[i], &got[i], sizeof(smvk::ForwardPlusLight)) != 0) {
					allSame = false; break;
				}
			}
			if (!allSame) break;
		}
		Check(allSame, "DsSortLightsByNearDistance matches stable_sort reference (32 random sets)");
	}
	// Key ties: equal keys must keep the original relative order.
	{
		glm::mat4 view = TestView(glm::vec3(0, 0, 10), 0.0f, 0.0f);
		std::vector<smvk::ForwardPlusLight> ref(64);
		for (uint32_t i = 0; i < 64; ++i) {
			// Symmetric grid around the view axis: many equal sort keys.
			ref[i].position = { float(i % 8 - 4) * 0.001f, 0.0f, -20.0f + (i % 2) * 1e-6f };
			ref[i].range = 1.0f;
			ref[i].intensity = float(i); // tag the original order
		}
		std::vector<smvk::ForwardPlusLight> got = ref;
		RefSortLightsByNearDistance(ref, view);
		smvk::DsSortLightsByNearDistance(got, view);
		bool same = true;
		for (size_t i = 0; i < ref.size(); ++i)
			same = same && ref[i].intensity == got[i].intensity;
		Check(same, "DsSortLightsByNearDistance preserves stable order on key ties");
	}
	{
		// Edge cases: empty and single-light inputs.
		std::vector<smvk::ForwardPlusLight> empty;
		glm::mat4 view = TestView(glm::vec3(0), 0, 0);
		smvk::DsSortLightsByNearDistance(empty, view);
		Check(empty.empty(), "DsSortLightsByNearDistance handles empty input");
		std::vector<smvk::ForwardPlusLight> one = RandomLights(rng, 1,
			glm::vec3(-5), glm::vec3(5));
		smvk::DsSortLightsByNearDistance(one, view);
		Check(one.size() == 1, "DsSortLightsByNearDistance handles single light");
	}

	// ---- zBins: watermark fill == old prefix rescan ----
	{
		glm::mat4 view = TestView(glm::vec3(3.0f, 4.0f, 12.0f), 0.4f, -0.2f);
		bool allSame = true;
		for (int trial = 0; trial < 32; ++trial) {
			std::vector<smvk::ForwardPlusLight> lights =
				RandomLights(rng, (trial * 61) % 1025,
					glm::vec3(-40, -15, -40), glm::vec3(40, 25, 40));
			// zBins consume the SORTED order.
			smvk::DsSortLightsByNearDistance(lights, view);
			std::vector<uint32_t> refBins, gotBins;
			RefBuildZBins(lights, view, 0.5f, 90.0f, refBins);
			smvk::DsBuildZBins(lights, view, 0.5f, 90.0f, gotBins);
			bool same = refBins.size() == gotBins.size();
			for (size_t i = 0; same && i < refBins.size(); ++i)
				same = refBins[i] == gotBins[i];
			if (!same) { allSame = false; break; }
		}
		Check(allSame, "DsBuildZBins matches prefix-rescan reference (32 random sets, incl. n=0/1024)");
	}
	{
		// All lights fully behind the camera (dead sentinel path).
		glm::mat4 view = TestView(glm::vec3(0, 0, 0), 0.0f, 0.0f);
		std::vector<smvk::ForwardPlusLight> behind = RandomLights(rng, 512,
			glm::vec3(-20, -5, -60), glm::vec3(20, 5, -10)); // all z < 0 (behind)
		std::vector<uint32_t> refBins, gotBins;
		RefBuildZBins(behind, view, 0.5f, 90.0f, refBins);
		smvk::DsBuildZBins(behind, view, 0.5f, 90.0f, gotBins);
		CompareBins(refBins, gotBins, "DsBuildZBins all-dead lights match reference");
	}

	// ---- TileMasks: cached planes == fresh rebuild ----
	{
		glm::mat4 view = TestView(glm::vec3(3.0f, 4.0f, 12.0f), 0.4f, -0.2f);
		bool allSame = true;
		for (int trial = 0; trial < 16; ++trial) {
			std::vector<smvk::ForwardPlusLight> lights =
				RandomLights(rng, 1 + (trial * 97) % 1100,
					glm::vec3(-30, -10, -30), glm::vec3(30, 20, 30));
			const uint32_t w = 256 + (trial * 137) % 1600;
			const uint32_t h = 256 + (trial * 89) % 900;
			glm::mat4 proj = TestProj(1.1f, float(w) / float(h), 0.5f, 90.0f);
			std::vector<uint32_t> refMasks, gotMasks;
			uint32_t rgx = 0, rgy = 0, ggx = 0, ggy = 0;
			RefBuildTileMasks(lights, view, proj, w, h, refMasks, rgx, rgy);
			smvk::DsBuildTileMasks(lights, view, proj, w, h, gotMasks, ggx, ggy);
			bool same = rgx == ggx && rgy == ggy && refMasks.size() == gotMasks.size();
			for (size_t i = 0; same && i < refMasks.size(); ++i)
				same = refMasks[i] == gotMasks[i];
			if (!same) { allSame = false; break; }
		}
		Check(allSame, "DsBuildTileMasks matches fresh-rebuild reference (16 random sets, varying extents)");
	}
	{
		// Cache behavior: same proj twice (hit), changed proj (rebuild),
		// changed extent (rebuild) — all must equal fresh reference builds.
		glm::mat4 view = TestView(glm::vec3(2, 3, 8), -0.3f, 0.15f);
		std::vector<smvk::ForwardPlusLight> lights = RandomLights(rng, 300,
			glm::vec3(-15, -5, -15), glm::vec3(15, 10, 15));
		glm::mat4 projA = TestProj(1.0f, 16.0f / 9.0f, 0.5f, 90.0f);
		glm::mat4 projB = TestProj(0.7f, 16.0f / 9.0f, 0.5f, 90.0f);
		std::vector<uint32_t> got, ref;
		uint32_t g = 0, r = 0;
		bool ok = true;
		// 1: fill the cache with (projA, 1920x1080).
		smvk::DsBuildTileMasks(lights, view, projA, 1920, 1080, got, g, g);
		RefBuildTileMasks(lights, view, projA, 1920, 1080, ref, r, r);
		for (size_t i = 0; i < ref.size(); ++i) ok = ok && ref[i] == got[i];
		// 2: cache hit (same inputs).
		smvk::DsBuildTileMasks(lights, view, projA, 1920, 1080, got, g, g);
		for (size_t i = 0; i < ref.size(); ++i) ok = ok && ref[i] == got[i];
		// 3: proj drift -> rebuild.
		smvk::DsBuildTileMasks(lights, view, projB, 1920, 1080, got, g, g);
		RefBuildTileMasks(lights, view, projB, 1920, 1080, ref, r, r);
		for (size_t i = 0; i < ref.size(); ++i) ok = ok && ref[i] == got[i];
		// 4: extent drift -> rebuild.
		smvk::DsBuildTileMasks(lights, view, projA, 1280, 720, got, g, g);
		RefBuildTileMasks(lights, view, projA, 1280, 720, ref, r, r);
		for (size_t i = 0; i < ref.size(); ++i) ok = ok && ref[i] == got[i];
		// 5: back to the original inputs (stale cache entry must not stick).
		smvk::DsBuildTileMasks(lights, view, projA, 1920, 1080, got, g, g);
		RefBuildTileMasks(lights, view, projA, 1920, 1080, ref, r, r);
		for (size_t i = 0; i < ref.size(); ++i) ok = ok && ref[i] == got[i];
		Check(ok, "DsBuildTileMasks cache hit/rebuild paths all match reference");
	}

	std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << " ("
		<< (g_checks - g_failures) << "/" << g_checks << ")\n";
	return g_failures == 0 ? 0 : 1;
}
