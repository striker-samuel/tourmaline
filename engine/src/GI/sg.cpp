#include <Engine/GI/sg.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace smvk::gi {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Sphere integral of SG(a, l, mu): a * (2*pi/l) * (1 - e^(-2l)).
inline float SgIntegral(float a, float l) {
	if (l < 1e-6f)
		return a * 4.0f * kPi;
	return a * (2.0f * kPi / l) * (1.0f - std::exp(-2.0f * l));
}

} // namespace

glm::vec3 SgEvaluate(const glm::vec3* amps, const glm::vec3& dir) {
	glm::vec3 sum(0.0f);
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		sum += amps[i] * SgBasisValue(i, dir);
	return sum;
}

float SgKernelProduct(uint32_t lobe, float otherAmp, float otherSharp, const glm::vec3& otherAxis) {
	const glm::vec3 mu(kSgBasis[lobe][0], kSgBasis[lobe][1], kSgBasis[lobe][2]);
	const glm::vec3 sum = kSgSharpness * mu + otherSharp * otherAxis;
	const float lPlus = glm::length(sum);
	if (lPlus < 1e-6f) {
		// Degenerate opposite axes: product is ~a * exp(-2*l)-flat; the
		// integral limit is a * 4*pi * e^-(l + ls) for the mutual tails.
		return otherAmp * 4.0f * kPi * std::exp(-(kSgSharpness + otherSharp));
	}
	const glm::vec3 muPlus = sum / lPlus;
	const float amp = otherAmp * std::exp(lPlus - kSgSharpness - otherSharp);
	return SgIntegral(amp, lPlus);
}

// Clamped-cosine kernel K(t) for a unit basis lobe (see kSgCosCheb):
// degree-6 Chebyshev fit (Clenshaw), tail clamped to >= 0. The true kernel
// is smooth and strictly positive on [-1, 1] (fat lobe tails), so no
// cutoff is needed. MUST mirror SgGiCosKernel in sggi.hlsli.
namespace {

inline float SgCosKernel(float t) {
	t = std::clamp(t, -1.0f, 1.0f);
	float b1 = 0.0f;
	float b2 = 0.0f;
	for (int k = static_cast<int>(kSgChebTermCount) - 1; k >= 1; --k) {
		const float next = 2.0f * t * b1 - b2 + kSgCosCheb[k];
		b2 = b1;
		b1 = next;
	}
	const float kernel = kSgCosCheb[0] + t * b1 - b2;
	return std::max(kernel, 0.0f);
}

} // namespace

glm::vec3 SgDiffuseIrradiance(const glm::vec3* amps, const glm::vec3& n) {
	glm::vec3 sum(0.0f);
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		const glm::vec3 mu(kSgBasis[i][0], kSgBasis[i][1], kSgBasis[i][2]);
		sum += amps[i] * SgCosKernel(glm::dot(mu, n));
	}
	return sum;
}

glm::vec3 SgSpecular(const glm::vec3* amps, const glm::vec3& r, float roughness) {
	// GGX reflection kernel K(w) = D(h(w)) / 4 (view assumed along the
	// normal, the standard ambient-specular simplification): normalized
	// (integral == 1 for a constant environment), centered on r. The GGX
	// NDF is Gaussian-ish in tan(H-angle) with sigma = alpha (alpha =
	// roughness^2, the shader's GGX convention); reflection doubles the
	// angle, so the omega-space lobe has tan-sigma 2*alpha and the SG
	// sharpness is 1/(4*alpha^2). Calibrated against GGX importance
	// sampling in the unit tests.
	const float a = std::clamp(roughness, 0.04f, 1.0f);
	const float alpha = a * a;
	const float lSharp = 1.0f / (4.0f * alpha * alpha + 1e-4f);
	const float norm = lSharp / (2.0f * kPi * (1.0f - std::exp(-2.0f * lSharp)));
	glm::vec3 sum(0.0f);
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		sum += amps[i] * SgKernelProduct(i, norm, lSharp, r);
	return sum;
}

void SgFitAccumulator::Solve(glm::vec3 outAmps[kSgLobeCount]) const {
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		outAmps[i] = glm::vec3(0.0f);
	if (sampleCount == 0)
		return;

	// Non-negative least squares by projected coordinate descent on the
	// objective  min 1/2 a'G a - rhs'a  s.t. a >= 0  (per channel; the
	// objective separates, so channels solve independently and hues can
	// never split by construction). This replaces the old damped-Cholesky
	// + clamp + multiplicative-NMF chain, which had a fatal data-flow
	// bug: CholeskyFactor() rewrote the normal matrix in place, and the
	// NMF refinement then multiplied by the FACTOR instead of Gram. With
	// negative factor off-diagonals, Ca could hit <= 0, the ratio blew up
	// through the 1e-12 floor, and amplitudes exploded to the fp16 clamp
	// (65504) — manufactured white dots. Coordinate descent never forms
	// an inverse, needs no damping, and each update is a clipped ratio:
	// nothing can explode. 48 sweeps over 9 lobes x 3 channels is ~12k
	// flops per probe — negligible next to tracing one ray.
	float normal[kSgLobeCount][kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		for (uint32_t k = 0; k < kSgLobeCount; ++k)
			normal[i][k] = (k >= i) ? gram[i][k] : gram[k][i];
	glm::vec3 a[kSgLobeCount];
	for (int channel = 0; channel < 3; ++channel) {
		float x[kSgLobeCount] = {};
		// Stable positive initial guess: independent per-lobe means.
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			x[i] = std::max(rhs[i][channel] / std::max(normal[i][i], 1e-12f), 0.0f);
		for (uint32_t sweep = 0; sweep < 48; ++sweep) {
			float maxChange = 0.0f;
			float maxAbs = 0.0f;
			for (uint32_t i = 0; i < kSgLobeCount; ++i) {
				float r = rhs[i][channel];
				for (uint32_t j = 0; j < kSgLobeCount; ++j)
					if (j != i)
						r -= normal[i][j] * x[j];
				const float next = std::max(r / std::max(normal[i][i], 1e-12f), 0.0f);
				maxChange = std::max(maxChange, std::abs(next - x[i]));
				maxAbs = std::max(maxAbs, std::abs(next));
				x[i] = next;
			}
			if (maxChange < 1e-6f * std::max(maxAbs, 1.0f))
				break;
		}
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			a[i][channel] = x[i];
	}

	constexpr float kFp16Max = 65504.0f;
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		// Belt-and-suspenders: any non-finite amplitude (should be
		// impossible after AddSample quarantine) becomes zero, never NaN
		// on disk — NaN probes render as black blobs and subdivide forever.
		glm::vec3 v = glm::clamp(a[i], glm::vec3(0.0f), glm::vec3(kFp16Max));
		if (!std::isfinite(v.x + v.y + v.z))
			v = glm::vec3(0.0f);
		outAmps[i] = v;
	}
}

void SgFitLobes(std::span<const glm::vec3> dirs, std::span<const glm::vec3> radiance,
	glm::vec3 outAmps[kSgLobeCount]) {
	const size_t n = std::min(dirs.size(), radiance.size());
	SgFitAccumulator acc;
	const float weight = (n > 0) ? (4.0f * kPi / float(n)) : 0.0f;
	for (size_t s = 0; s < n; ++s)
		acc.AddSample(dirs[s], radiance[s], weight);
	acc.Solve(outAmps);
}

} // namespace smvk::gi
