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

// Cholesky factorization + solve for the SPD Gram matrix (9x9, in-place).
// Returns false when the matrix is not positive definite (caller falls
// back to zero amplitudes).
bool CholeskyFactor(float a[kSgLobeCount][kSgLobeCount]) {
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		for (uint32_t j = 0; j <= i; ++j) {
			float sum = a[i][j];
			for (uint32_t k = 0; k < j; ++k)
				sum -= a[i][k] * a[j][k];
			if (i == j) {
				if (sum <= 1e-12f)
					return false;
				a[i][i] = std::sqrt(sum);
			} else {
				a[i][j] = sum / a[j][j];
				a[j][i] = a[i][j]; // keep full symmetric copy for back-subst
			}
		}
	}
	return true;
}

void CholeskySolve(const float fac[kSgLobeCount][kSgLobeCount],
	const glm::vec3 rhs[kSgLobeCount], glm::vec3 out[kSgLobeCount]) {
	glm::vec3 y[kSgLobeCount];
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		glm::vec3 sum = rhs[i];
		for (uint32_t k = 0; k < i; ++k)
			sum -= fac[i][k] * y[k];
		y[i] = sum / fac[i][i];
	}
	for (int i = kSgLobeCount - 1; i >= 0; --i) {
		glm::vec3 sum = y[i];
		for (uint32_t k = i + 1; k < kSgLobeCount; ++k)
			sum -= fac[k][i] * out[k];
		out[i] = sum / fac[i][i];
	}
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

	// Damped normal equations. The lobe kernels overlap heavily, so the
	// Gram matrix can be near-singular for sparse/biased sample sets; a
	// damping LADDER keeps the solve stable while staying unbiased for
	// well-sampled fields (the first level that factorizes wins).
	glm::vec3 a[kSgLobeCount];
	float C[kSgLobeCount][kSgLobeCount];
	bool solved = false;
	for (int level = 0; level < 5 && !solved; ++level) {
		float trace = 0.0f;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			trace += gram[i][i];
		const float damping = std::max(trace, 1e-6f)
			* (std::pow(10.0f, float(level) - 6.0f) / float(kSgLobeCount));
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			for (uint32_t k = 0; k < kSgLobeCount; ++k) {
				float v = (k >= i) ? gram[i][k] : gram[k][i];
				C[i][k] = v;
			}
			C[i][i] += damping;
		}
		if (!CholeskyFactor(C))
			continue;
		CholeskySolve(C, rhs, a);
		solved = true;
	}
	if (!solved)
		return;

	// Non-negativity: clamp, then multiplicative NMF-style updates
	// a <- a * d / (C a) which recover fit energy that clamping lost
	// while keeping every amplitude >= 0.
	bool clamped = false;
	for (uint32_t i = 0; i < kSgLobeCount; ++i) {
		if (glm::any(glm::lessThan(a[i], glm::vec3(0.0f))))
			clamped = true;
		a[i] = glm::max(a[i], glm::vec3(0.0f));
	}
	if (clamped) {
		for (uint32_t iter = 0; iter < 32; ++iter) {
			glm::vec3 Ca[kSgLobeCount];
			for (uint32_t i = 0; i < kSgLobeCount; ++i) {
				glm::vec3 sum(0.0f);
				for (uint32_t k = 0; k < kSgLobeCount; ++k)
					sum += C[i][k] * a[k];
				Ca[i] = sum;
			}
			for (uint32_t i = 0; i < kSgLobeCount; ++i) {
				const glm::vec3 ratio = rhs[i] / glm::max(Ca[i], glm::vec3(1e-12f));
				// Damped multiplicative step avoids oscillation on noisy fits.
				const glm::vec3 step = glm::pow(glm::max(ratio, glm::vec3(1e-4f)), glm::vec3(0.5f));
				a[i] *= step;
			}
		}
	}

	constexpr float kFp16Max = 65504.0f;
	for (uint32_t i = 0; i < kSgLobeCount; ++i)
		outAmps[i] = glm::clamp(a[i], glm::vec3(0.0f), glm::vec3(kFp16Max));
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
