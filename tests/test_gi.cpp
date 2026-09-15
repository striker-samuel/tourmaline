// gi_tests: CPU-side regression tests for the baked SG-GI probe system
// (Engine/GI): fixed-basis SG math (evaluation, closed-form product
// integrals, non-negative fitting), the triangle BVH used by the baker,
// and the .sggi serialization + level-grid sampling round-trip. No GPU
// required — the runtime sampling path is asserted against the CPU
// reference used to validate the bake (probe_volume.cpp mirrors sggi.hlsli).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/GI/bake.h>
#include <Engine/GI/lightmap.h>
#include <Engine/GI/probe_volume.h>
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
	bool near = true;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		if (std::abs(acc.gram[i][i] - self) / self > 0.10f)
			near = false;
	Check(near, "sg gram diagonal matches self energy (<10%)");
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

void TestProbeVolumeRoundtrip() {
	SgGiVolume vol;
	vol.boundsMin = glm::vec3(-2.0f);
	vol.boundsMax = glm::vec3(2.0f);
	vol.levelCount = 2;
	vol.cellSize[0] = 2.0f;
	vol.cellSize[1] = 1.0f;
	vol.probes.resize(3);
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		vol.probes[0].amps[i] = glm::vec3(0.5f + 0.02f * i);
		vol.probes[1].amps[i] = glm::vec3(2.0f);
		vol.probes[2].amps[i] = glm::vec3(0.25f);
	}
	vol.probes[0].position = glm::vec3(0.0f, 0.5f, 0.0f);
	vol.probes[0].radius = 0.87f;
	vol.probes[0].level = 0;
	vol.probes[0].visibility[0] = 0.5f; // +x obstacle at 0.5 m
	vol.probes[1].position = glm::vec3(2.5f, 0.5f, 0.0f);
	vol.probes[1].radius = 0.87f;
	vol.probes[1].level = 1;
	vol.probes[2].position = glm::vec3(-0.6f, 0.5f, 0.0f);
	vol.probes[2].radius = 0.87f;
	vol.probes[2].level = 0;
	vol.probes[2].visibility[0] = 0.1f;
	vol.BuildGrids();

	// Sampling near probe 0 returns its amplitudes.
	glm::vec3 amps[kSgLobeCount];
	Check(vol.Sample(glm::vec3(0.05f, 0.5f, 0.0f), amps),
		"volume sample covered");
	float maxDiff = 0.0f;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		maxDiff = std::max(maxDiff, glm::length(amps[i] - vol.probes[0].amps[i]));
	Check(maxDiff < 1e-3f, "volume sample returns probe amps");

	// Wall-awareness: probe 0 recorded a +x obstacle at 0.5 m.
	// (a) A point AT the obstacle surface survives (the slack term: the
	//     surface we shade lives at the recorded distance).
	glm::vec3 ampsSurf[kSgLobeCount];
	Check(vol.Sample(glm::vec3(0.52f, 0.5f, 0.0f), ampsSurf),
		"volume visibility keeps on-surface points");
	float surfDiff = 0.0f;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		surfDiff = std::max(surfDiff, glm::length(ampsSurf[i] - vol.probes[0].amps[i]));
	Check(surfDiff < 0.05f, "volume on-surface point blends probe 0");
	// (b) A point beyond the wall AND outside every probe sphere is
	// rejected outright (no distance fallback either — nothing covers).
	glm::vec3 amps2[kSgLobeCount];
	Check(!vol.Sample(glm::vec3(0.95f, 0.5f, 0.0f), amps2),
		"volume visibility rejects behind-wall probe");

	// File round-trip through the production reader.
	const auto tmp = std::filesystem::temp_directory_path()
		/ "vulkantoy_gi_test.sggi";
	std::string err;
	Check(vol.WriteFile(tmp.string(), err), "sggi write");
	SgGiVolume reloaded;
	Check(SgGiVolume::ReadFile(tmp.string(), reloaded, err), "sggi read");
	bool same = reloaded.probes.size() == vol.probes.size()
		&& reloaded.levelCount == vol.levelCount;
	if (same) {
		for (size_t i = 0; i < vol.probes.size(); ++i) {
			same = same
				&& glm::length(reloaded.probes[i].position - vol.probes[i].position) < 1e-5f
				&& std::abs(reloaded.probes[i].radius - vol.probes[i].radius) < 1e-4f
				&& reloaded.probes[i].level == vol.probes[i].level
				&& glm::length(reloaded.probes[i].amps[3] - vol.probes[i].amps[3]) < 1e-3f
				&& std::abs(reloaded.probes[i].visibility[0] - vol.probes[i].visibility[0]) < 1e-3f;
		}
	}
	Check(same, "sggi roundtrip equal");
	// Per-level probe counts must survive the round trip (regression: the
	// level walk in BuildGrids used to leave only the first non-empty
	// level populated -> runtime sampling found no probes).
	bool levelStatsOk = reloaded.levelStats.size() == 2
		&& reloaded.levelStats[0].probeCount == 2
		&& reloaded.levelStats[1].probeCount == 1;
	Check(levelStatsOk, "sggi per-level probe counts");
	// Sampling covers points near both levels' probes.
	glm::vec3 rAmps[kSgLobeCount];
	bool covered1 = reloaded.Sample(glm::vec3(0.05f, 0.5f, 0.0f), rAmps);
	bool covered2 = reloaded.Sample(glm::vec3(2.45f, 0.5f, 0.0f), rAmps);
	Check(covered1 && covered2, "sggi reloaded sampling covers both levels");
	std::filesystem::remove(tmp);

	// GPU packing: probe payload decodes back to the source values.
	std::vector<SgProbeGpu> gpu;
	SgGiGridGpu grid{};
	std::vector<uint32_t> cells, indices;
	reloaded.PackGpu(gpu, grid, cells, indices);
	Check(gpu.size() == reloaded.probes.size(), "gpu pack count");
	if (!gpu.empty()) {
		const SgProbeGpu& g0 = gpu[0];
		const SgProbe& p0 = reloaded.probes[0];
		bool packOk = glm::length(glm::vec3(g0.posRadius) - p0.position) < 1e-5f
			&& std::abs(g0.posRadius.w - p0.radius) < 1e-5f
			&& (g0.flags & kSgProbeFlagValid) != 0;
		for (uint32_t l = 0; l < kSgLobeCount && packOk; ++l)
			packOk = glm::length(SgProbeUnpackAmp(g0, l) - p0.amps[l]) < 1e-3f;
		for (uint32_t k = 0; k < 6 && packOk; ++k)
			packOk = std::abs(SgProbeUnpackVis(g0, k) - p0.visibility[k]) < 1e-3f;
		Check(packOk, "gpu probe pack/unpack roundtrip");
	}
	Check(grid.levelCount == reloaded.levelCount && grid.volumeMin_probeCount.w > 0.0f,
		"gpu grid header");
}

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
		/ "vulkantoy_gi_regular_test.sgrv";
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

} // namespace smvk::gi

int main() {
	smvk::gi::TestSgBasis();
	smvk::gi::TestSgSelfEnergy();
	smvk::gi::TestSgFitRoundtrip();
	smvk::gi::TestSgFitHighContrast();
	smvk::gi::TestGiEmptyScene();
	smvk::gi::TestGiPrimarySunIncluded();
	smvk::gi::TestGiEnergyBound();
	smvk::gi::TestSgDiffuseAgainstMonteCarlo();
	smvk::gi::TestLmSelectorDomain();
	smvk::gi::TestSgSpecular();
	smvk::gi::TestTriangleBvh();
	smvk::gi::TestProbeVolumeRoundtrip();
	smvk::gi::TestRegularVolume();
	smvk::gi::TestFp16Pack();
	smvk::gi::TestLightmapCurvatureScale();

	std::cout << "gi_tests: " << (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
