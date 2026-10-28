// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Spherical Gaussians for baked global illumination (GI probes).
//
// Design:
//  - Radiance probes store a fixed global basis of N spherical-Gaussian
//    lobes; only the RGB amplitudes vary per probe. Because lobe axes and
//    sharpness are GLOBAL constants (not per-probe data), neighboring
//    probes can be blended/interpolated safely: interpolation operates on
//    amplitudes only, and lobe correspondence is trivially preserved.
//    This is the precomputed-GI representation of "Precomputed Global
//    Illumination in The Order: 1886" (Hill/McAuley): their dynamic-object
//    probes were 9 fixed SG lobes with spatially varying RGB amplitudes.
//
//  - 9 lobes at sharpness 4 (FWHM ~ 68 deg) capture the broad directional
//    structure of indirect radiance: diffuse irradiance plus rough-glossy
//    reflections. Mirror reflections are left to the analytic sky env.
//
//  - Storage per probe (the historical sparse-octree form): 9 RGB
//    amplitudes + 6-axis visibility distances + position/support
//    radius, packed to 96 B on the GPU (SgProbeGpu, half-float
//    amplitudes). That runtime is RETIRED with the .sggi octree
//    (probe_volume.h deleted — stage 3 of the octree retirement); the
//    live consumers store lobe amps in dense grids/volumes instead.
//
// CPU/HLSL parity: kSgBasis / kSgSharpness must match
// engine/shaders/sggi.hlsli exactly (same numbers, same order). The .hlsli
// carries a mirror table; changing either side requires changing both.

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

namespace smvk::gi {

// ---------------------------------------------------------------------------
// Fixed SG basis
// ---------------------------------------------------------------------------

inline constexpr uint32_t kSgLobeCount = 9;

// Lobe sharpness lambda (SG convention: lobe(omega) = a * exp(l * (dot(mu,
// omega) - 1)), peak value == a at omega == mu).
inline constexpr float kSgSharpness = 4.0f;

// Nine well-spread axes from a golden-angle (Fibonacci) sphere spiral, Y up.
// MUST match kSgBasis in engine/shaders/sggi.hlsli.
inline constexpr float kSgBasis[kSgLobeCount][3] = {
	{ 0.45812285f, 0.88888889f, 0.00000000f },
	{ -0.54960231f, 0.66666667f, 0.50348074f },
	{ 0.07831653f, 0.44444444f, -0.89237641f },
	{ 0.59322548f, 0.22222222f, 0.77375759f },
	{ -0.98471349f, 0.00000000f, -0.17418195f },
	{ 0.82265807f, -0.22222222f, -0.52330773f },
	{ -0.23255520f, -0.44444444f, 0.86509376f },
	{ -0.34353981f, -0.66666667f, -0.66146500f },
	{ 0.43032455f, -0.88888889f, 0.15715383f },
};

// The clamped-cosine product kernel is PRECOMPUTED for the fixed lobe
// sharpness: K(t) = int lobe(mu) (w . n)+ dw with t = dot(mu, n) is a fixed
// 1D function, numerically integrated and fitted with a degree-6 Chebyshev
// polynomial (worst 0.33% relative error; unit tested against Monte Carlo).
// MUST match SG_GI_COS_CHEB in engine/shaders/sggi.hlsli.
inline constexpr uint32_t kSgChebTermCount = 7;
inline constexpr float kSgCosCheb[kSgChebTermCount] = {
	0.4445942889f,
	0.5893779824f,
	0.1573046510f,
	0.0000000000f,
	-0.0062341036f,
	0.0000000000f,
	0.0002682013f,
};

// ---------------------------------------------------------------------------
// fp16 packing (probe payloads store amplitudes/visibility as halves)
// ---------------------------------------------------------------------------

inline uint16_t PackHalf(float v) { return glm::packHalf1x16(v); }
inline float UnpackHalf(uint16_t v) { return glm::unpackHalf1x16(v); }

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

// Lobe basis value for direction omega (unit amplitude).
inline float SgBasisValue(uint32_t lobe, const glm::vec3& dir) {
	return std::exp(kSgSharpness
		* (glm::dot(glm::vec3(kSgBasis[lobe][0], kSgBasis[lobe][1], kSgBasis[lobe][2]), dir) - 1.0f));
}

// Full radiance field reconstruction: L(omega) = sum_i a_i * lobe_i(omega).
glm::vec3 SgEvaluate(const glm::vec3* amps, const glm::vec3& dir);

// Closed-form product integral of a unit-amplitude basis lobe with a second
// SG (otherAmp, otherSharp, otherAxis):
//   int lobe_i(omega) * SG_other(omega) domega.
// The product of two SGs is another SG: sharpness |l*mu_i + ls*mu_s|, and
// the sphere integral of an SG is a*(2*pi/l)*(1-e^-2l).
float SgKernelProduct(uint32_t lobe, float otherAmp, float otherSharp, const glm::vec3& otherAxis);

// Diffuse irradiance E(n) = int L(omega) (n . omega)+ domega, approximated
// with the SG cosine-lobe fit. Returns HDR irradiance (radiance units).
glm::vec3 SgDiffuseIrradiance(const glm::vec3* amps, const glm::vec3& n);

// Rough GGX specular response to the SG field: the BRDF kernel is
// approximated as a normalized SG around the reflection vector r with
// sharpness ~ 2 / roughness^4, and each lobe integrates against it
// analytically (the same SG x SG product). Caller supplies Fresnel.
// Radiance units; degrades to a mirror tap as roughness -> 0.
glm::vec3 SgSpecular(const glm::vec3* amps, const glm::vec3& r, float roughness);

// ---------------------------------------------------------------------------
// Fitting (fixed basis, diagonal SG projection — Ready-at-Dawn style)
// ---------------------------------------------------------------------------

// Self-overlap of one unit-amplitude basis lobe:
//   int G_i(w)^2 dw = pi / lambda * (1 - exp(-4 lambda)).
// For lambda = 4 this is ~0.785398. Analytic, so the diagonal projection
// needs no empirical Gram diagonal and no per-probe normalization.
inline float SgSelfEnergy() {
	constexpr float kPiLocal = 3.14159265358979323846f;
	return kPiLocal / kSgSharpness
		* (1.0f - std::exp(-4.0f * kSgSharpness));
}

// Online accumulation for the fixed-basis fit. Radiance samples enter
// with their solid-angle weight (uniform sphere sampling:
// weight = 4*pi / sampleCount).
//
// Production path is the DIAGONAL projection (Ready-at-Dawn style):
//   rhs[i] += L * G_i(dir) * weight;  amp[i] = rhs[i] / selfEnergy.
// Because physical radiance and basis lobes are both non-negative, the
// projected amplitudes are naturally non-negative — no NNLS, no Gram,
// no Cholesky, no repair needed. Blurrier than least-squares (SG lobes
// overlap, so the projection double-counts shared energy), but stable:
// the old coupled solver was the blow-up class (near-singular Gram +
// high-contrast sun pools -> 1e4-scale spikes).
//
// The Gram matrix is still accumulated for the legacy NNLS path
// (SolveNnls, tests/A-B only); the production bake never reads it.
struct SgFitAccumulator {
	float gram[kSgLobeCount][kSgLobeCount] = {};
	glm::vec3 rhs[kSgLobeCount] = {};
	uint32_t sampleCount = 0;
	// Production bakes that only ever Solve() (diagonal projection) can
	// skip the Gram accumulation (45 mults/sample): the cubemap-
	// reprojection bake gathers thousands of texel samples per probe
	// and never reads the Gram.
	bool accumulateGram = true;

	void AddSample(const glm::vec3& dir, const glm::vec3& radiance, float weight) {
		// Quarantine: one non-finite radiance sample (NaN shading normal,
		// Inf throughput, ...) would poison rhs -> the whole probe's
		// amplitudes. Drop it.
		if (!std::isfinite(radiance.x + radiance.y + radiance.z))
			return;
		float b[kSgLobeCount];
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			b[i] = SgBasisValue(i, dir);
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			rhs[i] += radiance * (b[i] * weight);
			if (accumulateGram)
				for (uint32_t k = i; k < kSgLobeCount; ++k)
					gram[i][k] += b[i] * b[k] * weight;
		}
		++sampleCount;
	}

	// Production solve: unconditional diagonal projection, clamped to
	// [0, 65504] (fp16 ceiling) per channel.
	void Solve(glm::vec3 outAmps[kSgLobeCount]) const;

	// Legacy path: non-negative least squares by projected coordinate
	// descent (min 1/2 a'G a - rhs'a s.t. a >= 0, per channel).
	// Kept for tests and A/B comparison only.
	void SolveNnls(glm::vec3 outAmps[kSgLobeCount]) const;
};

// Offline convenience for tests: fit amplitudes to a batch of radiance
// samples (uniform solid-angle sampling assumed; weight = 4*pi/count).
void SgFitLobes(std::span<const glm::vec3> dirs, std::span<const glm::vec3> radiance,
	glm::vec3 outAmps[kSgLobeCount]);

// ---------------------------------------------------------------------------
// GPU probe payload (96 bytes; mirrors SgProbeGpu in sggi.hlsli)
// ---------------------------------------------------------------------------

// Rows (uint4 granularity for block layout):
//   posRadius : float4 = xyz position, w support radius (partition weight)
//   amp[16]   : 27 halves, stream = lobe-major RGB (h = lobe*3 + channel;
//               word h >> 1, half h & 1). 5 halves spare.
//   vis[3]    : 6 halves = +x,-x,+y,-y,+z,-z obstacle distances.
//   flags     : bit0 = valid, bits 8..15 = level index.
struct alignas(16) SgProbeGpu {
	glm::vec4 posRadius = {};
	uint32_t amp[16] = {};
	uint32_t vis[3] = {};
	uint32_t flags = 0;
};
static_assert(sizeof(SgProbeGpu) == 96);

inline constexpr uint32_t kSgProbeFlagValid = 0x1u;

inline void SgProbePackAmps(SgProbeGpu& out, const glm::vec3* amps) {
	for (uint32_t lobe = 0; lobe < kSgLobeCount; ++lobe) {
		const uint32_t h = lobe * 3;
		const uint16_t r = PackHalf(amps[lobe].r);
		const uint16_t g = PackHalf(amps[lobe].g);
		const uint16_t b = PackHalf(amps[lobe].b);
		// little-endian half stream: word k holds halves 2k (low) / 2k+1 (high)
		out.amp[h >> 1] |= uint32_t(r) << ((h & 1) * 16);
		out.amp[(h + 1) >> 1] |= uint32_t(g) << (((h + 1) & 1) * 16);
		out.amp[(h + 2) >> 1] |= uint32_t(b) << (((h + 2) & 1) * 16);
	}
}

inline void SgProbePackVis(SgProbeGpu& out, const float vis[6]) {
	for (uint32_t k = 0; k < 6; ++k) {
		const uint16_t h = PackHalf(vis[k]);
		if (k & 1)
			out.vis[k >> 1] |= uint32_t(h) << 16;
		else
			out.vis[k >> 1] |= uint32_t(h);
	}
}

inline glm::vec3 SgProbeUnpackAmp(const SgProbeGpu& p, uint32_t lobe) {
	const uint32_t h = lobe * 3;
	auto halfAt = [&](uint32_t idx) {
		return UnpackHalf(uint16_t(p.amp[idx >> 1] >> ((idx & 1) * 16)));
	};
	return glm::vec3(halfAt(h), halfAt(h + 1), halfAt(h + 2));
}

inline float SgProbeUnpackVis(const SgProbeGpu& p, uint32_t k) {
	return UnpackHalf(uint16_t(p.vis[k >> 1] >> ((k & 1) * 16)));
}

} // namespace smvk::gi
