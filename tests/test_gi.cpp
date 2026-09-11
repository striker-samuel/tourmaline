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

#include <Engine/GI/probe_volume.h>
#include <Engine/GI/sg.h>
#include <Engine/GI/triangle_bvh.h>

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
	// A field that is exactly in the basis span: the LSQ + non-negative
	// refinement must recover the amplitudes.
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
	glm::vec3 fit[kSgLobeCount];
	SgFitLobes(dirs, radiance, fit);
	float maxErr = 0.0f;
	float maxAmp = 0.0f;
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		maxErr = std::max(maxErr, std::abs(fit[i].r - truth[i].r));
		maxErr = std::max(maxErr, std::abs(fit[i].g - truth[i].g));
		maxErr = std::max(maxErr, std::abs(fit[i].b - truth[i].b));
		maxAmp = std::max(maxAmp, truth[i].g);
	}
	Check(maxErr < 0.02f * maxAmp + 1e-3f, "sg fit recovers span field");

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

	// Constant radiance field: E(n) must be exactly pi * c.
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
		SgFitLobes(dirs, radiance, fittedConst);
	}
	const glm::vec3 Econst = SgDiffuseIrradiance(fittedConst, glm::vec3(0, 1, 0));
	const glm::vec3 expect(kPi);
	const float relErr = std::abs(Luminance(Econst) - Luminance(expect)) / kPi;
	Check(relErr < 0.10f, "sg constant-field irradiance == pi*c (<10%)");
}

void TestSgSpecular() {
	// Constant field (fitted): the normalized GGX reflection kernel must
	// return ~the constant regardless of roughness (K integrates to 1).
	glm::vec3 constAmps[kSgLobeCount];
	{
		std::mt19937 rng(41);
		std::vector<glm::vec3> dirs, radiance;
		for (int s = 0; s < 512; ++s) {
			dirs.push_back(UniformDir(rng));
			radiance.push_back(glm::vec3(0.7f));
		}
		SgFitLobes(dirs, radiance, constAmps);
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

} // namespace smvk::gi

int main() {
	smvk::gi::TestSgBasis();
	smvk::gi::TestSgFitRoundtrip();
	smvk::gi::TestSgDiffuseAgainstMonteCarlo();
	smvk::gi::TestSgSpecular();
	smvk::gi::TestTriangleBvh();
	smvk::gi::TestProbeVolumeRoundtrip();
	smvk::gi::TestFp16Pack();

	std::cout << "gi_tests: " << (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
