// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// gi_tests: CPU-side regression tests for the baked SG-GI system
// (Engine/GI): fixed-basis SG math (evaluation, closed-form product
// integrals, non-negative fitting), the triangle BVH used by the
// baker, and the live volume serializers (regular lattices, convex
// grids) + the lightmap chain. No GPU required. (The .sggi octree
// round trip is retired with the data layer — stage 3 of the octree
// retirement.)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h> // Cluster (static-bake-light harvest test)
#include <Engine/Asset/compression.h> // Lz4Compress (payload fixtures)
#include <Engine/GI/bake.h>
#include <Engine/GI/bake_inputs.h> // RecordCaptureReach (the reach sweep)
#include <Engine/GI/bc6h.h>
#include <Engine/GI/cubemap_volume.h>
#include <Engine/GI/lightmap.h>
#include <Engine/GI/regular_volume.h>
#include <Engine/GI/sg.h>
#include <Engine/GI/triangle_bvh.h>
#include <Engine/Renderer/mesh.h> // StaticMeshUpload (curvature test)

namespace {

constexpr float kPi = 3.14159265358979323846f;

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	} else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

void CheckNear(float value, float expected, float eps, const char* name) {
	Check(std::abs(value - expected) <= eps, name);
}

float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

glm::vec3 UniformDir(std::mt19937& rng) {
	std::uniform_real_distribution<float> u(0.0f, 1.0f);
	const float z = 1.0f - 2.0f * u(rng);
	const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
	const float phi = 2.0f * kPi * u(rng);
	return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

} // namespace

namespace smvk::gi {

// Test access to the fit path used by the baker.
void TestSgBasis() {
	// Coverage: every direction sees a lobe within a bounded angle.
	std::mt19937 rng(7);
	float worstDot = 1.0f;
	for (int i = 0; i < 5000; ++i) {
		const glm::vec3 d = UniformDir(rng);
		float best = -1.0f;
		for (uint32_t l = 0; l < kSgLobeCount; ++l) {
			const glm::vec3 axis(kSgBasis[l][0], kSgBasis[l][1], kSgBasis[l][2]);
			best = std::max(best, glm::dot(axis, d));
		}
		worstDot = std::min(worstDot, best);
	}
	// Covering radius < ~57 deg (9 lobes, golden-angle spiral).
	Check(worstDot > 0.54f, "sg basis covering radius");

	// Basis axes are unit length.
	bool unit = true;
	for (uint32_t l = 0; l < kSgLobeCount; ++l) {
		const glm::vec3 axis(kSgBasis[l][0], kSgBasis[l][1], kSgBasis[l][2]);
		if (std::abs(glm::length(axis) - 1.0f) > 1e-5f)
			unit = false;
	}
	Check(unit, "sg basis unit axes");

	// Peak convention: lobe(axis) == 1.
	glm::vec3 axis0(kSgBasis[0][0], kSgBasis[0][1], kSgBasis[0][2]);
	CheckNear(SgBasisValue(0, axis0), 1.0f, 1e-5f, "sg lobe peak == 1");
}

void TestSgFitRoundtrip() {
	// A field that is exactly in the basis span: the legacy NNLS path
	// must recover the amplitudes (least-squares is exact on span).
	std::mt19937 rng(11);
	glm::vec3 truth[kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		truth[i] = glm::vec3(0.2f + 0.8f * (i % 3) * 0.4f,
			0.1f + 0.05f * i, 0.3f + 0.07f * ((i * 5) % 9));
	}
	std::vector<glm::vec3> dirs, radiance;
	for (int s = 0; s < 512; ++s) {
		const glm::vec3 d = UniformDir(rng);
		dirs.push_back(d);
		radiance.push_back(SgEvaluate(truth, d));
	}
	{
		SgFitAccumulator acc;
		const float weight = 4.0f * kPi / float(dirs.size());
		for (size_t s = 0; s < dirs.size(); ++s)
			acc.AddSample(dirs[s], radiance[s], weight);
		glm::vec3 fit[kSgLobeCount];
		acc.SolveNnls(fit);
		float maxErr = 0.0f;
		float maxAmp = 0.0f;
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			maxErr = std::max(maxErr, std::abs(fit[i].r - truth[i].r));
			maxErr = std::max(maxErr, std::abs(fit[i].g - truth[i].g));
			maxErr = std::max(maxErr, std::abs(fit[i].b - truth[i].b));
			maxAmp = std::max(maxAmp, truth[i].g);
		}
		Check(maxErr < 0.02f * maxAmp + 1e-3f, "sg nnls recovers span field");
	}

	// Production diagonal projection on the same span field: blurrier
	// (overlapping lobes double-count shared energy) but finite,
	// non-negative, and bounded — stability over optimality.
	{
		glm::vec3 fit[kSgLobeCount];
		SgFitLobes(dirs, radiance, fit); // diagonal path
		bool finiteNonNeg = true;
		float maxLum = 0.0f;
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			for (int c = 0; c < 3; ++c)
				if (!std::isfinite(fit[i][c]) || fit[i][c] < 0.0f)
					finiteNonNeg = false;
			maxLum = std::max(maxLum, Luminance(fit[i]));
		}
		Check(finiteNonNeg, "sg diagonal span finite non-negative");
		Check(maxLum < 10.0f, "sg diagonal span peak bounded");
	}

	// Non-negativity holds under adversarial (negative) input: fitting a
	// field with a "negative" component still yields clamped amplitudes.
	for (glm::vec3& r : radiance)
		r *= -1.0f;
	glm::vec3 neg[kSgLobeCount];
	SgFitLobes(dirs, radiance, neg);
	bool allNonNeg = true;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		allNonNeg = allNonNeg && glm::all(glm::greaterThanEqual(neg[i], glm::vec3(0.0f)));
	Check(allNonNeg, "sg fit clamps to non-negative");
}

void TestSgSelfEnergy() {
	// Analytic self-overlap pi/lambda*(1-exp(-4 lambda)) ~= 0.785398.
	const float self = SgSelfEnergy();
	Check(std::abs(self - 0.785398f) < 1e-4f, "sg self energy analytic");
	// Empirical Gram diagonal on uniform sampling converges to it.
	std::mt19937 rng(5);
	SgFitAccumulator acc;
	const int N = 8192;
	for (int s = 0; s < N; ++s)
		acc.AddSample(UniformDir(rng), glm::vec3(1.0f), 4.0f * kPi / float(N));
	// (nearDiag, not near: windows.h's legacy empty `near` macro would
	// swallow the identifier and turn this into a syntax error — latent
	// until the first test rebuild since.)
	bool nearDiag = true;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		if (std::abs(acc.gram[i][i] - self) / self > 0.10f)
			nearDiag = false;
	Check(nearDiag, "sg gram diagonal matches self energy (<10%)");
}

void TestSgFitHighContrast() {
	// Sun-pool edge analogue: bimodal field (0 vs ~5000 across a sharp
	// horizon). The unconstrained least-squares fit rings with huge +/-
	// pairs on this input (near-singular Gram + inconsistent rhs); the
	// ringing guard must keep peaks bounded instead of emitting 10^4x
	// spikes that read as hard-edged dots at runtime.
	std::mt19937 rng(97);
	std::vector<glm::vec3> dirs, radiance;
	for (int s = 0; s < 512; ++s) {
		const glm::vec3 d = UniformDir(rng);
		dirs.push_back(d);
		radiance.push_back(d.y < 0.0f ? glm::vec3(5000.0f, 4500.0f, 4000.0f) : glm::vec3(0.0f));
	}
	glm::vec3 fit[kSgLobeCount];
	SgFitLobes(dirs, radiance, fit);
	bool finiteNonNeg = true;
	float maxLum = 0.0f;
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		for (int c = 0; c < 3; ++c) {
			if (!std::isfinite(fit[i][c]) || fit[i][c] < 0.0f)
				finiteNonNeg = false;
		}
		maxLum = std::max(maxLum, Luminance(fit[i]));
	}
	Check(finiteNonNeg, "sg fit high-contrast finite non-negative");
	Check(maxLum < 3.0f * 5000.0f, "sg fit high-contrast peak bounded");
	// Hue coherence: the input is warm-tinted (5000/4500/4000, ratio
	// 1.25); per-channel solver dynamics must not manufacture saturated
	// hues (white-world bakes measured 1500x splits from the old
	// per-channel NMF + per-channel fallback paths).
	{
		glm::vec3 mean(0.0f);
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			mean += fit[i];
		mean /= float(kSgLobeCount);
		const float mx = std::max(mean.x, std::max(mean.y, mean.z));
		const float mn = std::min(mean.x, std::min(mean.y, mean.z));
		Check(mx / std::max(mn, 1e-6f) < 5.0f, "sg fit high-contrast hue coherent");
	}
	// Downward reconstruction recovers substantial (not exact) energy.
	const glm::vec3 rec = SgEvaluate(fit, glm::vec3(0, -1, 0));
	Check(Luminance(rec) > 500.0f && Luminance(rec) < 15000.0f,
		"sg fit high-contrast recovers down energy");
}

// Analytical locks on the sun-only transport (GiTraceScene): with a
// KNOWN uniform albedo these become exact statements, not statistics.
// Any violation is a real transport bug (gain, leak, or NaN source).

namespace {

GiTraceScene MakeUniformScene(float albedo, const glm::vec3& sunDir,
	float sunIntensity, const std::vector<glm::vec3>& positions,
	const std::vector<uint32_t>& indices) {
	GiTraceScene scene;
	scene.positions = positions;
	scene.indices = indices;
	scene.materials = { GiTraceMaterial{ glm::vec3(albedo), glm::vec3(0.0f) } };
	scene.triMaterial = std::vector<uint32_t>(indices.size() / 3, 0);
	scene.sunDirection = glm::normalize(sunDir);
	scene.sunColor = glm::vec3(1.0f, 0.95f, 0.90f);
	scene.sunIntensity = sunIntensity;
	scene.Finalize();
	return scene;
}

} // namespace

void TestGiEmptyScene() {
	// No geometry: every ray misses, and a miss returns the analytic sky
	// radiance (the sky IBL is baked into the field; gradient only, no
	// sun disc). Exact SkyRadiance(dir), any direction, any depth.
	// Zeroing skyIntensity recovers the old sky-less field: exact zero.
	GiTraceScene scene = MakeUniformScene(0.7f, glm::vec3(0, 1, 0), 10000.0f, {}, {});
	Check(scene.Bvh().TriangleCount() == 0, "gi empty scene no tris");
	Pcg32 rng(3);
	bool allSky = true;
	for (int i = 0; i < 64; ++i) {
		std::mt19937 srng(uint32_t(i + 1));
		const glm::vec3 dir = UniformDir(srng);
		const glm::vec3 L = scene.TraceIncoming(
			glm::vec3(0, 0, 0), dir, rng, 3);
		const glm::vec3 sky = scene.SkyRadiance(dir);
		// TraceIncoming re-normalizes the direction; allow last-ulp.
		if (glm::length(L - sky) > 1e-4f * std::max(1.0f, glm::length(sky)))
			allSky = false;
	}
	Check(allSky, "gi empty scene traces exact sky radiance");
	// Sky above the horizon is brighter than the same-direction ground
	// gradient (sanity for the baked-in IBL actually being nonzero).
	Pcg32 upRng(7);
	Check(Luminance(scene.TraceIncoming(
		glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), upRng, 1)) > 0.0f,
		"gi empty scene sky nonzero");
	scene.skyIntensity = 0.0f;
	bool allZero = true;
	for (int i = 0; i < 64; ++i) {
		std::mt19937 srng(uint32_t(i + 1));
		const glm::vec3 L = scene.TraceIncoming(
			glm::vec3(0, 0, 0), UniformDir(srng), rng, 3);
		if (glm::length(L) != 0.0f)
			allZero = false;
	}
	Check(allZero, "gi empty scene zero-sky exact zero");
}

void TestGiPrimarySunIncluded() {
	// One big triangle; a ray that hits it as the PRIMARY vertex with no
	// bounces allowed returns EXACTLY the sunlit-surface term: the light
	// a sunlit surface reflects toward the probe is one-bounce indirect
	// at the probe (the runtime shadow maps evaluate direct sun at the
	// shaded surface itself, so nothing double-counts).
	// Expected: sunColor * (I * ndl) * albedo / pi, ndl = 1 here.
	std::vector<glm::vec3> pos = {
		{ -10, -10, 0 }, { 10, -10, 0 }, { 0, 10, 0 },
	};
	GiTraceScene scene = MakeUniformScene(0.7f, glm::vec3(0, 0, 1), 10000.0f,
		pos, { 0, 1, 2 });
	Pcg32 rng(5);
	const glm::vec3 L = scene.TraceIncoming(
		glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), rng, 0);
	const glm::vec3 expect = glm::vec3(1.0f, 0.95f, 0.90f)
		* (10000.0f * 0.7f / kPi);
	Check(glm::length(L - expect) / glm::length(expect) < 1e-5f,
		"gi primary vertex includes one-bounce sun exactly");
}

void TestGiEnergyBound() {
	// Open box (floor + 4 walls, no roof), uniform albedo A = 0.7, sun
	// overhead at intensity 10000. Per-ray bound (geometric series: the
	// primary vertex contributes <= Imax * A / pi, bounce vertex j >= 1
	// contributes <= Imax * A^(j+1) / pi):
	//   L <= Imax * A / (pi * (1 - A)) ~= 7427.
	// Asserts with margin at 8000. A gain bug (albedo > 1 creeping in,
	// throughput mishandled) blows straight through this ceiling, and any
	// NaN source trips the finiteness check (regression: 49k NaN probes).
	std::vector<glm::vec3> pos;
	std::vector<uint32_t> idx;
	auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
		const uint32_t b0 = uint32_t(pos.size());
		pos.push_back(a); pos.push_back(b); pos.push_back(c); pos.push_back(d);
		idx.push_back(b0 + 0); idx.push_back(b0 + 1); idx.push_back(b0 + 2);
		idx.push_back(b0 + 0); idx.push_back(b0 + 2); idx.push_back(b0 + 3);
	};
	quad({ -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 }); // floor
	quad({ -5, 0, -5 }, { -5, 3, -5 }, { -5, 3, 5 }, { -5, 0, 5 }); // walls
	quad({ 5, 0, -5 }, { 5, 0, 5 }, { 5, 3, 5 }, { 5, 3, -5 });
	quad({ -5, 0, -5 }, { 5, 0, -5 }, { 5, 3, -5 }, { -5, 3, -5 });
	quad({ -5, 0, 5 }, { -5, 3, 5 }, { 5, 3, 5 }, { 5, 0, 5 });
	GiTraceScene scene = MakeUniformScene(0.7f, glm::vec3(0, 1, 0), 10000.0f, pos, idx);
	Check(scene.Bvh().TriangleCount() == 10, "gi bound scene tri count");

	Pcg32 rng(11);
	std::mt19937 srng(13);
	bool bounded = true;
	bool finite = true;
	float worst = 0.0f;
	double mean = 0.0;
	const int N = 200;
	for (int i = 0; i < N; ++i) {
		const glm::vec3 L = scene.TraceIncoming(
			glm::vec3(0, 1, 0), UniformDir(srng), rng, 3);
		for (int c = 0; c < 3; ++c) {
			if (!std::isfinite(L[c]))
				finite = false;
		}
		const float l = Luminance(L);
		worst = std::max(worst, l);
		mean += l;
		if (l > 8000.0f)
			bounded = false;
	}
	mean /= N;
	std::cout << "  box-room: worst ray " << worst << ", mean " << mean << "\n";
	Check(finite, "gi bound scene all samples finite");
	Check(bounded, "gi per-ray energy bound (Imax*A/(pi*(1-A)))");
	// The sun DOES reach in through the open roof: floor primary rays
	// return ~A * I / pi ~= 2228 each, so the mean over all directions
	// must be well above zero (a zero mean here means the sun is broken).
	Check(mean > 200.0f && mean < 8000.0f, "gi box-room mean in sane range");
}

void TestSgDiffuseAgainstMonteCarlo() {
	std::mt19937 rng(23);
	glm::vec3 amps[kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		amps[i] = glm::vec3(0.3f + 0.5f * std::sin(float(i * 2.3f)),
			0.5f + 0.3f * std::cos(float(i * 1.7f)),
			0.2f + 0.4f * std::sin(float(i * 0.9f)));

	const glm::vec3 normals[4] = {
		glm::vec3(0, 1, 0), glm::vec3(0.8f, 0.6f, 0),
		glm::vec3(0, -1, 0), glm::vec3(0.577f, 0.577f, 0.577f),
	};
	bool allOk = true;
	float worstRel = 0.0f;
	for (const glm::vec3& n : normals) {
		// MC reference: E = int L(omega) (n.w)+ domega, uniform sampling.
		glm::vec3 mcSum(0.0f);
		const int N = 20000;
		for (int s = 0; s < N; ++s) {
			const glm::vec3 d = UniformDir(rng);
			const float cosP = std::max(0.0f, glm::dot(n, d));
			mcSum += SgEvaluate(amps, d) * cosP;
		}
		const glm::vec3 mcE = mcSum * (4.0f * kPi / float(N));
		const glm::vec3 sgE = SgDiffuseIrradiance(amps, n);
		const float rel = std::abs(Luminance(sgE) - Luminance(mcE))
			/ std::max(Luminance(mcE), 1e-5f);
		worstRel = std::max(worstRel, rel);
		if (rel > 0.12f)
			allOk = false;
	}
	Check(allOk, "sg diffuse product integral vs MC (<12%)");
	std::cout << "  worst relative diffuse error: " << worstRel << "\n";

	// Constant radiance field via the legacy NNLS span fit: E(n) must
	// be pi * c. (The production diagonal projection is deliberately
	// NOT exact here — overlapping lobes double-count shared energy,
	// so a diagonal constant fit overshoots. Exactness is the NNLS
	// path's property; the diagonal path trades it for stability.)
	glm::vec3 constAmps[kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		constAmps[i] = glm::vec3(1.0f); // span fit of a constant
	glm::vec3 fittedConst[kSgLobeCount];
	{
		std::vector<glm::vec3> dirs, radiance;
		for (int s = 0; s < 256; ++s) {
			dirs.push_back(UniformDir(rng));
			radiance.push_back(glm::vec3(1.0f));
		}
		SgFitAccumulator acc;
		const float weight = 4.0f * kPi / float(dirs.size());
		for (size_t s = 0; s < dirs.size(); ++s)
			acc.AddSample(dirs[s], radiance[s], weight);
		acc.SolveNnls(fittedConst);
	}
	const glm::vec3 Econst = SgDiffuseIrradiance(fittedConst, glm::vec3(0, 1, 0));
	const glm::vec3 expect(kPi);
	const float relErr = std::abs(Luminance(Econst) - Luminance(expect)) / kPi;
	Check(relErr < 0.10f, "sg constant-field irradiance == pi*c (<10%)");
	// Diagonal on the same constant input: all-positive, bounded,
	// mutually equal (isotropic input -> isotropic amplitudes).
	{
		std::mt19937 r2(23);
		std::vector<glm::vec3> ddirs;
		std::vector<glm::vec3> drad;
		for (int s = 0; s < 256; ++s) {
			ddirs.push_back(UniformDir(r2));
			drad.push_back(glm::vec3(1.0f));
		}
		glm::vec3 diag[kSgLobeCount];
		SgFitLobes(ddirs, drad, diag);
		bool pos = true;
		float mn = 1e30f, mx = -1e30f;
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			if (!std::isfinite(diag[i].x + diag[i].y + diag[i].z) || diag[i].x < 0.0f)
				pos = false;
			mn = std::min(mn, diag[i].x);
			mx = std::max(mx, diag[i].x);
		}
		Check(pos, "sg diagonal constant positive finite");
		Check(mx < 5.0f && mn > 0.1f && mx / std::max(mn, 1e-6f) < 1.5f,
			"sg diagonal constant isotropic bounded");
	}
}

void TestLmSelectorDomain() {
	// Domain + bias check for the per-mesh lobe-count selection: the
	// selector scores SgLightmapDiffuse(amps, count, n) against
	// sum w*L*max(dot(d,n),0) over the traced samples. Both must be the
	// SAME quantity — raw irradiance E = int L cos domega (the runtime
	// applies the Lambert 1/pi when multiplying albedo). A pi-domain
	// mismatch would scale every rel error by ~3.14 and pin all meshes
	// at the max lobe count; a diagonal-projection constant-field bias
	// would inflate errors at EVERY count on flat lighting — same
	// visible failure, different cause. This measures both, with
	// numbers, on the exact production functions.
	std::mt19937 rng(91);
	static constexpr uint32_t kCounts[4] = { 5, 6, 9, 12 };
	const uint32_t kRays = 128;
	glm::vec3 dirs[kRays];
	for (uint32_t i = 0; i < kRays; ++i)
		dirs[i] = UniformDir(rng);
	const float weight = 4.0f * kPi / float(kRays);
	const glm::vec3 normals[4] = {
		glm::vec3(0, 1, 0), glm::vec3(0.8f, 0.6f, 0),
		glm::vec3(0, -0.4f, 0.9f), glm::vec3(0.577f, 0.577f, 0.577f),
	};

	std::cout << "  lm selector domain (constant-white field, raw irradiance):\n";
	bool domainOk = true;
	float worstRel = 0.0f;
	for (uint32_t ci = 0; ci < 4; ++ci) {
		smvk::gi::SgLightmapFitAccumulator acc(kCounts[ci]);
		for (uint32_t i = 0; i < kRays; ++i)
			acc.AddSample(dirs[i], glm::vec3(1.0f), weight);
		glm::vec3 amps[smvk::gi::kLightmapMaxLobes];
		acc.SolveGram(amps);
		float worstThis = 0.0f;
		glm::vec3 printFit(0.0f), printTrue(0.0f);
		for (const glm::vec3& n : normals) {
			// Ground truth EXACTLY as the selector accumulates it.
			glm::vec3 eTrue(0.0f);
			for (uint32_t i = 0; i < kRays; ++i)
				eTrue += glm::vec3(1.0f)
					* (std::max(glm::dot(dirs[i], n), 0.0f) * weight);
			const glm::vec3 eFit =
				smvk::gi::SgLightmapDiffuse(amps, kCounts[ci], n);
			const float rel = std::abs(Luminance(eFit) - Luminance(eTrue))
				/ std::max(Luminance(eTrue), 1e-6f);
			worstThis = std::max(worstThis, rel);
			printFit = eFit;
			printTrue = eTrue;
		}
		std::cout << "    lobes=" << kCounts[ci]
			<< " relErr=" << (worstThis * 100.0f) << "%"
			<< " eFit(" << printFit.x << " " << printFit.y << " " << printFit.z << ")"
			<< " eTrue(" << printTrue.x << " " << printTrue.y << " " << printTrue.z << ")"
			<< "\n";
		worstRel = std::max(worstRel, worstThis);
		// pi-domain consistency: eFit within 35% of eTrue on a constant
		// field (the diagonal projection overshoots constants somewhat —
		// the exact overshoot is measured here — but a missing 1/pi would
		// read as ~68% low, a doubled pi as ~214% high).
		if (worstThis > 0.35f)
			domainOk = false;
	}
	Check(domainOk, "lm selector domain: eFit within 35% of eTrue (no pi mismatch)");
	Check(worstRel < 1.0f, "lm selector domain: constant fit bounded (<100%)");
}

void TestSgSpecular() {
	// Constant field via NNLS span fit: the normalized GGX reflection
	// kernel must return ~the constant regardless of roughness
	// (K integrates to 1). NNLS, not the production diagonal (which
	// overshoots constants by design — see the diffuse test note).
	glm::vec3 constAmps[kSgLobeCount];
	{
		std::mt19937 rng(41);
		std::vector<glm::vec3> dirs, radiance;
		for (int s = 0; s < 512; ++s) {
			dirs.push_back(UniformDir(rng));
			radiance.push_back(glm::vec3(0.7f));
		}
		SgFitAccumulator acc;
		const float weight = 4.0f * kPi / float(dirs.size());
		for (size_t s = 0; s < dirs.size(); ++s)
			acc.AddSample(dirs[s], radiance[s], weight);
		acc.SolveNnls(constAmps);
	}
	bool constOk = true;
	for (float rough : { 0.3f, 0.5f, 0.8f }) {
		const glm::vec3 s = SgSpecular(constAmps, glm::vec3(0, 1, 0), rough);
		if (std::abs(Luminance(s) - 0.7f) > 0.08f)
			constOk = false;
	}
	Check(constOk, "sg specular kernel normalized (const field)");

	// Aligned sharp lobe at low roughness approaches the lobe amplitude
	// (mirror-like tap).
	glm::vec3 mirrorAmps[kSgLobeCount] = {};
	mirrorAmps[0] = glm::vec3(2.0f);
	const glm::vec3 axis0(kSgBasis[0][0], kSgBasis[0][1], kSgBasis[0][2]);
	const glm::vec3 sMirror = SgSpecular(mirrorAmps, axis0, 0.1f);
	Check(Luminance(sMirror) > 1.2f, "sg specular mirror tap (aligned)");

	// MC cross-check of the physical kernel (D(h(w))/4, view == normal so
	// the reflection axis == n) via GGX NDF importance sampling.
	std::mt19937 rng(31);
	glm::vec3 field[kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		field[i] = glm::vec3(0.4f + 0.6f * std::cos(float(i * 1.3f)));
	const float rough = 0.55f;
	const float a2 = std::pow(rough, 4.0f); // shader convention: alpha2 = r^4
	const glm::vec3 n(0, 1, 0);
	glm::vec3 mcSum(0.0f);
	const int N = 65536;
	std::uniform_real_distribution<float> u(0.0f, 1.0f);
	for (int s = 0; s < N; ++s) {
		// NDF sampling: pdf(wh) = D(wh) (n.wh).
		const float xi1 = std::max(u(rng), 1e-5f);
		const float xi2 = u(rng);
		const float cosTheta = std::sqrt((1.0f - xi1) / (1.0f + (a2 - 1.0f) * xi1));
		const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
		const float phi = 2.0f * kPi * xi2;
		const glm::vec3 h(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
		const glm::vec3 w = glm::normalize(2.0f * glm::dot(n, h) * h - n); // reflect view n
		// estimator: L(w) (n.h) / N (pdf D(n.h) cancels the D(n.h)/4 kernel
		// against the 4(n.h) reflection Jacobian).
		mcSum += SgEvaluate(field, w) * glm::dot(n, h);
	}
	const glm::vec3 mcSpec = mcSum / float(N);
	const glm::vec3 sgSpec = SgSpecular(field, n, rough);
	const float rel = std::abs(Luminance(sgSpec) - Luminance(mcSpec))
		/ std::max(Luminance(mcSpec), 1e-5f);
	Check(rel < 0.20f, "sg specular vs GGX MC (<20%)");
	std::cout << "  sg specular rel err vs MC: " << rel << "\n";
}

void TestTriangleBvh() {
	// One triangle on z=0 plus one "wall" quad triangle; hand-checked
	// distances and hits.
	std::vector<glm::vec3> positions = {
		{ 0, 0, 0 }, { 2, 0, 0 }, { 0, 2, 0 },          // T0 (z=0 plane)
		{ 4, 0, 0 }, { 4, 0, 4 }, { 4, 3, 0 },          // T1 (x=4 plane)
	};
	const std::vector<uint32_t> indices = { 0, 1, 2, 3, 4, 5 };
	TriangleBvh bvh;
	bvh.Build(positions, indices);
	Check(bvh.TriangleCount() == 2, "bvh triangle count");

	TriangleBvh::Hit h;
	Check(bvh.Raycast(glm::vec3(0.5f, 0.5f, 1.0f), glm::vec3(0, 0, -1), 10.0f, h)
		&& std::abs(h.t - 1.0f) < 1e-4f && h.tri == 0,
		"bvh raycast hit t/tri");
	Check(!bvh.Raycast(glm::vec3(0.5f, 0.5f, 1.0f), glm::vec3(0, 0, 1), 10.0f, h),
		"bvh raycast miss");
	Check(bvh.Raycast(glm::vec3(0.5f, 0.5f, -1.0f), glm::vec3(0, 0, 1), 10.0f, h),
		"bvh two-sided hit");
	Check(bvh.AnyHit(glm::vec3(3, 1, 2), glm::vec3(1, 0, 0), 10.0f),
		"bvh anyhit wall");
	Check(!bvh.AnyHit(glm::vec3(0.5f, 0.5f, 0.5f), glm::vec3(0, 0, 1), 10.0f),
		"bvh anyhit miss");

	CheckNear(bvh.ClosestDistance(glm::vec3(0.5f, 0.5f, 1.0f)), 1.0f, 1e-4f,
		"bvh closest distance above tri");
	CheckNear(bvh.ClosestDistance(glm::vec3(0.5f, 0.5f, 0.0f)), 0.0f, 1e-4f,
		"bvh closest on surface");
	CheckNear(bvh.ClosestDistance(glm::vec3(5.0f, 1.0f, 1.0f)), 1.0f, 1e-4f,
		"bvh closest distance to wall");
	glm::vec3 cp;
	bvh.ClosestDistance(glm::vec3(0.5f, 0.5f, 1.0f), &cp);
	Check(glm::length(cp - glm::vec3(0.5f, 0.5f, 0.0f)) < 1e-4f,
		"bvh closest point");
	// Far query: distance to the wall plane corner region.
	Check(bvh.ClosestDistance(glm::vec3(-10, -10, -10)) > 10.0f,
		"bvh far distance sane");

	// Reorder regression (> leaf size forces a split + tri reorder):
	// 12 isolated quads (2 tris each = 24 tris) spread along X. Every
	// triangle must resolve to its OWN vertices after the build —
	// TriangleVertices(k) takes the original index, the BVH reorder is
	// resolved by the caller. The old double-indirection returned a
	// shuffled triangle here (wrong hits, normals, emissive picks).
	{
		std::vector<glm::vec3> pos2;
		std::vector<uint32_t> idx2;
		for (int q = 0; q < 12; ++q) {
			const float x = float(q) * 10.0f;
			const uint32_t b = uint32_t(pos2.size());
			pos2.push_back({ x, 0, 0 });
			pos2.push_back({ x + 1, 0, 0 });
			pos2.push_back({ x, 1, 0 });
			pos2.push_back({ x + 1, 1, 0 });
			idx2.push_back(b + 0); idx2.push_back(b + 1); idx2.push_back(b + 2);
			idx2.push_back(b + 1); idx2.push_back(b + 3); idx2.push_back(b + 2);
		}
		TriangleBvh bvh2;
		bvh2.Build(pos2, idx2);
		bool vertsOk = true;
		for (uint32_t k = 0; k < bvh2.TriangleCount(); ++k) {
			glm::vec3 v0, v1, v2;
			bvh2.TriangleVertices(k, v0, v1, v2);
			const uint32_t e0 = idx2[k * 3 + 0], e1 = idx2[k * 3 + 1], e2 = idx2[k * 3 + 2];
			if (glm::length(v0 - pos2[e0]) > 1e-5f
				|| glm::length(v1 - pos2[e1]) > 1e-5f
				|| glm::length(v2 - pos2[e2]) > 1e-5f)
				vertsOk = false;
		}
		Check(vertsOk, "bvh triangle vertices survive reorder");
		bool hitsOk = true;
		for (int q = 0; q < 12; ++q) {
			const float x = float(q) * 10.0f;
			TriangleBvh::Hit h2;
			// Aim at the quad center from +z: must hit one of its 2 tris.
			if (!bvh2.Raycast(glm::vec3(x + 0.5f, 0.5f, 1.0f),
					glm::vec3(0, 0, -1), 10.0f, h2)
				|| std::abs(h2.t - 1.0f) > 1e-4f
				|| h2.tri / 2 != uint32_t(q)) {
				hitsOk = false;
				break;
			}
			// Normal of the hit tri must be +z (not a shuffled neighbor).
			const glm::vec3 n = bvh2.TriangleNormal(h2.tri);
			if (glm::length(n - glm::vec3(0, 0, 1)) > 1e-4f) {
				hitsOk = false;
				break;
			}
		}
		Check(hitsOk, "bvh raycast tri ids survive reorder");
	}
}

// (TestProbeVolumeRoundtrip — the .sggi serializer/dense-layer/GPU-pack
// round trip — is RETIRED with the SgGiVolume data layer: stage 3 of
// the octree retirement. The live volume tests are TestRegularVolume
// + TestGiVolume below.)
void TestRegularVolume() {
	// Single-voxel volume: Sample returns the voxel amps everywhere inside.
	SgRegularVolume vol;
	vol.obb.center = glm::vec3(0.0f);
	vol.obb.halfExtents = glm::vec3(1.0f);
	vol.obb.dims[0] = vol.obb.dims[1] = vol.obb.dims[2] = 1;
	vol.Resize();
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		vol.coeffs[i] = glm::vec3(1.0f + float(i));
	glm::vec3 amps[kSgLobeCount];
	Check(vol.Sample(glm::vec3(0.0f), amps), "regular single voxel covered");
	bool match = true;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		if (glm::length(amps[i] - vol.coeffs[i]) > 1e-5f)
			match = false;
	Check(match, "regular single voxel returns amps");
	Check(!vol.Sample(glm::vec3(5.0f, 0.0f, 0.0f), amps), "regular outside uncovered");

	// 2x1x1 lerp: midpoint is the voxel average (hardware trilinear mirror).
	SgRegularVolume strip;
	strip.obb.center = glm::vec3(0.0f);
	strip.obb.halfExtents = glm::vec3(1.0f, 0.5f, 0.5f);
	strip.obb.dims[0] = 2; strip.obb.dims[1] = 1; strip.obb.dims[2] = 1;
	strip.Resize();
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		strip.coeffs[0 * kSgLobeCount + i] = glm::vec3(0.0f);
		strip.coeffs[1 * kSgLobeCount + i] = glm::vec3(2.0f);
	}
	Check(strip.Sample(glm::vec3(0.0f, 0.0f, 0.0f), amps), "regular strip covered");
	bool mid = true;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		if (std::abs(amps[i].x - 1.0f) > 1e-4f)
			mid = false;
	Check(mid, "regular trilinear midpoint averages");

	// File round-trip through the production reader.
	const auto tmp = std::filesystem::temp_directory_path()
		/ "tourmaline_gi_regular_test.sgrv";
	std::string err;
	Check(strip.WriteFile(tmp.string(), err), "sgrv write");
	SgRegularVolume reloaded;
	Check(SgRegularVolume::ReadFile(tmp.string(), reloaded, err), "sgrv read");
	bool same = reloaded.VoxelCount() == strip.VoxelCount()
		&& reloaded.obb.dims[0] == 2
		&& glm::length(reloaded.obb.center - strip.obb.center) < 1e-5f;
	if (same) {
		for (size_t k = 0; k < strip.coeffs.size(); ++k)
			if (glm::length(reloaded.coeffs[k] - strip.coeffs[k]) > 2e-3f)
				same = false;
	}
	Check(same, "sgrv roundtrip equal");
	std::filesystem::remove(tmp);

	// GPU packing: 7 layers x voxels x RGBA halves.
	std::vector<uint16_t> layers[7];
	strip.PackGpuLayers(layers);
	bool packOk = true;
	for (int t = 0; t < 7; ++t)
		if (layers[t].size() != size_t(2) * 4)
			packOk = false;
	Check(packOk, "regular gpu pack shape");
	Check(strip.GpuBytes() == size_t(2) * 56, "regular gpu bytes");

	// Empty-scene bake: every ray hits open sky, so every voxel fits the
	// sky radiance (nonzero — the sky IBL is baked into the field now).
	// With skyIntensity zeroed the field is the old sky-less one: exact
	// zero (fixed lattice, no relocation to smuggle in coverage).
	{
		GiTraceScene scene;
		scene.positions = {};
		scene.indices = {};
		scene.materials = { GiTraceMaterial{ glm::vec3(0.7f), glm::vec3(0.0f) } };
		scene.triMaterial = {};
		scene.Finalize();
		SgRegularObb obb{};
		obb.center = glm::vec3(0.0f);
		obb.halfExtents = glm::vec3(1.0f);
		obb.dims[0] = obb.dims[1] = obb.dims[2] = 2;
		SgRegularVolume baked;
		SgRegularBakeConfig rcfg{};
		rcfg.raysPerProbe = 16;
		rcfg.maxBounces = 0;
		SgRegularBakeStats rstats;
		Check(BakeSgRegularVolume(scene, obb, rcfg, baked, rstats, nullptr),
			"regular bake empty scene runs");
		bool anySky = false;
		for (const glm::vec3& a : baked.coeffs)
			if (glm::length(a) > 0.0f)
				anySky = true;
		Check(anySky, "regular bake empty scene carries sky");
		scene.skyIntensity = 0.0f;
		SgRegularVolume skyless;
		SgRegularBakeStats rstats2;
		Check(BakeSgRegularVolume(scene, obb, rcfg, skyless, rstats2, nullptr),
			"regular bake skyless scene runs");
		bool allZero = true;
		for (const glm::vec3& a : skyless.coeffs)
			if (glm::length(a) != 0.0f)
				allZero = false;
		Check(allZero, "regular bake zero-sky exact zero");
	}
}

void TestGiVolume() {
	// ---- Weight math (CPU mirror of sg_gi_volume.hlsli; per-plane fades) ----
	{
		GiVolumeDef def;
		def.planes = GiVolumeBoxPlanes(glm::vec3(-1.0f), glm::vec3(1.0f), 0.1f, 0.1f);
		Check(def.planes.size() == 6, "GI volume box has six planes");
		Check(def.Contains(glm::vec3(0.0f)), "GI volume box contains center");
		Check(!def.Contains(glm::vec3(1.5f, 0.0f, 0.0f)),
			"GI volume box rejects outside");
		Check(def.Weight(glm::vec3(0.0f)) == 1.0f, "GI volume deep-inside weight 1");
		Check(def.Weight(glm::vec3(2.0f, 0.0f, 0.0f)) == 0.0f,
			"GI volume far-outside weight 0");
		CheckNear(def.Weight(glm::vec3(1.0f, 0.0f, 0.0f)), 0.5f, 1e-5f,
			"GI volume face weight 0.5 (symmetric fades)");
		// ---- Rotated box (OBB): GiVolumeBoxPlanesRotated --------------------
		// Identity axes degenerate to GiVolumeBoxPlanes exactly (same plane
		// order: -x, +x, -y, +y, -z, +z — the doorway-retune indices
		// keep meaning the same faces).
		{
			const std::vector<GiVolumePlane> id = GiVolumeBoxPlanesRotated(
				glm::vec3(0.0f), glm::mat3(1.0f), glm::vec3(1.0f),
				0.1f, 0.1f);
			const std::vector<GiVolumePlane> aa = GiVolumeBoxPlanes(
				glm::vec3(-1.0f), glm::vec3(1.0f), 0.1f, 0.1f);
			Check(id.size() == 6 && aa.size() == 6,
				"GI volume rotated box six planes");
			bool same = true;
			for (size_t i = 0; i < 6; ++i)
				same &= glm::all(glm::equal(id[i].normal, aa[i].normal))
					&& id[i].d == aa[i].d;
			Check(same, "GI volume identity axes == axis-aligned planes");
		}
		// 90-degree yaw around Y: the u0 axis maps to -Z, u2 to +X.
		// A point 1.2 m out along WORLD +X (past the rotated u2 half
		// extent) falls outside; 0.9 m stays inside — and a point past
		// the OLD axis-aligned extent in Z (now the long axis) stays
		// INSIDE (the rotation reshapes the hull, not just the AABB).
		{
			GiVolumeDef rot;
			// yaw 90deg: columns = u0 = (0,0,-1), u1 = (0,1,0), u2 = (1,0,0)
			rot.planes = GiVolumeBoxPlanesRotated(glm::vec3(0.0f),
				glm::mat3(0.0f, 0.0f, -1.0f,
					0.0f, 1.0f, 0.0f,
					1.0f, 0.0f, 0.0f),
				glm::vec3(1.0f, 2.0f, 0.5f), 0.0f, 0.0f);
			Check(rot.Contains(glm::vec3(0.0f)),
				"GI volume rotated contains center");
			Check(!rot.Contains(glm::vec3(1.2f, 0.0f, 0.0f)),
				"GI volume rotated rejects past u2 half extent");
			Check(rot.Contains(glm::vec3(0.4f, 0.0f, 0.0f)),
				"GI volume rotated accepts inside u2 half extent");
			// u0 half = 1.0 along -Z: 0.9 in, 1.2 out.
			Check(rot.Contains(glm::vec3(0.0f, 0.0f, -0.9f)),
				"GI volume rotated accepts inside u0 half extent");
			Check(!rot.Contains(glm::vec3(0.0f, 0.0f, -1.2f)),
				"GI volume rotated rejects past u0 half extent");
			// u1 half = 2.0 along Y: 1.9 in, 2.2 out (the tallest axis).
			Check(rot.Contains(glm::vec3(0.0f, 1.9f, 0.0f)),
				"GI volume rotated tall axis inside");
			Check(!rot.Contains(glm::vec3(0.0f, 2.2f, 0.0f)),
				"GI volume rotated tall axis outside");
			// Off-center rotation: the same hull translated by c.
			GiVolumeDef moved;
			moved.planes = GiVolumeBoxPlanesRotated(
				glm::vec3(5.0f, 0.0f, 0.0f), glm::mat3(1.0f),
				glm::vec3(1.0f), 0.0f, 0.0f);
			Check(moved.Contains(glm::vec3(5.0f, 0.0f, 0.0f)),
				"GI volume off-center contains center");
			Check(!moved.Contains(glm::vec3(0.0f)),
				"GI volume off-center rejects the origin");
		}
		// Inside the fade band: 0.05 m inside the +x face.
		// smoothstep(-0.1, 0.1, 0.05): t = 0.75 -> 0.84375.
		CheckNear(def.Weight(glm::vec3(0.95f, 0.0f, 0.0f)), 0.84375f, 1e-5f,
			"GI volume fade band smoothstep");
		// Inside the OUTSIDE fade band: 0.05 m outside the +x face.
		// smoothstep(-0.1, 0.1, -0.05): t = 0.25 -> 0.15625 — the volume
		// keeps partial influence in the doorway bleed zone.
		CheckNear(def.Weight(glm::vec3(1.05f, 0.0f, 0.0f)), 0.15625f, 1e-5f,
			"GI volume outside fade band partial weight");
		// Past the outside fade: no influence.
		CheckNear(def.Weight(glm::vec3(1.2f, 0.0f, 0.0f)), 0.0f, 1e-6f,
			"GI volume past outside fade is 0");
		// Hard faces (zero fades): the weight steps at the plane.
		GiVolumeDef hard;
		hard.planes = GiVolumeBoxPlanes(glm::vec3(-1.0f), glm::vec3(1.0f), 0.0f, 0.0f);
		Check(hard.Weight(glm::vec3(0.999f, 0.0f, 0.0f)) == 1.0f,
			"GI volume hard face inside");
		Check(hard.Weight(glm::vec3(1.001f, 0.0f, 0.0f)) == 0.0f,
			"GI volume hard face outside");
		// Doorway: retune the +x face (index 1) to a 2 m fade; the
		// MIN-combine keeps the doorway fade alive where tight faces
		// would crush a product to zero.
		GiVolumeDef room;
		room.planes = GiVolumeBoxPlanes(glm::vec3(-1.0f), glm::vec3(1.0f), 0.1f, 0.1f);
		room.planes[1].insideFade = 2.0f;
		room.planes[1].outsideFade = 2.0f;
		// Room center: 1 m inside the doorway plane ->
		// smoothstep(-2, 2, 1): t = 0.75 -> 0.84375.
		CheckNear(room.Weight(glm::vec3(0.0f)), 0.84375f, 1e-5f,
			"GI volume doorway fade at room center");
		// Threshold: half weight, and the tight floor/walls do not
		// crush it (min, not product).
		CheckNear(room.Weight(glm::vec3(1.0f, -0.5f, 0.0f)), 0.5f, 1e-5f,
			"GI volume doorway threshold min-combine");
		// A tight face still dominates where it is the nearest boundary:
		// 1 cm inside the floor -> smoothstep(-0.1, 0.1, 0.01):
		// t = 0.55 -> 0.57475 (the doorway at this x is 0.84375).
		CheckNear(room.Weight(glm::vec3(0.0f, -0.99f, 0.0f)), 0.57475f, 1e-4f,
			"GI volume tight floor wins near the floor");
	}
	// ---- Bounds clipping ----
	{
		GiVolumeDef def;
		def.planes = GiVolumeBoxPlanes(glm::vec3(0.0f), glm::vec3(2.0f), 0.1f, 0.1f);
		glm::vec3 mn, mx;
		Check(def.Bounds(mn, mx), "GI volume bounds box");
		bool ok = glm::length(mn - glm::vec3(0.0f)) < 1e-4f
			&& glm::length(mx - glm::vec3(2.0f)) < 1e-4f;
		Check(ok, "GI volume bounds box extents");
		GiVolumePlane cut{};
		cut.normal = glm::vec3(0.0f, 1.0f, 0.0f);
		cut.d = -1.0f; // inside y <= 1
		def.planes.push_back(cut);
		Check(def.Bounds(mn, mx) && mx.y <= 1.0f + 1e-4f,
			"GI volume bounds clipped by extra plane");
		// Unbounded plane set is rejected.
		GiVolumeDef openSet;
		GiVolumePlane half{};
		half.normal = glm::vec3(1.0f, 0.0f, 0.0f);
		openSet.planes.push_back(half);
		glm::vec3 omn, omx;
		Check(!openSet.Bounds(omn, omx), "GI volume unbounded plane set rejected");
	}
	// ---- CPU trilinear mirror + serialization + GPU pack ----
	{
		GiVolume vol;
		vol.gridMin = glm::vec3(-0.5f);
		vol.spacing = 1.0f;
		vol.dims[0] = vol.dims[1] = vol.dims[2] = 1;
		vol.def.planes = GiVolumeBoxPlanes(glm::vec3(-0.5f), glm::vec3(0.5f));
		vol.Resize();
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			vol.coeffs[i] = glm::vec3(1.0f + float(i));
		glm::vec3 amps[kSgLobeCount];
		Check(vol.Sample(glm::vec3(0.0f), amps), "GI volume single voxel covered");
		bool match = true;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			if (glm::length(amps[i] - vol.coeffs[i]) > 1e-5f)
				match = false;
		Check(match, "GI volume single voxel returns amps");
		Check(!vol.Sample(glm::vec3(5.0f, 0.0f, 0.0f), amps),
			"GI volume outside uncovered");

		// 2x1x1 strip: the VISIBILITY-WEIGHTED trilerp (the CPU mirror
		// of SgGiVolumeSample). With NO vis data (a hand-built volume)
		// every corner factor is 1 — the plain trilerp: the midpoint
		// (grid coord 1.0 in x, exactly on the voxel boundary) is the
		// 50/50 blend of voxels #0 (0) and #1 (2) = 1.0.
		GiVolume strip;
		strip.gridMin = glm::vec3(-1.0f, 0.0f, 0.0f);
		strip.spacing = 1.0f;
		strip.dims[0] = 2;
		strip.dims[1] = 1;
		strip.dims[2] = 1;
		strip.def.planes = GiVolumeBoxPlanes(glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(1.0f, 1.0f, 1.0f));
		strip.def.priority = 3;
		strip.Resize();
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			strip.coeffs[0 * kSgLobeCount + i] = glm::vec3(0.0f);
			strip.coeffs[1 * kSgLobeCount + i] = glm::vec3(2.0f);
		}
		Check(strip.Sample(glm::vec3(0.0f, 0.5f, 0.5f), amps),
			"GI volume strip covered");
		bool mid = true;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			if (std::abs(amps[i].x - 1.0f) > 1e-4f)
				mid = false;
		Check(mid, "GI volume no-vis midpoint is the plain trilerp blend");

		// The rejection: hand-author the 6-axis vis (a wall just behind
		// voxel #1 — its -x obstacle at 0.05 m) + no clean flags. A
		// sample at the boundary now REJECTS voxel #1 (the offset 0.5
		// spacing units along its blocked axis exceeds dist+slack) and
		// reads voxel #0's value alone: the through-wall behavior.
		strip.vis.assign(2 * 6, 15.0f * strip.spacing); // open everywhere
		strip.vis[1 * 6 + 1] = 0.05f; // voxel #1's -x obstacle (a wall)
		strip.visCellClean.clear();    // no fast-path flags
		Check(strip.Sample(glm::vec3(0.0f, 0.5f, 0.5f), amps),
			"GI volume strip covered (vis)");
		bool rej = true;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			if (std::abs(amps[i].x - 0.0f) > 1e-4f)
				rej = false;
		Check(rej, "GI volume blocked-side tap rejects across the wall");

		// File round trip (priority + planes + coeffs + the vis data).
		const auto tmp = std::filesystem::temp_directory_path()
			/ "tourmaline_gi_volume_test.givolume";
		std::string err;
		Check(strip.WriteFile(tmp.string(), err), "GI volume write");
		GiVolume reloaded;
		Check(GiVolume::ReadFile(tmp.string(), reloaded, err), "GI volume read");
		bool same = reloaded.VoxelCount() == strip.VoxelCount()
			&& reloaded.def.priority == 3
			&& reloaded.def.planes.size() == strip.def.planes.size()
			&& glm::length(reloaded.gridMin - strip.gridMin) < 1e-5f
			&& std::abs(reloaded.spacing - strip.spacing) < 1e-6f;
		if (same && reloaded.def.planes.size() == strip.def.planes.size()) {
			for (size_t p = 0; p < strip.def.planes.size(); ++p)
				if (glm::length(reloaded.def.planes[p].normal
						- strip.def.planes[p].normal) > 1e-5f)
					same = false;
			for (size_t k = 0; k < strip.coeffs.size(); ++k)
				if (glm::length(reloaded.coeffs[k] - strip.coeffs[k]) > 2e-3f)
					same = false;
		}
		Check(same, "GI volume roundtrip equal");
		// The v2 visibility block round-trips: the hand-authored
		// blocked axis + the open defaults survive byte-exact (the
		// clean-flag block was absent — serialized as the no-flags
		// shape, read back empty).
		bool visSame = reloaded.vis.size() == strip.vis.size();
		if (visSame)
			for (size_t k = 0; k < strip.vis.size(); ++k)
				if (std::abs(reloaded.vis[k] - strip.vis[k]) > 1e-5f)
					visSame = false;
		Check(visSame, "GI volume vis roundtrip equal");
		Check(reloaded.visCellClean.empty(),
			"GI volume absent clean flags stay absent");
		std::filesystem::remove(tmp);

		// GPU packing: 7 layers x voxels x RGBA halves (sgrv layout).
		std::vector<uint16_t> layers[7];
		strip.PackGpuLayers(layers);
		bool packOk = true;
		for (int t = 0; t < 7; ++t)
			if (layers[t].size() != size_t(2) * 4)
				packOk = false;
		Check(packOk, "GI volume gpu pack shape");
		Check(strip.GpuBytes() == size_t(2) * 64, "GI volume gpu bytes");

		// The vis GPU pack (PackGpuVisLayers): 2 RGBA8_UNORM layers x
		// voxels x 4 bytes; the distances quantized unorm8 over [0,15]
		// spacing units; the clean-flag channel rides layer 1's .b
		// (255 = clean). The hand-authored 0.05 m -x obstacle on voxel
		// #1 packs at 0.05/1.0/15*255 ~= 1 (nearly blocked); the open
		// axes pack 255; with no clean flags every .b reads 0.
		{
			std::vector<uint8_t> vis[2];
			strip.PackGpuVisLayers(vis);
			bool shapeOk = vis[0].size() == size_t(2) * 4
				&& vis[1].size() == size_t(2) * 4;
			Check(shapeOk, "GI volume vis pack shape");
			const uint8_t qBlocked = uint8_t(
				0.05f / 15.0f * 255.0f + 0.5f);
			// Layer 0 carries +x,-x,+y,-y: voxel #1's -x is channel 1.
			bool valsOk = vis[0][1 * 4 + 0] == uint8_t(255) // voxel1 +x open
				&& vis[0][1 * 4 + 1] == qBlocked // voxel1 -x = the wall
				&& vis[0][0 * 4 + 0] == uint8_t(255) // voxel0 open
				&& vis[1][1 * 4 + 0] == uint8_t(255) // voxel1 +z open
				&& vis[1][1 * 4 + 2] == uint8_t(0); // no clean flags
			Check(valsOk, "GI volume vis pack values");
		}

		// Isotropic-mean payload (the volumetric fog's one-fetch path —
		// PackGpuMeanLayer): rgb = 0.124959 * Σ lobes per voxel (the
		// VolGiMean constant, volumetric.frag.hlsl), a = 1, half-rounded —
		// trilerp(Σ) == Σ(trilerp) makes it the same mean the fog's
		// 7-fetch + unpack + lobe-sum path computes.
		std::vector<uint16_t> meanLayer;
		strip.PackGpuMeanLayer(meanLayer);
		Check(meanLayer.size() == size_t(2) * 4, "GI volume mean pack shape");
		bool meanOk = true;
		for (uint64_t v = 0; v < strip.VoxelCount(); ++v) {
			glm::vec3 sum(0.0f);
			for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe)
				sum += strip.coeffs[size_t(v) * kSgLobeCount + lobe];
			const glm::vec3 want = glm::clamp(sum * 0.124959f,
				glm::vec3(0.0f), glm::vec3(65504.0f));
			for (int c = 0; c < 3; ++c) {
				const float got = UnpackHalf(meanLayer[size_t(v) * 4 + c]);
				if (std::abs(got - want[c]) > 0.05f)
					meanOk = false;
			}
			if (UnpackHalf(meanLayer[size_t(v) * 4 + 3]) != 1.0f)
				meanOk = false;
		}
		Check(meanOk, "GI volume mean pack matches VolGiMean sum");
	}
	// ---- Bake: empty scene (all-sky) ----
	{
		GiTraceScene scene;
		scene.positions = {};
		scene.indices = {};
		scene.materials = { GiTraceMaterial{ glm::vec3(0.7f), glm::vec3(0.0f) } };
		scene.triMaterial = {};
		scene.Finalize();
		GiVolumeDef def;
		def.planes = GiVolumeBoxPlanes(glm::vec3(-1.0f), glm::vec3(1.0f));
		GiVolumeBakeConfig cfg{};
		cfg.captureDirs = 128;
		cfg.maxBounces = 0;
		cfg.smoothingPasses = 0;
		cfg.threadCount = 2;
		GiVolume baked;
		GiVolumeBakeStats stats;
		Check(BakeGiVolume(scene, def, 0.5f, cfg, baked, stats, nullptr),
			"GI volume bake empty scene runs");
		Check(stats.probes > 0 && stats.samplesClamped >= 0,
			"GI volume bake stats populated");
		bool anySky = false;
		for (const glm::vec3& a : baked.coeffs)
			if (glm::length(a) > 0.0f)
				anySky = true;
		Check(anySky, "GI volume bake empty scene carries sky");
		// Per-probe identity: the bake IS the direct fit of the
		// direction set now (the old reprojection pipeline's
		// captureSpacing == spacing identity case, by construction).
		{
			const uint32_t N = cfg.captureDirs;
			SgFitAccumulator ref;
			const float weight = 4.0f * kPi / float(N);
			for (uint32_t i = 0; i < N; ++i) {
				const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(N);
				const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
				const float phi = float(i) * 2.39996323f;
				const glm::vec3 d(std::cos(phi) * r, y, std::sin(phi) * r);
				ref.AddSample(d, scene.SkyRadiance(d), weight);
			}
			glm::vec3 refAmps[kSgLobeCount];
			ref.Solve(refAmps);
			// Center probe of the baked grid.
			const uint32_t cx = baked.dims[0] / 2;
			const uint32_t cy = baked.dims[1] / 2;
			const uint32_t cz = baked.dims[2] / 2;
			const size_t idx = (size_t(cz) * baked.dims[1] + cy) * baked.dims[0] + cx;
			bool same = true;
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				if (glm::length(baked.coeffs[idx * kSgLobeCount + i] - refAmps[i])
					> 1e-3f * std::max(1.0f, glm::length(refAmps[i])))
					same = false;
			Check(same, "GI volume per-probe direct fit identity");
		}
				// The visibility bake (empty scene): every probe's every axis
		// records the CAP (no obstacle anywhere = open), and every
		// forward cell is CLEAN (no rejection possible) — the
		// fast-path shape a no-geometry bake should produce.
		{
			bool visOpen = baked.vis.size()
				== size_t(baked.VoxelCount()) * 6;
			for (size_t k = 0; visOpen && k < baked.vis.size(); ++k)
				if (std::abs(baked.vis[k]
						- cfg.visibilityMaxDistance) > 1e-5f)
						visOpen = false;
				Check(visOpen, "GI volume empty-scene vis all open");
            // Every FORWARD cell is clean (no geometry anywhere); the
            // edge voxels' flags stay 0 by contract (no forward cell
            // exists - the runtime clamps its base corner to
            // dims-2), so the set count is (nx-1)*(ny-1)*(nz-1).
            bool cleanAll = baked.visCellClean.size()
                == size_t(baked.VoxelCount());
            size_t setCells = 0;
            for (size_t k = 0; k < baked.visCellClean.size(); ++k)
                if (baked.visCellClean[k])
                    ++setCells;
            if (cleanAll)
                cleanAll = setCells == size_t(baked.dims[0] - 1)
                    * (baked.dims[1] - 1) * (baked.dims[2] - 1);
            Check(cleanAll, "GI volume empty-scene cells all clean");
		}
		// Skyless: exact zero (no geometry, no light).
		scene.skyIntensity = 0.0f;
		GiVolume skyless;
		GiVolumeBakeStats stats2;
		Check(BakeGiVolume(scene, def, 0.5f, cfg, skyless, stats2, nullptr),
			"GI volume bake skyless scene runs");
		bool allZero = true;
		for (const glm::vec3& a : skyless.coeffs)
			if (glm::length(a) != 0.0f)
				allZero = false;
		Check(allZero, "GI volume bake zero-sky exact zero");
	}
		// ---- Bake: the WALL scene (the through-wall rejection data end
	// to end) ----
	// A thin wall slab (x in [-0.05, 0.05], y in [0, 3], z in
	// [-1.5, 1.5]) baked into a volume straddling it at 0.5 m
	// spacing: the probes either side must record the wall on their
	// FACING axis (and the cap = open on the away axes), and the cells
	// straddling the wall are NOT clean (the runtime's weighted path)
	// while cells well away from it are. No probe center lands inside
	// the 10 cm slab at this spacing — the wall falls BETWEEN the
	// x=±0.25 probe pair, which is exactly the runtime leak geometry.
	{
		GiTraceScene scene;
		const glm::vec3 q[] = {
			glm::vec3(0.05f, 0.0f, -1.5f), glm::vec3(0.05f, 3.0f, -1.5f),
			glm::vec3(0.05f, 3.0f, 1.5f), glm::vec3(0.05f, 0.0f, 1.5f),
			glm::vec3(-0.05f, 0.0f, -1.5f), glm::vec3(-0.05f, 3.0f, -1.5f),
			glm::vec3(-0.05f, 3.0f, 1.5f), glm::vec3(-0.05f, 0.0f, 1.5f),
		};
		for (const glm::vec3& p : q)
			scene.positions.push_back(p);
		auto quad = [&](size_t a, size_t b, size_t c, size_t d) {
			scene.indices.push_back(uint32_t(a));
			scene.indices.push_back(uint32_t(b));
			scene.indices.push_back(uint32_t(c));
			scene.indices.push_back(uint32_t(a));
			scene.indices.push_back(uint32_t(c));
			scene.indices.push_back(uint32_t(d));
		};
		quad(0, 1, 2, 3); // +x face
		quad(4, 7, 6, 5); // -x face (wound the other way)
		scene.materials = { GiTraceMaterial{ glm::vec3(0.7f), glm::vec3(0.0f) } };
		scene.triMaterial.assign(scene.indices.size() / 3, 0);
		scene.skyIntensity = 0.0f; // geometry-only: deterministic
		scene.Finalize();
		GiVolumeDef def;
		def.planes = GiVolumeBoxPlanes(glm::vec3(-2.5f, 0.0f, -1.5f),
			glm::vec3(2.5f, 3.0f, 1.5f));
		GiVolumeBakeConfig cfg{};
		cfg.captureDirs = 64;
		cfg.maxBounces = 0;
		cfg.smoothingPasses = 0;
		cfg.threadCount = 2;
		GiVolume baked;
		GiVolumeBakeStats stats;
		Check(BakeGiVolume(scene, def, 0.5f, cfg, baked, stats, nullptr),
			"GI volume wall-scene bake runs");
		const uint32_t nx = baked.dims[0];
		const uint64_t n = baked.VoxelCount();
		Check(baked.vis.size() == size_t(n) * 6,
			"GI volume wall-scene vis populated");
		// gridMin = (-1.5, 0, -1.5), spacing 0.5 -> probe centers at
		// x = -1.25, -0.75, -0.25, 0.25, 0.75, 1.25 (the +0.5 center
		// convention). The wall's faces sit 0.2 m from the x=±0.25
		// probes; x=±0.75 read ~0.7 m.
		const float cap = cfg.visibilityMaxDistance;
		const auto probeAt = [&](float x, float y, float z) -> int {
			const glm::vec3 rel = (glm::vec3(x, y, z) - baked.gridMin)
				/ baked.spacing;
			const int ix = int(std::lround(rel.x - 0.5f));
			const int iy = int(std::lround(rel.y - 0.5f));
			const int iz = int(std::lround(rel.z - 0.5f));
			if (ix < 0 || iy < 0 || iz < 0 || ix >= int(baked.dims[0])
				|| iy >= int(baked.dims[1]) || iz >= int(baked.dims[2]))
				return -1;
			return int((uint64_t(iz) * baked.dims[1] + uint64_t(iy)) * nx
				+ uint64_t(ix));
		};
		bool wallRecorded = true;
		{
			const int right = probeAt(0.25f, 1.5f, 0.0f);
			const int left = probeAt(-0.25f, 1.5f, 0.0f);
			if (right < 0 || left < 0) {
				wallRecorded = false;
			}
			else {
				const float rMinusX = baked.vis[size_t(right) * 6 + 1];
				const float lPlusX = baked.vis[size_t(left) * 6 + 0];
				const float rPlusX = baked.vis[size_t(right) * 6 + 0];
				const float lMinusX = baked.vis[size_t(left) * 6 + 1];
				wallRecorded = rMinusX > 0.1f && rMinusX < 0.35f
					&& lPlusX > 0.1f && lPlusX < 0.35f
					&& rPlusX > cap - 1e-3f
					&& lMinusX > cap - 1e-3f;
			}
		}
		Check(wallRecorded, "GI volume wall-scene probes record the wall");
		bool flagsOk = baked.visCellClean.size() == size_t(n);
		if (flagsOk) {
			// Both cells touching the wall (bases at the ±0.25 and the
			// 0.25/0.75 pair) are NOT clean; the far cell (bases
			// 1.75/2.25, min obstacle 1.7 m > 1.35 spacing units) is.
			// NOT the last x voxel — its forward cell doesn't exist
			// and the flag stays 0 by contract.
			const int sLeft = probeAt(-0.25f, 1.5f, 0.0f);
			const int sRight = probeAt(0.25f, 1.5f, 0.0f);
			const int farCell = probeAt(1.75f, 1.5f, 0.0f);
			flagsOk = sLeft >= 0 && sRight >= 0 && farCell >= 0
				&& !baked.visCellClean[size_t(sLeft)]
				&& !baked.visCellClean[size_t(sRight)]
				&& baked.visCellClean[size_t(farCell)];
		}
		Check(flagsOk, "GI volume wall-scene clean flags");
		glm::vec3 amps[kSgLobeCount];
		Check(baked.Sample(glm::vec3(1.25f, 1.5f, 0.0f), amps),
			"GI volume wall-scene far sample covered");
		(void)amps; // the photometric content is zero by construction
	}
	// ---- Bake: floor-plane scene (bounce radiance over geometry) ----
	{
		GiTraceScene scene;
		// One ground quad at y = 0 (two triangles), albedo 0.5.
		scene.positions = {
			glm::vec3(-10.0f, 0.0f, -10.0f), glm::vec3(10.0f, 0.0f, -10.0f),
			glm::vec3(10.0f, 0.0f, 10.0f), glm::vec3(-10.0f, 0.0f, 10.0f),
		};
		scene.indices = { 0, 1, 2, 0, 2, 3 };
		scene.materials = { GiTraceMaterial{ glm::vec3(0.5f), glm::vec3(0.0f) } };
		scene.triMaterial = { 0, 0 };
		scene.Finalize();
		GiVolumeDef def;
		// Volume above the floor: y in [0.05, 2], x/z in [-1, 1].
		def.planes = GiVolumeBoxPlanes(glm::vec3(-1.0f, 0.05f, -1.0f),
			glm::vec3(1.0f, 2.0f, 1.0f));
		GiVolumeBakeConfig cfg{};
		cfg.captureDirs = 256;
		cfg.maxBounces = 1;
		cfg.smoothingPasses = 1;
		cfg.threadCount = 2;
		GiVolume baked;
		GiVolumeBakeStats stats;
		Check(BakeGiVolume(scene, def, 0.5f, cfg, baked, stats, nullptr),
			"GI volume bake floor scene runs");
		Check(stats.probes > 0, "GI volume floor scene baked its grid");
		bool finite = true;
		bool nonNegative = true;
		for (const glm::vec3& a : baked.coeffs) {
			if (!std::isfinite(a.x + a.y + a.z))
				finite = false;
			if (a.x < 0.0f || a.y < 0.0f || a.z < 0.0f)
				nonNegative = false;
		}
		Check(finite, "GI volume floor bake amps finite");
		Check(nonNegative, "GI volume floor bake amps non-negative");
		// Sky above, floor below: the lowest probe layer must see MORE
		// floor-bounced light than the highest layer sees of... rather:
		// both see sky; the point of the check is the field varies
		// spatially (not one uniform value) once geometry is involved.
		float lo = 0.0f, hi = 0.0f;
		{
			const uint32_t nx = baked.dims[0], ny = baked.dims[1];
			const uint32_t iz = baked.dims[2] / 2;
			auto lumAt = [&](uint32_t iy) {
				float l = 0.0f;
				for (uint32_t ix = 0; ix < nx; ++ix) {
					const size_t v = (size_t(iz) * baked.dims[1] + iy) * nx + ix;
					for (uint32_t lb = 0; lb < kSgLobeCount; ++lb)
						l += Luminance(baked.coeffs[v * kSgLobeCount + lb]);
				}
				return l;
			};
			lo = lumAt(0);
			hi = lumAt(ny - 1);
		}
		Check(lo > 0.0f && hi > 0.0f, "GI volume floor bake nonzero layers");
		// Probes close to the floor reproject its bounced light from
		// below-facing directions; high probes see mostly open sky. The
		// totals differ once the reprojection is depth-aware.
		Check(std::abs(lo - hi) > 1e-4f, "GI volume floor bake layers differ");
	}
}

void TestFp16Pack() {
	// Amplitude packing through fp16 keeps HDR values representable.
	SgProbeGpu g{};
	glm::vec3 amps[kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		amps[i] = glm::vec3(1.0f + float(i), 100.0f, 0.001f * float(i + 1));
	SgProbePackAmps(g, amps);
	bool ok = true;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		ok = ok && glm::length(SgProbeUnpackAmp(g, i) - amps[i]) < 2e-3f;
	Check(ok, "fp16 amplitude pack roundtrip");

	SgProbeGpu v{};
	float vis[6] = { 0.25f, 0.5f, 1.0f, 2.0f, 0.0625f, 2.5f };
	SgProbePackVis(v, vis);
	bool visOk = true;
	for (uint32_t k = 0; k < 6; ++k)
		visOk = visOk && std::abs(SgProbeUnpackVis(v, k) - vis[k]) < 1e-4f;
	Check(visOk, "fp16 visibility pack roundtrip");
}

void TestLightmapCurvatureScale() {
	// Curvature-driven density scale: dihedral/edge-length is the discrete
	// curvature, so the scale must respond to the bend RATE (1/radius),
	// not the tessellation, ignore authored creases, and clamp to [1,max].
	using namespace smvk;
	// Bent-wall helper: M segments of `degPerSeg` around radius r (a
	// cylinder wall strip, height 1). Shared vertical edges carry the
	// smooth dihedral; quad-internal edges are flat.
	auto bentWall = [&](float r, float degPerSeg, uint32_t segs) {
		StaticMeshUpload u;
		const float step = glm::radians(degPerSeg);
		for (uint32_t j = 0; j <= segs; ++j) {
			const float a = float(j) * step;
			for (int y = 0; y <= 1; ++y) {
				StaticVertex v{};
				v.pos = { r * std::cos(a), float(y), r * std::sin(a) };
				v.color = { 1, 1, 1, 1 };
				v.normal = { -std::cos(a), 0.0f, -std::sin(a) };
				u.vertices.push_back(v);
			}
		}
		for (uint32_t j = 0; j < segs; ++j) {
			const uint32_t a0 = j * 2, a1 = a0 + 1;
			const uint32_t b0 = (j + 1) * 2, b1 = b0 + 1;
			u.indices.insert(u.indices.end(), { a0, b0, a1, b0, b1, a1 });
		}
		u.aabbMin = { -r, 0.0f, -r };
		u.aabbMax = { r, 1.0f, r };
		return u;
	};

	// r=1 at 4 texels/unit: 1 rad/m / 4 = ~14.4 deg per texel; target 7
	// deg -> scale ~2.05 (P90 of the equal-length smooth edges is the
	// shared bend curvature ~1.003/r).
	{
		auto wall = bentWall(1.0f, 15.0f, 12);
		const float s = gi::LightmapCurvatureScale(wall, 4.0f, 7.0f, 2.5f);
		CheckNear(s, 2.05f, 0.12f, "lm.curve.scale");
	}
	// Same wall, tighter cap -> exactly the cap.
	{
		auto wall = bentWall(1.0f, 15.0f, 12);
		const float s = gi::LightmapCurvatureScale(wall, 4.0f, 7.0f, 1.5f);
		CheckNear(s, 1.5f, 1e-4f, "lm.curve.clamp");
	}
	// Sharper radius (r=0.25) overshoots the cap by 8x -> cap.
	{
		auto wall = bentWall(0.25f, 15.0f, 12);
		const float s = gi::LightmapCurvatureScale(wall, 4.0f, 7.0f, 2.5f);
		CheckNear(s, 2.5f, 1e-4f, "lm.curve.sharp.clamp");
	}
	// Very gentle bend (r=100): under the target -> no boost.
	{
		auto wall = bentWall(100.0f, 2.0f, 12);
		const float s = gi::LightmapCurvatureScale(wall, 4.0f, 7.0f, 2.5f);
		CheckNear(s, 1.0f, 1e-4f, "lm.curve.gentle.noop");
	}
	// Flat sheet: all dihedrals 0 -> no boost.
	{
		StaticMeshUpload flat;
		for (int y = 0; y <= 2; ++y)
			for (int x = 0; x <= 2; ++x) {
				StaticVertex v{};
				v.pos = { float(x), float(y), 0.0f };
				v.color = { 1, 1, 1, 1 };
				v.normal = { 0, 0, 1 };
				flat.vertices.push_back(v);
			}
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 2; ++x) {
				const uint32_t a = y * 3 + x, b = a + 1, c = a + 3, d = a + 4;
				flat.indices.insert(flat.indices.end(), { a, b, c, b, d, c });
			}
		CheckNear(gi::LightmapCurvatureScale(flat, 4.0f, 7.0f, 2.5f),
			1.0f, 1e-4f, "lm.curve.flat.noop");
	}
	// Crease-only mesh (90 deg folds): authored edges do not count.
	{
		StaticMeshUpload box;
		const glm::vec3 p[8] = {
			{ 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 },
			{ 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 },
		};
		for (const glm::vec3& v : p) {
			StaticVertex sv{};
			sv.pos = v;
			sv.color = { 1, 1, 1, 1 };
			box.vertices.push_back(sv);
		}
		box.indices = {
			0, 1, 2, 0, 2, 3,  // bottom
			4, 7, 6, 4, 6, 5,  // top
			0, 4, 5, 0, 5, 1,  // front
			1, 5, 6, 1, 6, 2,  // right
			2, 6, 7, 2, 7, 3,  // back
			3, 7, 4, 3, 4, 0,  // left
		};
		CheckNear(gi::LightmapCurvatureScale(box, 4.0f, 7.0f, 2.5f),
			1.0f, 1e-4f, "lm.curve.crease.excluded");
	}
	// Degenerate / disabled inputs.
	{
		StaticMeshUpload empty;
		CheckNear(gi::LightmapCurvatureScale(empty, 4.0f, 7.0f, 2.5f),
			1.0f, 1e-4f, "lm.curve.empty");
		auto wall = bentWall(1.0f, 15.0f, 12);
		CheckNear(gi::LightmapCurvatureScale(wall, 4.0f, 0.0f, 2.5f),
			1.0f, 1e-4f, "lm.curve.disabled");
	}
}

// Xatlas charts (GenerateLightmapChartsXatlas — the only chart source).
// The stacked-plate pair reproduces the old unmasked packing's collision
// geometry: every corner at x<0, plates 0.1 m apart in Y. The xatlas
// output is SOURCE-FACE indexed (indexArray[t*3+k] = input triangle t,
// corner k): every triangle keeps exactly its own chart's UVs, never a
// zero/permuted parameterization from a remap.
void TestLmXatlasCharts() {
	using namespace smvk;
	auto plate = [](float y) {
		// 1x1 quad in the XZ plane at height y, entirely at x<0 (the
		// old packing only collided for negative coordinates), CCW
		// seen from +Y.
		StaticMeshUpload u;
		auto vert = [y](float x, float z) {
			StaticVertex v{};
			v.pos = { x, y, z };
			v.color = { 1, 1, 1, 1 };
			v.normal = { 0.0f, 1.0f, 0.0f };
			return v;
		};
		u.vertices = {
			vert(-2.0f, -1.0f), vert(-1.0f, -1.0f),
			vert(-1.0f, 0.0f), vert(-2.0f, 0.0f),
		};
		u.indices = { 0, 2, 1, 0, 3, 2 };
		return u;
	};
	auto stackPlates = [&](StaticMeshUpload& dst, float y0, float y1) {
		for (float y : { y0, y1 }) {
			auto p = plate(y);
			const uint32_t base = uint32_t(dst.vertices.size());
			dst.vertices.insert(dst.vertices.end(),
				p.vertices.begin(), p.vertices.end());
			for (uint32_t ix : p.indices)
				dst.indices.push_back(base + ix);
		}
	};
	std::string err;

	gi::SgLightmapXatlasOptions xopts{};
	xopts.metersPerTexel = 1.0f / 32.0f;
	xopts.gutterTexels = 2;

	// Xatlas on the same stacked plates (the old collision geometry):
	// every triangle charted, the plates in DIFFERENT charts (they
	// share no 3D edge), each triangle a non-degenerate [0,1]
	// parameterization.
	{
		StaticMeshUpload stacked;
		stackPlates(stacked, -1.0f, -0.9f);
		gi::SgLightmapXatlasMeshInput in{};
		in.upload = &stacked;
		in.meshKey = 1;
		std::vector<gi::SgLightmapXatlasMeshInput> ins = { in };
		gi::SgLightmapXatlasLevel lvl;
		err.clear();
		Check(gi::GenerateLightmapChartsXatlas(ins, xopts, lvl, err),
			"lm.xatlas.stacked.ok");
		Check(lvl.meshes.size() == 1 && lvl.meshes[0].ok,
			"lm.xatlas.stacked.meshok");
		if (lvl.meshes.size() == 1 && lvl.meshes[0].ok) {
			const gi::SgLightmapChartResult& r = lvl.meshes[0].chart;
			Check(r.triChart.size() == 4, "lm.xatlas.stacked.tricount");
			bool allCharted = true;
			for (uint32_t t = 0; t < 4; ++t)
				allCharted = allCharted && r.triChart[t] != 0xFFFFFFFFu;
			Check(allCharted, "lm.xatlas.stacked.allcharted");
			Check(r.triChart[0] == r.triChart[1]
				&& r.triChart[2] == r.triChart[3]
				&& r.triChart[0] != r.triChart[2],
				"lm.xatlas.stacked.twocharts");
			bool uvOk = true;
			for (uint32_t t = 0; t < 4; ++t) {
				const glm::vec2& a = r.cornerChartUv[t * 3 + 0];
				const glm::vec2& b = r.cornerChartUv[t * 3 + 1];
				const glm::vec2& c = r.cornerChartUv[t * 3 + 2];
				uvOk = uvOk && a.x >= 0.0f && a.x <= 1.0f
					&& a.y >= 0.0f && a.y <= 1.0f
					&& b.x >= 0.0f && b.x <= 1.0f
					&& b.y >= 0.0f && b.y <= 1.0f
					&& c.x >= 0.0f && c.x <= 1.0f
					&& c.y >= 0.0f && c.y <= 1.0f;
				const glm::vec2 e0 = b - a, e1 = c - a;
				uvOk = uvOk
					&& std::abs(e0.x * e1.y - e0.y * e1.x) > 1e-6f;
			}
			Check(uvOk, "lm.xatlas.stacked.uvs");
			Check(!r.charts.empty()
				&& !r.charts[size_t(r.triChart[0])].texels.empty()
				&& !r.charts[size_t(r.triChart[2])].texels.empty(),
				"lm.xatlas.stacked.texels");
		}
	}

	// Xatlas on a cube in the negative octant: all 12 faces charted,
	// corner uvs in [0,1], every rect inside the packed atlas.
	{
		StaticMeshUpload cube;
		const glm::vec3 p[8] = {
			{ -1, -1, -1 }, { 0, -1, -1 }, { 0, -1, 0 }, { -1, -1, 0 },
			{ -1, 0, -1 }, { 0, 0, -1 }, { 0, 0, 0 }, { -1, 0, 0 },
		};
		for (const glm::vec3& v : p) {
			StaticVertex sv{};
			sv.pos = v;
			sv.color = { 1, 1, 1, 1 };
			cube.vertices.push_back(sv);
		}
		cube.indices = {
			0, 1, 2, 0, 2, 3,  // bottom
			4, 7, 6, 4, 6, 5,  // top
			0, 4, 5, 0, 5, 1,  // front
			1, 5, 6, 1, 6, 2,  // right
			2, 6, 7, 2, 7, 3,  // back
			3, 7, 4, 3, 4, 0,  // left
		};
		gi::SgLightmapXatlasMeshInput in{};
		in.upload = &cube;
		in.meshKey = 2;
		std::vector<gi::SgLightmapXatlasMeshInput> ins = { in };
		gi::SgLightmapXatlasLevel lvl;
		err.clear();
		Check(gi::GenerateLightmapChartsXatlas(ins, xopts, lvl, err),
			"lm.xatlas.cube.ok");
		Check(lvl.meshes.size() == 1 && lvl.meshes[0].ok,
			"lm.xatlas.cube.meshok");
		Check(lvl.atlasWidth > 0 && lvl.atlasHeight > 0,
			"lm.xatlas.cube.atlas");
		if (lvl.meshes.size() == 1 && lvl.meshes[0].ok) {
			const gi::SgLightmapXatlasMeshOutput& mo = lvl.meshes[0];
			const gi::SgLightmapChartResult& r = mo.chart;
			Check(r.triChart.size() == 12, "lm.xatlas.cube.tricount");
			bool allCharted = true;
			for (uint32_t t = 0; t < 12; ++t)
				allCharted = allCharted && r.triChart[t] != 0xFFFFFFFFu;
			Check(allCharted, "lm.xatlas.cube.allcharted");
			bool uvOk = true;
			for (uint32_t t = 0; t < 12; ++t) {
				for (int k = 0; k < 3; ++k) {
					const glm::vec2& u =
						r.cornerChartUv[size_t(t) * 3 + k];
					uvOk = uvOk && u.x >= 0.0f && u.x <= 1.0f
						&& u.y >= 0.0f && u.y <= 1.0f;
				}
			}
			Check(uvOk, "lm.xatlas.cube.uvrange");
			// Rects may poke their margin RING past the atlas border
			// (charts packed against the edge; the ring is gutter,
			// never sampled) — the INNER region must stay inside.
			bool rectsInside = !mo.rects.empty();
			for (const auto& rc : mo.rects)
				rectsInside = rectsInside
					&& rc.minX >= 0.0f && rc.minY >= 0.0f
					&& rc.maxX - float(xopts.gutterTexels)
						<= float(lvl.atlasWidth)
					&& rc.maxY - float(xopts.gutterTexels)
						<= float(lvl.atlasHeight);
			Check(rectsInside, "lm.xatlas.cube.rects");
			bool texels = true;
			for (const auto& c : r.charts)
				texels = texels && !c.texels.empty();
			Check(texels, "lm.xatlas.cube.texels");
		}
	}
}

// Layout-first / deferred rasterize: convergence probes pack + shrink
// only (texels empty), the winner rasterizes once before the bake.
// Layout must already be final + disjoint at probe time; rasterizing
// must fill texels without moving rects; Append must emit disjoint
// chunks (GPU packing has no ownership check).
void TestLmDeferRasterize() {
	using namespace smvk;
	StaticMeshUpload quad;
	{
		auto vert = [](float x, float z) {
			StaticVertex v{};
			v.pos = { x, 0.0f, z };
			v.color = { 1, 1, 1, 1 };
			v.normal = { 0.0f, 1.0f, 0.0f };
			return v;
		};
		quad.vertices = {
			vert(-1.0f, -1.0f), vert(0.0f, -1.0f),
			vert(0.0f, 0.0f), vert(-1.0f, 0.0f),
		};
		quad.indices = { 0, 2, 1, 0, 3, 2 };
	}
	gi::SgLightmapXatlasMeshInput in{};
	in.upload = &quad;
	in.meshKey = 7;
	std::vector<gi::SgLightmapXatlasMeshInput> ins = { in };
	gi::SgLightmapXatlasOptions xopts{};
	xopts.metersPerTexel = 1.0f / 32.0f;
	xopts.gutterTexels = 2;
	xopts.deferTexelRaster = true;
	gi::SgLightmapXatlasLevel lvl;
	std::string err;
	Check(gi::GenerateLightmapChartsXatlas(ins, xopts, lvl, err),
		"lm.defer.pack.ok");
	Check(lvl.meshes.size() == 1 && lvl.meshes[0].ok,
		"lm.defer.pack.meshok");
	if (lvl.meshes.size() == 1 && lvl.meshes[0].ok) {
		bool texelsEmpty = true;
		for (const auto& c : lvl.meshes[0].chart.charts)
			texelsEmpty = texelsEmpty && c.texels.empty();
		Check(texelsEmpty, "lm.defer.pack.texels.empty");
		Check(!lvl.meshes[0].rects.empty(), "lm.defer.pack.rects");
		std::string oerr;
		Check(gi::CheckXatlasLevelRectsForOverlap(lvl, &oerr),
			"lm.defer.pack.nooverlap");
		// Winner rasterize: fills texels, rects must not move.
		auto rectsBefore = lvl.meshes[0].rects;
		Check(gi::RasterizeLightmapPackTexels(ins, lvl, 2, err),
			"lm.defer.raster.ok");
		Check(lvl.meshes[0].rects.size() == rectsBefore.size(),
			"lm.defer.raster.rectcount");
		bool rectsSame = lvl.meshes[0].rects.size() == rectsBefore.size();
		for (size_t i = 0; rectsSame && i < rectsBefore.size(); ++i) {
			rectsSame = rectsSame
				&& lvl.meshes[0].rects[i].minX == rectsBefore[i].minX
				&& lvl.meshes[0].rects[i].minY == rectsBefore[i].minY
				&& lvl.meshes[0].rects[i].maxX == rectsBefore[i].maxX
				&& lvl.meshes[0].rects[i].maxY == rectsBefore[i].maxY;
		}
		Check(rectsSame, "lm.defer.raster.rects.stable");
		bool texelsFull = true;
		for (const auto& c : lvl.meshes[0].chart.charts)
			texelsFull = texelsFull && !c.texels.empty();
		Check(texelsFull, "lm.defer.raster.texels");
		Check(gi::CheckXatlasLevelRectsForOverlap(lvl, &oerr),
			"lm.defer.raster.nooverlap");
		// Append: chunks disjoint (second gate).
		gi::SgLightmapAtlas atlas;
		std::vector<uint32_t> bases;
		std::vector<uint32_t> keys = { 7 };
		Check(gi::AppendXatlasLevelToAtlas(atlas, lvl, keys, 9u,
				1024, 1024, bases, err),
			"lm.defer.append.ok");
		Check(gi::CheckAtlasChunksForOverlap(atlas, &oerr),
			"lm.defer.chunks.nooverlap");
	}
}

// Repack: post-shrink AABBs of concave charts are expected to overlap
// (xatlas only promises dilated-coverage disjointness). RepackXatlasLevel
// forgets the canvas placement and shelf-packs big->small into disjoint
// rects, preserving sizes; chart-local uvs are placement-independent.
void TestLmRepackDisjoint() {
	gi::SgLightmapXatlasLevel lvl;
	lvl.meshes.resize(1);
	auto& mo = lvl.meshes[0];
	mo.ok = true;
	auto addChart = [&](float minX, float minY, float w, float h) {
		gi::SgLightmapXatlasMeshOutput::Rect r;
		r.minX = minX;
		r.minY = minY;
		r.maxX = minX + w;
		r.maxY = minY + h;
		mo.rects.push_back(r);
		gi::SgLightmapChartResult::Chart c;
		c.width = uint32_t(w);
		c.height = uint32_t(h);
		mo.chart.charts.push_back(std::move(c));
	};
	// Three overlapping AABBs: big 20x10, medium 10x10, small 6x6.
	addChart(0.0f, 0.0f, 20.0f, 10.0f);
	addChart(5.0f, 3.0f, 10.0f, 10.0f);
	addChart(8.0f, 2.0f, 6.0f, 6.0f);
	lvl.atlasWidth = 30;
	lvl.atlasHeight = 20;
	std::string oerr;
	Check(!gi::CheckXatlasLevelRectsForOverlap(lvl, &oerr),
		"lm.repack.pre.overlap");
	std::string err;
	Check(gi::RepackXatlasLevel(lvl, 64, 64, err)
		== gi::RepackStatus::Ok, "lm.repack.ok");
	Check(gi::CheckXatlasLevelRectsForOverlap(lvl, &oerr),
		"lm.repack.nooverlap");
	// Sizes preserved exactly (only placement moves).
	Check(mo.rects.size() == 3, "lm.repack.count");
	bool sizesOk = mo.rects.size() == 3;
	{
		bool seen20x10 = false, seen10x10 = false, seen6x6 = false;
		for (const auto& r : mo.rects) {
			const float w = r.maxX - r.minX, h = r.maxY - r.minY;
			if (w == 20.0f && h == 10.0f)
				seen20x10 = true;
			else if (w == 10.0f && h == 10.0f)
				seen10x10 = true;
			else if (w == 6.0f && h == 6.0f)
				seen6x6 = true;
			else
				sizesOk = false;
		}
		sizesOk = sizesOk && seen20x10 && seen10x10 && seen6x6;
	}
	Check(sizesOk, "lm.repack.sizes");
	// Big first: the 20-wide chart must start at the origin row.
	bool bigFirst = false;
	for (const auto& r : mo.rects) {
		if (r.maxX - r.minX == 20.0f && r.maxY - r.minY == 10.0f)
			bigFirst = (r.minX == 0.0f && r.minY == 0.0f);
	}
	Check(bigFirst, "lm.repack.bigfirst");
	// Chart payload dims track the rect spans.
	bool dimsOk = true;
	for (size_t i = 0; i < mo.rects.size(); ++i) {
		dimsOk = dimsOk
			&& mo.chart.charts[i].width
				== uint32_t(mo.rects[i].maxX - mo.rects[i].minX)
			&& mo.chart.charts[i].height
				== uint32_t(mo.rects[i].maxY - mo.rects[i].minY);
	}
	Check(dimsOk, "lm.repack.dims");
	// Repacked level appends cleanly with disjoint chunks.
	gi::SgLightmapAtlas atlas;
	std::vector<uint32_t> bases;
	std::vector<uint32_t> keys = { 1 };
	Check(gi::AppendXatlasLevelToAtlas(atlas, lvl, keys, 9u, 64, 64, bases,
			err),
		"lm.repack.append.ok");
	Check(gi::CheckAtlasChunksForOverlap(atlas, &oerr),
		"lm.repack.chunks.nooverlap");
	// Oversize chart fails with a useful error.
	gi::SgLightmapXatlasLevel tooWide;
	tooWide.meshes.resize(1);
	tooWide.meshes[0].ok = true;
	gi::SgLightmapXatlasMeshOutput::Rect wr{ 0, 0, 100, 4 };
	tooWide.meshes[0].rects.push_back(wr);
	gi::SgLightmapChartResult::Chart wc;
	wc.width = 100;
	wc.height = 4;
	tooWide.meshes[0].chart.charts.push_back(std::move(wc));
	Check(gi::RepackXatlasLevel(tooWide, 64, 64, err)
		== gi::RepackStatus::Infeasible,
		"lm.repack.toowide.infeasible");
	// Genuinely tight fit (total area just under the cap): Infeasible,
	// NOT an internal error — callers converge the density.
	{
		gi::SgLightmapXatlasLevel tight;
		tight.meshes.resize(1);
		tight.meshes[0].ok = true;
		// 64x64 cap = 4096; 25 charts of 12x12 = 3600 raw, ~4900
		// sep-inflated — near-capacity, a legit no-fit.
		for (int i = 0; i < 25; ++i) {
			gi::SgLightmapXatlasMeshOutput::Rect r{ 0, 0, 12, 12 };
			tight.meshes[0].rects.push_back(r);
			gi::SgLightmapChartResult::Chart c;
			c.width = 12;
			c.height = 12;
			tight.meshes[0].chart.charts.push_back(std::move(c));
		}
		const gi::RepackStatus rs =
			gi::RepackXatlasLevel(tight, 64, 64, err);
		Check(rs == gi::RepackStatus::Infeasible,
			"lm.repack.tight.infeasible");
	}
	// Packer-bug class: the whole sep-inflated job fits under HALF the
	// atlas area, yet no placement — InternalError, callers must abort.
	{
		gi::SgLightmapXatlasLevel buggy;
		buggy.meshes.resize(1);
		buggy.meshes[0].ok = true;
		// 10 charts of 8x8 = 640 raw, ~1600 inflated, cap 4096: 39% of
		// the atlas. A healthy placer fits this trivially; failing it
		// would be a stranded-space bug like the old guillotine one.
		for (int i = 0; i < 10; ++i) {
			gi::SgLightmapXatlasMeshOutput::Rect r{ 0, 0, 8, 8 };
			buggy.meshes[0].rects.push_back(r);
			gi::SgLightmapChartResult::Chart c;
			c.width = 8;
			c.height = 8;
			buggy.meshes[0].chart.charts.push_back(std::move(c));
		}
		const gi::RepackStatus rs =
			gi::RepackXatlasLevel(buggy, 64, 64, err);
		// These must FIT (the placer is correct today); the InternalError
		// branch is exercised only by real placement bugs. Assert Ok so
		// a future placer regression downgrades to a visible test fail
		// instead of a silent InternalError in cooks.
		Check(rs == gi::RepackStatus::Ok, "lm.repack.smallfit.ok");
	}
}

// Seam stitching (StitchSgLightmapSeams): boundary texels of sibling
// chunks blend toward same-normal foreign texels; interiors, opposite
// normals and lobe-count mismatches are untouched.
void TestLmSeamStitch() {
	// Two 7x7 payloads (lobeCount 5), 4x4 claimed blocks: boundary =
	// all but the center 2x2. A sits at x in [0.1,0.4], B adjacent at
	// x in [0.41,0.71] (0.01 gap, well inside 2.5x the 0.1 texel size).
	auto makeChunk = [](float x0, const glm::vec3& fill,
		const glm::vec3& nrm, std::vector<gi::SgLightmapTexel>& texels) {
		gi::SgLightmapAtlas::ChunkPayload p;
		p.width = p.height = 7;
		p.coeffs.assign(size_t(7) * 7 * 5, glm::vec3(0.0f));
		texels.clear();
		for (uint32_t y = 0; y < 4; ++y)
			for (uint32_t x = 0; x < 4; ++x) {
				gi::SgLightmapTexel t{};
				t.position = glm::vec3(x0 + 0.1f * float(x), 0.0f,
					0.1f * float(y));
				t.normal = nrm;
				t.uv = glm::vec2(1.5f + float(x), 1.5f + float(y));
				texels.push_back(t);
				for (uint32_t l = 0; l < 5; ++l)
					p.coeffs[(size_t(1 + y) * 7 + (1 + x)) * 5 + l] = fill;
			}
		return p;
	};
	std::vector<gi::SgLightmapTexel> texA, texB;
	gi::SgLightmapAtlas::ChunkPayload payA =
		makeChunk(0.1f, glm::vec3(10.0f), glm::vec3(0, 1, 0), texA);
	gi::SgLightmapAtlas::ChunkPayload payB =
		makeChunk(0.41f, glm::vec3(0.0f), glm::vec3(0, 1, 0), texB);
	gi::SgLightmapChunkBatch bA{ &payA, &texA, 1, nullptr };
	gi::SgLightmapChunkBatch bB{ &payB, &texB, 1, nullptr };
	std::vector<gi::SgLightmapChunkBatch> batches = { bA, bB };
	std::string err;
	uint64_t blended = 0;
	Check(gi::StitchSgLightmapSeams(batches, 0.5f, &blended, err),
		"lm.stitch.ok");
	Check(blended > 0, "lm.stitch.blended");
	// Boundary texels met halfway (10 -> 5, 0 -> 5); the center 2x2 of
	// each block is interior and must be untouched.
	auto amp = [](const gi::SgLightmapAtlas::ChunkPayload& p,
		uint32_t x, uint32_t y) {
		return p.coeffs[(size_t(y) * 7 + x) * 5].x;
	};
	Check(std::abs(amp(payA, 4, 2) - 5.0f) < 1e-4f, "lm.stitch.a.edge");
	Check(std::abs(amp(payB, 1, 2) - 5.0f) < 1e-4f, "lm.stitch.b.edge");
	Check(std::abs(amp(payA, 2, 2) - 10.0f) < 1e-4f,
		"lm.stitch.a.interior");
	Check(std::abs(amp(payB, 2, 2) - 0.0f) < 1e-4f,
		"lm.stitch.b.interior");
	// Opposite normals: no blending (thin double-sided guard).
	std::vector<gi::SgLightmapTexel> texC, texD;
	gi::SgLightmapAtlas::ChunkPayload payC =
		makeChunk(0.1f, glm::vec3(10.0f), glm::vec3(0, 1, 0), texC);
	gi::SgLightmapAtlas::ChunkPayload payD =
		makeChunk(0.41f, glm::vec3(0.0f), glm::vec3(0, -1, 0), texD);
	std::vector<gi::SgLightmapChunkBatch> batches2 = {
		{ &payC, &texC, 2, nullptr }, { &payD, &texD, 2, nullptr } };
	blended = 0;
	Check(gi::StitchSgLightmapSeams(batches2, 0.5f, &blended, err),
		"lm.stitch.flip.ok");
	Check(blended == 0, "lm.stitch.flip.none");
	Check(std::abs(amp(payC, 4, 2) - 10.0f) < 1e-4f,
		"lm.stitch.flip.untouched");
	// Bad blend factor fails cleanly.
	Check(!gi::StitchSgLightmapSeams(batches, 0.0f, &blended, err),
		"lm.stitch.badblend.fail");
}

// Frostbite-style chart transfer (TransferLightmapCharts): a charted
// reference (the LOD1 stand-in) projects onto tessellated targets of
// the same surface — every triangle lands in ONE chart with a
// non-degenerate [0,1] parameterization; a target triangle SPANNING
// two reference charts picks the majority chart and re-projects its
// off-chart corner into it (never a two-rect span); transferring the
// reference onto itself round-trips the layout; a corrupted reference
// fails cleanly.
void TestLmChartTransfer() {
	using namespace smvk;
	auto buildQuad = [](float x0, float z0, float x1, float z1, float y,
		uint32_t segs, StaticMeshUpload& u) {
		// XZ-plane quad [x0,x1]x[z0,z1] at height y, segs x segs grid,
		// CCW seen from +Y.
		for (uint32_t j = 0; j <= segs; ++j)
			for (uint32_t i = 0; i <= segs; ++i) {
				StaticVertex v{};
				v.pos = { x0 + (x1 - x0) * float(i) / float(segs), y,
					z0 + (z1 - z0) * float(j) / float(segs) };
				v.color = { 1, 1, 1, 1 };
				v.normal = { 0.0f, 1.0f, 0.0f };
				u.vertices.push_back(v);
			}
		auto ix = [&](uint32_t i, uint32_t j) {
			return j * (segs + 1) + i;
		};
		for (uint32_t j = 0; j < segs; ++j)
			for (uint32_t i = 0; i < segs; ++i) {
				const uint32_t a = ix(i, j), b = ix(i + 1, j),
					c = ix(i + 1, j + 1), d = ix(i, j + 1);
				u.indices.insert(u.indices.end(),
					{ a, c, b, a, d, c });
			}
	};
	auto chartSingle = [&](const StaticMeshUpload& up,
		gi::SgLightmapChartResult& out, const char* okName,
		const char* meshName) {
		gi::SgLightmapXatlasMeshInput in{};
		in.upload = &up;
		std::vector<gi::SgLightmapXatlasMeshInput> ins = { in };
		gi::SgLightmapXatlasOptions xopts{};
		xopts.metersPerTexel = 1.0f / 32.0f;
		xopts.gutterTexels = 2;
		gi::SgLightmapXatlasLevel lvl;
		std::string xerr;
		const bool ok = gi::GenerateLightmapChartsXatlas(ins, xopts,
			lvl, xerr);
		Check(ok, okName);
		if (ok && lvl.meshes.size() == 1 && lvl.meshes[0].ok) {
			out = lvl.meshes[0].chart;
			return true;
		}
		Check(false, meshName);
		return false;
	};
	std::string err;

	// Reference: a single 2x2 quad at y=-1 (one chart).
	StaticMeshUpload refUp;
	buildQuad(-2.0f, -1.0f, 0.0f, 1.0f, -1.0f, 2, refUp);
	gi::SgLightmapChartResult ref;
	if (chartSingle(refUp, ref, "lm.xfer.ref.ok",
			"lm.xfer.ref.meshok")) {
		Check(ref.charts.size() == 1, "lm.xfer.ref.onechart");
		// Round-trip: the reference onto itself reproduces its own
		// layout (every corner projects exactly onto itself).
		{
			gi::SgLightmapChartResult rt;
			err.clear();
			Check(gi::TransferLightmapCharts(refUp, ref, refUp, rt, err),
				"lm.xfer.self.ok");
			bool same = rt.triChart.size() == ref.triChart.size();
			const uint32_t tris = uint32_t(ref.triChart.size());
			for (uint32_t t = 0; same && t < tris; ++t) {
				same = rt.triChart[t] == ref.triChart[t];
				for (int k = 0; same && k < 3; ++k)
					same = glm::length(rt.cornerChartUv[t * 3 + k]
						- ref.cornerChartUv[t * 3 + k]) < 1e-4f;
			}
			Check(same, "lm.xfer.self.roundtrip");
		}
		// Finer target (7x7): all triangles in the single reference
		// chart, uvs in [0,1], non-degenerate parameterization.
		{
			StaticMeshUpload t;
			buildQuad(-2.0f, -1.0f, 0.0f, 1.0f, -1.0f, 7, t);
			gi::SgLightmapChartResult xfer;
			err.clear();
			Check(gi::TransferLightmapCharts(refUp, ref, t, xfer, err),
				"lm.xfer.fine.ok");
			const uint32_t tris = uint32_t(t.indices.size() / 3);
			Check(xfer.triChart.size() == tris
				&& xfer.cornerChartUv.size() == size_t(tris) * 3,
				"lm.xfer.fine.sizes");
			bool allCharted = true, uvOk = true;
			for (uint32_t tr = 0; tr < tris; ++tr) {
				allCharted = allCharted
					&& xfer.triChart[tr] != 0xFFFFFFFFu;
				const glm::vec2& a = xfer.cornerChartUv[tr * 3 + 0];
				const glm::vec2& b = xfer.cornerChartUv[tr * 3 + 1];
				const glm::vec2& c = xfer.cornerChartUv[tr * 3 + 2];
				const glm::vec2 e0 = b - a, e1 = c - a;
				uvOk = uvOk
					&& a.x >= 0.0f && a.x <= 1.0f && a.y >= 0.0f && a.y <= 1.0f
					&& b.x >= 0.0f && b.x <= 1.0f && b.y >= 0.0f && b.y <= 1.0f
					&& c.x >= 0.0f && c.x <= 1.0f && c.y >= 0.0f && c.y <= 1.0f
					&& std::abs(e0.x * e1.y - e0.y * e1.x) > 1e-6f;
			}
			Check(allCharted, "lm.xfer.fine.allcharted");
			Check(uvOk, "lm.xfer.fine.uvs");
		}
		// Corrupted reference sizes fail cleanly.
		{
			StaticMeshUpload t;
			buildQuad(-2.0f, -1.0f, 0.0f, 1.0f, -1.0f, 2, t);
			gi::SgLightmapChartResult bad = ref;
			bad.triChart.pop_back();
			gi::SgLightmapChartResult xfer;
			err.clear();
			Check(!gi::TransferLightmapCharts(refUp, bad, t, xfer, err)
				&& !err.empty(), "lm.xfer.badref.fail");
		}
	}

	// Seam-spanning target: reference = TWO disconnected quads (two
	// charts); target = one quad covering both plus the gap between
	// them. Every target triangle must land in a SINGLE chart
	// (majority + restricted re-projection for off-chart corners).
	{
		StaticMeshUpload spanRef;
		buildQuad(-2.0f, -1.0f, 0.0f, 1.0f, -1.0f, 1, spanRef);
		buildQuad(1.0f, -1.0f, 3.0f, 1.0f, -1.0f, 1, spanRef);
		gi::SgLightmapChartResult rc;
		if (chartSingle(spanRef, rc, "lm.xfer.span.ref.ok",
				"lm.xfer.span.ref.meshok")) {
			Check(rc.charts.size() == 2, "lm.xfer.span.ref.twocharts");
			StaticMeshUpload t;
			buildQuad(-2.0f, -1.0f, 3.0f, 1.0f, -1.0f, 5, t);
			gi::SgLightmapChartResult xfer;
			err.clear();
			Check(gi::TransferLightmapCharts(spanRef, rc, t, xfer, err),
				"lm.xfer.span.ok");
			// All triangles single-chart + in-range. NOTE: target
			// triangles hovering in the synthetic GAP between the two
			// charts get the degenerate RESCUE: planar barycentric
			// extrapolation onto the closest reference triangle,
			// clamped to [0,1] — distinct corners on the rect edge
			// instead of collapsing onto one point (that geometry does
			// not exist on the reference; real LOD chains have no
			// gaps, so the span test checks the re-projection path
			// stays in-range, not the uv area).
			const uint32_t tris = uint32_t(t.indices.size() / 3);
			bool allCharted = true, uvOk = true;
			for (uint32_t tr = 0; tr < tris; ++tr) {
				allCharted = allCharted
					&& xfer.triChart[tr] != 0xFFFFFFFFu;
				for (int k = 0; k < 3; ++k) {
					const glm::vec2& u = xfer.cornerChartUv[tr * 3 + k];
					uvOk = uvOk && u.x >= 0.0f && u.x <= 1.0f
						&& u.y >= 0.0f && u.y <= 1.0f;
				}
			}
			Check(allCharted, "lm.xfer.span.allcharted");
			Check(uvOk, "lm.xfer.span.uvs");
		}
	}
}

// Scene-authored static lights (SceneLight::kSceneLightFlag_Static) are
// bake sources: CollectStaticBakeLights converts spots/points to
// GiTraceSpot NEE lights and seeds the bake sun from the first static
// directional; unflagged lights are never baked.
void TestStaticBakeLights() {
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetOfflineMode(true);
	::Scene& scene = cluster->scene;
	addNode(scene, -1, 0); // root

	auto addLightNode = [&](glm::vec3 pos, const SceneLight& def) {
		const int n = addNode(scene, 0, 1);
		glm::mat4 g(1.0f);
		g[3] = glm::vec4(pos, 1.0f);
		scene.localTransform[n] = g;
		scene.globalTransform[n] = g;
		const smvk::ClusterLightHandle h = cluster->CreateLight(def);
		scene.lightForNode[(uint32_t)n] = h.index();
		return n;
	};

	// Static spot at (0,2,0), identity rotation -> node -Z aims at
	// (0,0,-1) (same aim convention the runtime cooked path uses).
	SceneLight spotDef;
	spotDef.type = SceneLight::Type::Spot;
	spotDef.color = glm::vec3(1.0f, 0.5f, 0.25f);
	spotDef.intensity = 64.0f; // candela
	spotDef.range = 12.0f;
	spotDef.innerConeAngle = glm::radians(15.0f);
	spotDef.outerConeAngle = glm::radians(30.0f);
	spotDef.flags = SceneLight::kSceneLightFlag_Static;
	addLightNode(glm::vec3(0.0f, 2.0f, 0.0f), spotDef);

	// Static point at (0,5,0).
	SceneLight pointDef;
	pointDef.type = SceneLight::Type::Point;
	pointDef.color = glm::vec3(0.25f, 0.5f, 1.0f);
	pointDef.intensity = 100.0f;
	pointDef.range = 6.0f;
	pointDef.flags = SceneLight::kSceneLightFlag_Static;
	addLightNode(glm::vec3(0.0f, 5.0f, 0.0f), pointDef);

	// UNFLAGGED spot: runtime-only, must never reach the bake.
	SceneLight dynDef;
	dynDef.type = SceneLight::Type::Spot;
	dynDef.intensity = 500.0f;
	addLightNode(glm::vec3(1.0f, 1.0f, 1.0f), dynDef);

	// Static directional: identity rotation -> emis (0,0,-1) -> sun
	// direction (0,0,1). A second static directional must NOT override.
	SceneLight sunDef;
	sunDef.type = SceneLight::Type::Directional;
	sunDef.color = glm::vec3(1.0f, 0.95f, 0.9f);
	sunDef.intensity = 20000.0f; // lux
	sunDef.flags = SceneLight::kSceneLightFlag_Static;
	addLightNode(glm::vec3(0.0f, 10.0f, 0.0f), sunDef);
	SceneLight sunDef2 = sunDef;
	sunDef2.intensity = 1.0f;
	sunDef2.color = glm::vec3(0.0f);
	addLightNode(glm::vec3(2.0f, 10.0f, 0.0f), sunDef2);

	gi::GiTraceScene ts;
	const gi::GiStaticLightCounts counts = gi::CollectStaticBakeLights(*cluster, ts);
	Check(counts.spots == 1 && counts.points == 1 && counts.suns == 1,
		"static.collect.counts");
	Check(ts.spots.size() == 2, "static.collect.two");

	// Spot conversion: world position, aim direction, candela->radiance
	// scale, cone cosines (inner clamps to the smaller angle).
	bool spotOk = ts.spots.size() >= 1;
	if (spotOk) {
		const gi::GiTraceSpot& s = ts.spots[0];
		spotOk = glm::all(glm::equal(s.position, glm::vec3(0.0f, 2.0f, 0.0f)))
			&& glm::all(glm::equal(s.direction, glm::vec3(0.0f, 0.0f, -1.0f)))
			&& s.range == 12.0f
			&& glm::all(glm::equal(
				s.color, glm::vec3(1.0f, 0.5f, 0.25f) * 64.0f))
			&& std::abs(s.cosInner - std::cos(glm::radians(15.0f))) < 1e-6f
			&& std::abs(s.cosOuter - std::cos(glm::radians(30.0f))) < 1e-6f;
	}
	Check(spotOk, "static.collect.spot.fields");

	// Point conversion: full-cone spot (cosInner == cosOuter == -1).
	bool pointOk = ts.spots.size() >= 2;
	if (pointOk) {
		const gi::GiTraceSpot& s = ts.spots[1];
		pointOk = glm::all(glm::equal(s.position, glm::vec3(0.0f, 5.0f, 0.0f)))
			&& s.cosInner == -1.0f && s.cosOuter == -1.0f
			&& s.range == 6.0f
			&& glm::all(glm::equal(
				s.color, glm::vec3(0.25f, 0.5f, 1.0f) * 100.0f));
	}
	Check(pointOk, "static.collect.point.fullcone");

	// Sun seeding: first static directional wins.
	bool sunOk = glm::all(glm::equal(ts.sunDirection, glm::vec3(0.0f, 0.0f, 1.0f)))
		&& glm::all(glm::equal(ts.sunColor, glm::vec3(1.0f, 0.95f, 0.9f)))
		&& ts.sunIntensity == 20000.0f;
	Check(sunOk, "static.collect.sun.firstwins");

	// Aim axis convention (GltfPunctualAimAxis): raw locals (the coordFix
	// basis rides the imported chain's TOP node), so the glTF -Z aim
	// reads (0,0,-1) in engine-local space. A 90-degree Y rotation aims
	// the default (0,0,-1) axis at -X; an explicit (0,0,1) axis at +X.
	// Runtime + bake share the convention.
	{
		auto cluster2 = std::make_unique<smvk::Cluster>(nullptr);
		cluster2->SetOfflineMode(true);
		::Scene& scene2 = cluster2->scene;
		addNode(scene2, -1, 0); // root
		glm::mat4 g = glm::mat4_cast(
			glm::quat(0.70710678118f, 0.0f, 0.70710678118f, 0.0f)); // rotY(90)
		{
			const int n = addNode(scene2, 0, 1);
			scene2.localTransform[n] = g;
			scene2.globalTransform[n] = g;
			const smvk::ClusterLightHandle h = cluster2->CreateLight(spotDef);
			scene2.lightForNode[(uint32_t)n] = h.index();
		}
		{
			const int n = addNode(scene2, 0, 1);
			scene2.localTransform[n] = g;
			scene2.globalTransform[n] = g;
			const smvk::ClusterLightHandle h = cluster2->CreateLight(sunDef);
			scene2.lightForNode[(uint32_t)n] = h.index();
		}
		gi::GiTraceScene tsA, tsB;
		CollectStaticBakeLights(*cluster2, tsA); // engine-space default axis
		CollectStaticBakeLights(*cluster2, tsB, glm::vec3(0.0f, 0.0f, 1.0f));
		Check(tsA.spots.size() == 1 && tsB.spots.size() == 1
			&& tsA.spots.size() == tsB.spots.size(),
			"static.aimaxis.spot.count");
		if (tsA.spots.size() == 1 && tsB.spots.size() == 1) {
			Check(glm::all(glm::lessThan(glm::abs(
					tsA.spots[0].direction - glm::vec3(-1.0f, 0.0f, 0.0f)),
					glm::vec3(1e-6f)))
				&& glm::all(glm::lessThan(glm::abs(
					tsB.spots[0].direction - glm::vec3(1.0f, 0.0f, 0.0f)),
					glm::vec3(1e-6f))),
				"static.aimaxis.spot.aim");
		}
		Check(glm::all(glm::lessThan(glm::abs(
				tsA.sunDirection - glm::vec3(1.0f, 0.0f, 0.0f)),
				glm::vec3(1e-6f)))
			&& glm::all(glm::lessThan(glm::abs(
				tsB.sunDirection - glm::vec3(-1.0f, 0.0f, 0.0f)),
				glm::vec3(1e-6f))),
			"static.aimaxis.sun.direction");
	}

	// Numeric: the converted point behaves EXACTLY like a point light —
	// receiver 1 m below the point, outside the static spot's cone
	// (cosA = 0 < cosOuter(30 deg) there), so only the point term shows.
	// Frostbite windowed inverse-square (mirror of SampleSpots):
	//   window = (1 - (d/range)^4)^2, atten = window / (4 pi d^2)
	ts.Finalize(); // empty geometry: BVH hit test stays clear
	gi::GiTraceMaterial mat;
	mat.albedo = glm::vec3(1.0f);
	const glm::vec3 pos(0.0f, 1.0f, 0.0f);
	const glm::vec3 n(0.0f, 1.0f, 0.0f);
	const glm::vec3 got = ts.SampleSpots(pos, n, mat);
	const float dist = 4.0f;
	const float x = dist / 6.0f;
	const float window_ = (1.0f - x * x * x * x) * (1.0f - x * x * x * x);
	const float atten = window_ / (4.0f * kPi * dist * dist);
	const glm::vec3 expect =
		glm::vec3(0.25f, 0.5f, 1.0f) * 100.0f * atten / kPi;
	Check(glm::all(glm::lessThan(
			glm::abs(got - expect), glm::vec3(1e-5f))),
		"static.collect.point.falloff");
}

// Finalize() scene validation: a malformed trace soup (short material
// map, out-of-range material/vertex indices, geometry with no
// materials) must become consistent data — repaired/dropped with a
// stderr report, never traced as garbage (OOB reads surface as NaN
// radiance downstream).
void TestGiMalformedScene() {
	{
		// Material map one entry short: padded with material 0; both
		// triangles survive with a valid shading material.
		GiTraceScene scene;
		scene.positions = {
			{ -1, 0, -1 }, { 1, 0, -1 }, { 1, 0, 1 }, { -1, 0, 1 } };
		scene.indices = { 0, 1, 2, 0, 2, 3 };
		scene.materials = { GiTraceMaterial{ glm::vec3(0.7f), glm::vec3(0.0f) } };
		scene.triMaterial = { 0 };
		scene.Finalize();
		Check(scene.triMaterial.size() == 2, "gi.malformed.matmap.padded");
		Check(scene.Bvh().TriangleCount() == 2, "gi.malformed.matmap.tris");
	}
	{
		// Out-of-range vertex index (tri 0) and out-of-range material
		// index (tri 1): both triangles drop; the healthy one survives.
		GiTraceScene scene;
		scene.positions = {
			{ -1, 0, -1 }, { 1, 0, -1 }, { 1, 0, 1 }, { -1, 0, 1 } };
		scene.indices = { 0, 1, 99, 0, 1, 2, 0, 2, 3 };
		scene.materials = { GiTraceMaterial{ glm::vec3(0.7f), glm::vec3(0.0f) } };
		scene.triMaterial = { 0, 7, 0 };
		scene.Finalize();
		Check(scene.triMaterial.size() == 1, "gi.malformed.badidx.matmap");
		Check(scene.Bvh().TriangleCount() == 1, "gi.malformed.badidx.dropped");
	}
	{
		// Geometry with an empty material list: a default material is
		// injected, the (empty) map pads to one entry, and tracing
		// through the repaired scene stays finite end to end.
		GiTraceScene scene;
		scene.positions = { { -1, 0, -1 }, { 1, 0, -1 }, { 1, 0, 1 } };
		scene.indices = { 0, 1, 2 };
		scene.triMaterial = {};
		scene.sunIntensity = 1000.0f;
		scene.Finalize();
		Check(scene.materials.size() == 1, "gi.malformed.defaultmat.injected");
		Check(scene.Bvh().TriangleCount() == 1, "gi.malformed.defaultmat.tris");
		std::mt19937 srng(5);
		Pcg32 rng(11);
		bool allFinite = true;
		for (int i = 0; i < 16; ++i) {
			const glm::vec3 L = scene.TraceIncoming(
				glm::vec3(0.0f, 1.0f, 0.0f), UniformDir(srng), rng, 2);
			if (!std::isfinite(L.x + L.y + L.z))
				allFinite = false;
		}
		Check(allFinite, "gi.malformed.trace.finite");
	}
}

// .sglm roundtrip: the writer stores the section as segmented LZ4 (a 16M-texel
// atlas packs ~1.2 GB, over the single-block codec's kMaxBlobBytes cap
// — the failure that killed the first night bistro cook), and the
// reader keeps v1 single-block files alive.
void TestLightmapAtlasRoundtrip() {
	SgLightmapAtlas a{};
	a.width = 64;
	a.height = 32;
	const uint32_t lobeCounts[2] = { 5, 12 };
	for (uint32_t ci = 0; ci < 2; ++ci) {
		SgLightmapChunk c{};
		c.lobeCount = lobeCounts[ci];
		c.meshKey = 100 + ci;
		c.rectMin[0] = float(ci * 16);
		c.rectMin[1] = 0.0f;
		c.rectMax[0] = float(ci * 16 + 16);
		c.rectMax[1] = 8.0f;
		a.chunks.push_back(c);
		SgLightmapAtlas::ChunkPayload p{};
		p.width = 16;
		p.height = 8;
		const uint32_t n = c.lobeCount;
		p.coeffs.assign(size_t(p.width) * p.height * n, glm::vec3(0.0f));
		for (size_t i = 0; i < p.coeffs.size(); ++i) {
			const float f = float(i % 97) * 0.031f + float(ci);
			p.coeffs[i] = glm::vec3(f, f * 0.5f + 1.0f, f * 0.25f);
		}
		a.payloads.push_back(std::move(p));
	}

	auto payloadClose = [](const SgLightmapAtlas& x,
			const SgLightmapAtlas& y) {
		if (x.chunks.size() != y.chunks.size()
			|| x.payloads.size() != y.payloads.size()
			|| x.width != y.width || x.height != y.height)
			return false;
		for (size_t ci = 0; ci < x.chunks.size(); ++ci) {
			const SgLightmapChunk& cx = x.chunks[ci];
			const SgLightmapChunk& cy = y.chunks[ci];
			if (cx.lobeCount != cy.lobeCount || cx.meshKey != cy.meshKey)
				return false;
			for (int k = 0; k < 2; ++k)
				if (std::abs(cx.rectMin[k] - cy.rectMin[k]) > 1e-6f
					|| std::abs(cx.rectMax[k] - cy.rectMax[k]) > 1e-6f)
					return false;
			const auto& px = x.payloads[ci];
			const auto& py = y.payloads[ci];
			if (px.width != py.width || px.height != py.height
				|| px.coeffs.size() != py.coeffs.size())
				return false;
			for (size_t i = 0; i < px.coeffs.size(); ++i)
				// fp16 storage: half-precision relative slack.
				if (glm::length(px.coeffs[i] - py.coeffs[i]) > 1e-2f)
					return false;
		}
		return true;
	};

	const auto tmp = std::filesystem::temp_directory_path()
		/ "tourmaline_gi_test.sglm";
	std::string err;
	Check(a.WriteFile(tmp.string(), err), "sglm write");
	SgLightmapAtlas b;
	Check(SgLightmapAtlas::ReadFile(tmp.string(), b, err), "sglm read");
	Check(payloadClose(a, b), "sglm roundtrip equal");

	// BC6H (bc6hopt): compress -> write -> read keeps the block
	// payload verbatim, reconstructs the chunk payloads from the blocks
	// EXACTLY as a direct decode would (the read path must not re-derive
	// anything), and the decoded content tracks the fixture coarsely
	// (block compression is lossy; the fixture's i%97 pattern is
	// high-frequency noise on purpose).
	{
		Check(a.CompressToBc6h(true, err), "sglm bc6h compress");
		Check(a.compression == SgLightmapCompression::Bc6HOpt
			&& a.bc6hLobes == 12
			&& a.bc6hBlocks.size()
				== size_t(a.width / 4) * (a.height / 4) * 16 * 12,
			"sglm bc6h layout");
		Check(a.WriteFile(tmp.string(), err), "sglm bc6h write");
		SgLightmapAtlas d;
		Check(SgLightmapAtlas::ReadFile(tmp.string(), d, err),
			"sglm bc6h read");
		Check(d.compression == SgLightmapCompression::Bc6HOpt
			&& d.bc6hLobes == 12
			&& d.bc6hBlocks == a.bc6hBlocks
			&& d.width == a.width && d.height == a.height,
			"sglm bc6h blocks equal");
		// Reference decode: the read-side payloads must equal a direct
		// block decode (no re-fitting, no re-encoding).
		const size_t blockBytes = size_t(a.width / 4) * (a.height / 4) * 16;
		bool reconExact = true;
		for (uint32_t lobe = 0; lobe < 12 && reconExact; ++lobe) {
			std::vector<uint16_t> dec;
			Bc6hDecodeImage(a.bc6hBlocks.data() + size_t(lobe) * blockBytes,
				a.width, a.height, dec);
			for (size_t ci = 0; ci < a.chunks.size() && reconExact; ++ci) {
				const auto& c = a.chunks[ci];
				if (lobe >= c.lobeCount)
					continue;
				const auto& pref = a.payloads[ci];
				const auto& pdec = d.payloads[ci];
				const int ox = int(c.rectMin[0]);
				const int oy = int(c.rectMin[1]);
				for (uint32_t y = 0; y < pref.height && reconExact; ++y)
					for (uint32_t x = 0; x < pref.width; ++x) {
						const glm::vec3 ref(
							UnpackHalf(dec[(size_t(oy + int(y)) * a.width
								+ size_t(ox + x)) * 3 + 0]),
							UnpackHalf(dec[(size_t(oy + int(y)) * a.width
								+ size_t(ox + x)) * 3 + 1]),
							UnpackHalf(dec[(size_t(oy + int(y)) * a.width
								+ size_t(ox + x)) * 3 + 2]));
						reconExact =
							pdec.coeffs[(size_t(y) * pref.width + size_t(x))
								* c.lobeCount + lobe] == ref;
					}
			}
		}
		Check(reconExact, "sglm bc6h payload recon");
		// Coarse content check: the decode tracks the fixture (mean
		// relative error well under 50% despite the noisy data).
		double errSum = 0.0;
		size_t errN = 0;
		for (size_t ci = 0; ci < a.chunks.size(); ++ci) {
			const auto& pa = a.payloads[ci];
			const auto& pd = d.payloads[ci];
			for (size_t i = 0; i < pa.coeffs.size(); ++i) {
				const float scale = std::max(
					glm::length(pa.coeffs[i]), 0.05f);
				errSum += double(glm::length(pd.coeffs[i] - pa.coeffs[i])
					/ scale);
				++errN;
			}
		}
		Check(errN > 0 && errSum / double(errN) < 0.5,
			"sglm bc6h payload close");
	}
}

// BC6H codec: encode/decode round trip through the vendored DirectXTex
// block codec. Constant blocks decode EXACTLY (zero included); smooth
// gradients (comparable channel scales, like real SG lobe amplitudes)
// stay within a small relative band; the full half range round-trips
// without inf/nan garbage.
void TestBc6hCodec() {
	using namespace smvk::gi;
	// 8x8: left half = smooth gradient, right half = constant zero
	// (the uncovered-separator case: min == max == 0 is lossless).
	std::vector<uint16_t> halves(8 * 8 * 3);
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x) {
			uint16_t* dst = &halves[(size_t(y) * 8 + x) * 3];
			if (x >= 4) {
				dst[0] = dst[1] = dst[2] = 0;
				continue;
			}
			const float t = float(x) / 4.0f + float(y) * 0.05f;
			dst[0] = PackHalf(1.0f * t + 0.5f);
			dst[1] = PackHalf(0.9f * t + 0.6f);
			dst[2] = PackHalf(0.8f * t + 0.4f);
		}
	std::vector<uint8_t> blocks;
	Bc6hEncodeImage(halves.data(), 8, 8, blocks);
	Check(blocks.size() == 2 * 2 * 16, "bc6h.size");
	std::vector<uint16_t> back;
	Bc6hDecodeImage(blocks.data(), 8, 8, back);
	bool zeroExact = true;
	bool gradClose = true;
	double errSum = 0.0;
	size_t errN = 0;
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x) {
			const uint16_t* src = &halves[(size_t(y) * 8 + x) * 3];
			const uint16_t* dec = &back[(size_t(y) * 8 + x) * 3];
			for (uint32_t c = 0; c < 3; ++c) {
				const float s = UnpackHalf(src[c]);
				const float v = UnpackHalf(dec[c]);
				if (x >= 4) {
					zeroExact = zeroExact && v == 0.0f;
				} else {
					const float scale = std::max(s, 0.1f);
					errSum += std::abs(v - s) / scale;
					++errN;
					gradClose = gradClose
						&& std::abs(v - s) < 0.12f * scale;
				}
			}
		}
	Check(zeroExact, "bc6h.zero.exact");
	Check(gradClose || errSum / double(std::max<size_t>(errN, 1)) < 0.04,
		"bc6h.gradient.close");

	// Range probe: dark corner + 65504 max in one image — every decode
	// must stay finite and non-negative (unsigned BC6H domain).
	{
		std::vector<uint16_t> ext(8 * 8 * 3, PackHalf(65504.0f));
		for (uint32_t i = 0; i < 8; ++i)
			ext[size_t(i) * 3 + 0] = PackHalf(0.0f);
		std::vector<uint8_t> eb;
		Bc6hEncodeImage(ext.data(), 8, 8, eb);
		std::vector<uint16_t> ed;
		Bc6hDecodeImage(eb.data(), 8, 8, ed);
		bool finite = true;
		for (uint16_t h : ed) {
			const float v = UnpackHalf(h);
			finite = finite && std::isfinite(v) && v >= 0.0f;
		}
		Check(finite, "bc6h.range.finite");
	}
}

void TestGiWeaveTransmission() {
	// The weave-transmissive cloth (GiSceneBuildOptions::maskTransmissive
	// paths - the MASK cutout cloth): a quad at z=0 carrying a weave
	// material (albedo 0.5 + the textured transmission) over a 2x2
	// baseColor texture - red fabric at texel (0,1), a cutout HOLE (alpha
	// 0) at texel (1,0). Rays through the FABRIC pass tinted per channel
	// by texel * transTexMul; rays through the HOLE pass unobstructed.
	// The sun stays off (sunIntensity 0) so the expected values are
	// exactly the sky terms.
	GiTraceScene scene;
	// Quad corners (x,y in [-1,1], z=0), uv corners (0..1): the hit at
	// (x,y) samples uv = ((x+1)/2, (y+1)/2). Texel-center samples:
	// (0.5,-0.5) -> uv (0.75,0.25) = texel (1,0) [inside tri 0];
	// (-0.5,0.5) -> uv (0.25,0.75) = texel (0,1) [inside tri 1].
	scene.positions = {
		{ -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 },
	};
	scene.indices = { 0, 1, 2, 0, 2, 3 };
	scene.uvs = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	GiTraceMaterial cloth{};
	cloth.albedo = glm::vec3(0.5f);
	cloth.weave = true; // the EXPLICIT marker (an albedo inference breaks
	                    // on the metallic-1 sponza cloth: albedo exactly 0)
	// The intern shape (MASK + maskTransmissive): the flat transmission
	// = diffuse * kMaskWeaveTransmittance; the textured slots carry the
	// SAME texture id with transTexMul = diffuse * k (the texel replaces
	// the average). The test sets them directly (the intern itself is the
	// cook path; this pins the TRACE semantics).
	cloth.transmission = glm::vec3(kMaskWeaveTransmittance);
	cloth.transTexId = 0;
	cloth.transTexMul = glm::vec3(kMaskWeaveTransmittance);
	cloth.transAlphaCutoff = 0.5f;
	scene.materials = { cloth };
	scene.triMaterial = { 0, 0 };
	GiTraceTexelData tex;
	tex.width = 2;
	tex.height = 2;
	tex.texels = {
		glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), // (0,0): green fabric
		glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), // (1,0): cutout HOLE
		glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), // (0,1): red fabric
		glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), // (1,1): blue fabric
	};
	scene.textures.emplace(0, std::move(tex));
	scene.sunIntensity = 0.0f; // no sun: the expectations are pure sky
	scene.skyIntensity = 6000.0f;
	scene.Finalize();

	Pcg32 rng(11);
	// maxDepth 1: the pass-through consumes the first depth slot (it IS a
	// vertex interaction), the sky miss needs one more iteration.
	// Through the RED fabric (hit (-0.5,0.5) -> uv (0.25,0.75)): the sky
	// tints per channel by texel(1,0,0) * k - only red survives.
	{
		const glm::vec3 dir(0, 0, -1);
		const glm::vec3 L = scene.TraceIncoming(
			glm::vec3(-0.5f, 0.5f, 1.0f), dir, rng, 1);
		const glm::vec3 expect = scene.SkyRadiance(dir)
			* glm::vec3(kMaskWeaveTransmittance, 0.0f, 0.0f);
		Check(glm::length(L - expect)
			/ std::max(glm::length(expect), 1e-5f) < 1e-4f,
			"gi weave fabric tints sky per channel");
	}
	// Through the HOLE (hit (0.5,-0.5) -> uv (0.75,0.25), alpha 0 <
	// cutoff 0.5): the ray passes UNOBSTRUCTED - the exact sky radiance.
	{
		const glm::vec3 dir(0, 0, -1);
		const glm::vec3 L = scene.TraceIncoming(
			glm::vec3(0.5f, -0.5f, 1.0f), dir, rng, 1);
		const glm::vec3 expect = scene.SkyRadiance(dir);
		Check(glm::length(L - expect)
			/ std::max(glm::length(expect), 1e-5f) < 1e-4f,
			"gi weave hole passes unobstructed");
	}
	// TransmissionAt directly. Tri 0 = corners (0,1,2): the point
	// (0.5,-0.5) has barycentric (v=0.5, w=0.25) -> uv (0.75,0.25) (the
	// hole). Tri 1 = corners (0,2,3): the point (-0.5,0.5) has (v=0.25,
	// w=0.5) -> uv (0.25,0.75) (the red fabric).
	{
		const glm::vec3 hole = scene.TransmissionAt(cloth, 0, glm::vec2(0.5f, 0.25f));
		Check(glm::length(hole - glm::vec3(1.0f)) < 1e-4f,
			"gi weave TransmissionAt hole texel");
	}
	{
		const glm::vec3 fab = scene.TransmissionAt(cloth, 1, glm::vec2(0.25f, 0.5f));
		Check(glm::length(fab
			- glm::vec3(kMaskWeaveTransmittance, 0.0f, 0.0f)) < 1e-4f,
			"gi weave TransmissionAt fabric texel");
	}
	// The glass convention stays: a flat-transmission material (no
	// transTexId) multiplies by the flat averaged transmission, and its
	// albedo 0 means NO vertex shading - the pure pass-through.
	{
		GiTraceMaterial glass{};
		glass.transmission = glm::vec3(0.4f, 0.5f, 0.6f);
		glass.albedo = glm::vec3(0.0f);
		const glm::vec3 t = scene.TransmissionAt(glass, 0, glm::vec2(0.5f, 0.25f));
		Check(glm::length(t - glass.transmission) < 1e-6f,
			"gi glass flat transmission unchanged");
	}
}

// ---------------------------------------------------------------------------
// The observed-reach sweep (RecordCaptureReach) — zero-direction axes +
// boundary crossings (the invalidation model's traversal). The old
// traversal divided the boundary-crossing parameter by the RAW direction
// component: with a capture positioned EXACTLY on a cell boundary and a
// ray whose component on another axis is EXACTLY zero, the parameter
// computed 0/0 = NaN — and NaN never wins a < comparison, so EVERY axis
// comparison collapsed and the walk died after one cell (skewed or
// truncated reach sets: captures whose hash missed whole corridors).
// ---------------------------------------------------------------------------
void TestCaptureReachTraversal() {
	using CellIdArr = std::array<int32_t, 3>;
	// The discriminator: dirCount = 1 produces the ray
	// dir = (1, 0, 0) EXACTLY (z = 1 - 2*(0.5)/1 = 0, r = 1, phi = 0 ->
	// y-component r*sin(0) = 0). From a capture at (0, 1, 0) — x on the
	// 0-cell boundary, y on the 1-cell boundary — an EMPTY scene (no hit,
	// tHit = 2000): the sweep must record the +X corridor (x = 0..~2000,
	// y = 1, z = 0 fixed). The OLD code died after one cell here.
	{
		GiTraceScene scene = MakeUniformScene(0.7f, glm::vec3(0, 1, 0),
			10000.0f, {}, {});
		const std::vector<CellId> cells =
			RecordCaptureReach(scene, glm::vec3(0.0f, 1.0f, 0.0f), 1);
		bool corridor = cells.size() > 1000;
		for (const CellId& c : cells)
			if (c[1] != 1 || c[2] != 0 || c[0] < 0)
				corridor = false;
		Check(corridor, "gi.reach.zero-axis-ray.full-corridor (old NaN"
			" traversal died after 1 cell)");
	}
	// Boundary crossings: the same +X ray from x = 0 records BOTH the
	// start cell (0) and the immediate boundary-step neighbor (1) —
	// the crossing at t = 0 must advance, not wedge the walk.
	{
		GiTraceScene scene = MakeUniformScene(0.7f, glm::vec3(0, 1, 0),
			10000.0f, {}, {});
		const std::vector<CellId> cells =
			RecordCaptureReach(scene, glm::vec3(0.0f, 1.0f, 0.0f), 1);
		bool hasStart = false, hasNeighbor = false;
		for (const CellId& c : cells) {
			if (c == CellIdArr{ 0, 1, 0 })
				hasStart = true;
			if (c == CellIdArr{ 1, 1, 0 })
				hasNeighbor = true;
		}
		Check(hasStart && hasNeighbor,
			"gi.reach.boundary-crossing.steps-into-neighbor");
	}
	// A hit-rich scene (the open box): sorted-unique contract + the start
	// cell + the hit ring (the bounce neighborhood) present + determinism
	// (the same sweep must observe the same cells twice — the bake and
	// the pak gate replay the same tour and must agree bit-for-bit).
	{
		std::vector<glm::vec3> pos;
		std::vector<uint32_t> idx;
		auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
			const uint32_t b0 = uint32_t(pos.size());
			pos.push_back(a); pos.push_back(b); pos.push_back(c); pos.push_back(d);
			idx.push_back(b0 + 0); idx.push_back(b0 + 1); idx.push_back(b0 + 2);
			idx.push_back(b0 + 0); idx.push_back(b0 + 2); idx.push_back(b0 + 3);
		};
		quad({ -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 });
		quad({ -5, 0, -5 }, { -5, 3, -5 }, { -5, 3, 5 }, { -5, 0, 5 });
		quad({ 5, 0, -5 }, { 5, 0, 5 }, { 5, 3, 5 }, { 5, 3, -5 });
		quad({ -5, 0, -5 }, { 5, 0, -5 }, { 5, 3, -5 }, { -5, 3, -5 });
		quad({ -5, 0, 5 }, { -5, 3, 5 }, { 5, 3, 5 }, { 5, 0, 5 });
		GiTraceScene scene = MakeUniformScene(0.7f, glm::vec3(0, 1, 0),
			10000.0f, pos, idx);
		const std::vector<CellId> a =
			RecordCaptureReach(scene, glm::vec3(0.0f, 1.0f, 0.0f), 64);
		const std::vector<CellId> b =
			RecordCaptureReach(scene, glm::vec3(0.0f, 1.0f, 0.0f), 64);
		Check(a == b, "gi.reach.deterministic");
		bool sortedUnique =
			std::is_sorted(a.begin(), a.end())
			&& std::adjacent_find(a.begin(), a.end()) == a.end();
		Check(sortedUnique, "gi.reach.sorted-unique");
		bool hasStart = false;
		for (const CellId& c : a)
			if (c == CellIdArr{ 0, 1, 0 })
				hasStart = true;
		Check(hasStart, "gi.reach.contains-start-cell");
		// All cells finite (a NaN-driven axis runaway would blow past the
		// scene): the box spans [-5,5]x[0,3]x[-5,5]; the hit ring adds 1
		// cell + the escape rays run to 2000 — bound at [-2001, 2001].
		bool sane = !a.empty();
		for (const CellId& c : a)
			for (int32_t v : c)
				if (v < -2001 || v > 2001)
					sane = false;
		Check(sane, "gi.reach.cells-bounded");
	}
}

} // namespace smvk::gi

int main() {
	smvk::gi::TestSgBasis();
	smvk::gi::TestSgSelfEnergy();
	smvk::gi::TestSgFitRoundtrip();
	smvk::gi::TestSgFitHighContrast();
	smvk::gi::TestGiEmptyScene();
	smvk::gi::TestGiPrimarySunIncluded();
	smvk::gi::TestGiEnergyBound();
	smvk::gi::TestGiWeaveTransmission();
	smvk::gi::TestSgDiffuseAgainstMonteCarlo();
	smvk::gi::TestLmSelectorDomain();
	smvk::gi::TestSgSpecular();
	smvk::gi::TestTriangleBvh();
	smvk::gi::TestRegularVolume();
	smvk::gi::TestGiVolume();
	smvk::gi::TestFp16Pack();
	smvk::gi::TestLightmapCurvatureScale();
	smvk::gi::TestLmXatlasCharts();
	smvk::gi::TestLmDeferRasterize();
	smvk::gi::TestLmRepackDisjoint();
	smvk::gi::TestLmSeamStitch();
	smvk::gi::TestLmChartTransfer();
	smvk::gi::TestStaticBakeLights();
	smvk::gi::TestGiMalformedScene();
	smvk::gi::TestLightmapAtlasRoundtrip();
	smvk::gi::TestBc6hCodec();
	smvk::gi::TestCaptureReachTraversal();

	std::cout << "gi_tests: " << (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
