#include <Engine/GI/lightmap.h>

// NOTE: pool.h must precede Rendergraph/environment.h (bake.h pulls it).
#include <Engine/Memory/pool.h>
#include <Engine/GI/bake.h>
#include <Engine/Renderer/mesh.h> // StaticMeshUpload (charts API)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <unordered_map>

#include <Engine/Asset/compression.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/platform.h>

#include <xatlas/xatlas.h>

namespace smvk::gi {

namespace {

constexpr float kPi = 3.14159265358979323846f;

constexpr uint32_t kSglmVersion = 2;

// v2 payload segment slice: each slice compresses/decompresses through
// the blob codec independently (kMaxBlobBytes cap). 16M-texel atlases
// pack a ~1.2 GB section — over the cap for a single LZ4 block.
constexpr uint32_t kSglmSegmentBytes = 256u * 1024u * 1024u;

struct SglmFileHeader {
	char magic[4] = { 'S', 'G', 'L', 'M' };
	uint32_t version = kSglmVersion;
	uint32_t atlasWidth = 0;
	uint32_t atlasHeight = 0;
	uint32_t chunkCount = 0;
	uint32_t payloadBytes = 0; // uncompressed post-header section
	// 0 = section stored raw, 1 = single LZ4 block (v1 files),
	// 2 = segmented LZ4 (v2: u32 segCount + (u64 raw, u64 comp) table +
	// segCount blocks, slices of kSglmSegmentBytes raw bytes).
	uint32_t compressed = 0;
};

inline float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

// ---------------------------------------------------------------------------
// Fixed bases (BakingLab GenerateUniformSGs, converted to engine Y-up):
// Fibonacci-spiral axes + sharpness s = log(0.65)*N / (minDP - 1.0001).
// Z-up -> Y-up is the rotation (x, -z, y), which preserves the spread.
// ---------------------------------------------------------------------------

struct SgBasisTable {
	uint32_t lobeCount;
	float sharpness;
	glm::vec3 axes[kLightmapMaxLobes];
};

constexpr SgBasisTable kBases[4] = {
	{ 5, 2.272914f, {
		glm::vec3( 0.60000000f,  0.80000000f,  0.00000000f),
		glm::vec3(-0.67580974f,  0.40000000f,  0.61909708f),
		glm::vec3( 0.08742572f,  0.00000000f, -0.99617104f),
		glm::vec3( 0.55764343f, -0.40000000f,  0.72734710f),
		glm::vec3(-0.59082809f, -0.80000000f, -0.10450917f),
	} },
	{ 6, 3.385227f, {
		glm::vec3( 0.55277080f,  0.83333333f,  0.00000000f),
		glm::vec3(-0.63858018f,  0.50000000f,  0.58499175f),
		glm::vec3( 0.08620293f,  0.16666667f, -0.98223789f),
		glm::vec3( 0.59992881f, -0.16666667f,  0.78250089f),
		glm::vec3(-0.85278689f, -0.50000000f, -0.15084599f),
		glm::vec3( 0.46640329f, -0.83333333f,  0.29668759f),
	} },
	{ 9, 5.996703f, {
		glm::vec3( 0.45812285f,  0.88888889f,  0.00000000f),
		glm::vec3(-0.54960231f,  0.66666667f,  0.50348074f),
		glm::vec3( 0.07831653f,  0.44444444f, -0.89237641f),
		glm::vec3( 0.59322548f,  0.22222222f,  0.77375759f),
		glm::vec3(-0.98471349f,  0.00000000f, -0.17418195f),
		glm::vec3( 0.82265807f, -0.22222222f, -0.52330773f),
		glm::vec3(-0.23255520f, -0.44444444f,  0.86509376f),
		glm::vec3(-0.34353981f, -0.66666667f, -0.66146500f),
		glm::vec3( 0.43032455f, -0.88888889f,  0.15715383f),
	} },
	{ 12, 7.253499f, {
		glm::vec3( 0.39965263f,  0.91666667f,  0.00000000f),
		glm::vec3(-0.48772367f,  0.75000000f,  0.44679483f),
		glm::vec3( 0.07101005f,  0.58333333f, -0.80912286f),
		glm::vec3( 0.55310703f,  0.41666667f,  0.72143018f),
		glm::vec3(-0.95344473f,  0.25000000f, -0.16865095f),
		glm::vec3( 0.84082048f,  0.08333333f, -0.53486117f),
		glm::vec3(-0.25870133f, -0.08333333f,  0.96235606f),
		glm::vec3(-0.44627131f, -0.25000000f, -0.85926825f),
		glm::vec3( 0.85389880f, -0.41666667f,  0.31184247f),
		glm::vec3(-0.75078384f, -0.58333333f,  0.30991265f),
		glm::vec3( 0.28034777f, -0.75000000f, -0.59908691f),
		glm::vec3( 0.11960958f, -0.91666667f,  0.38133420f),
	} },
};

const SgBasisTable* BasisFor(uint32_t lobeCount) {
	for (const SgBasisTable& t : kBases)
		if (t.lobeCount == lobeCount)
			return &t;
	return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Basis access
// ---------------------------------------------------------------------------

float LightmapSharpness(uint32_t lobeCount) {
	const SgBasisTable* t = BasisFor(lobeCount);
	return t ? t->sharpness : 0.0f;
}

glm::vec3 LightmapAxis(uint32_t lobeCount, uint32_t lobe) {
	const SgBasisTable* t = BasisFor(lobeCount);
	if (!t || lobe >= t->lobeCount)
		return glm::vec3(0.0f, 1.0f, 0.0f);
	return t->axes[lobe];
}

// ---------------------------------------------------------------------------
// Evaluation (CPU mirrors of sg_lightmap.hlsli)
// ---------------------------------------------------------------------------

glm::vec3 SgLightmapIrradianceFitted(const glm::vec3& amp, const glm::vec3& axis,
	float sharpness, const glm::vec3& n) {
	// Stephen Hill's fitted clamped-cosine approximation (BakingLab
	// SGIrradianceFitted, MIT). Fixed sharpness per lobe count.
	const float muDotN = glm::dot(axis, n);
	const float lambda = std::max(sharpness, 1e-4f);
	const float c0 = 0.36f;
	const float c1 = 1.0f / (4.0f * c0);
	const float eml = std::exp(-lambda);
	const float em2l = eml * eml;
	const float rl = 1.0f / lambda;
	const float scale = 1.0f + 2.0f * em2l - rl;
	const float bias = (eml - em2l) * rl - em2l;
	const float x = SafeSqrt(std::max(0.0f, 1.0f - scale));
	const float x0 = c0 * muDotN;
	const float x1 = c1 * x;
	const float nr = x0 + x1;
	const float y = (std::abs(x0) <= x1) ? nr * nr / x : std::clamp(muDotN, 0.0f, 1.0f);
	const float normIrr = scale * y + bias;
	// ApproximateSGIntegral = 2*pi*amp/lambda
	return normIrr * (2.0f * kPi * amp / lambda);
}

namespace {

// GGX visibility term evaluated at the warped lobe center (BakingLab
// SpecularTermSGWarp uses GGX_V1, the Smith height-correlated V1 approx).
inline float GgxV1(float m2, float nDotX) {
	return 2.0f * nDotX
		/ (nDotX + SafeSqrt(m2 + (1.0f - m2) * nDotX * nDotX));
}

} // namespace

glm::vec3 SgLightmapSpecularLobe(const glm::vec3& amp, const glm::vec3& axis,
	float sharpness, const glm::vec3& n, const glm::vec3& view,
	float roughness, const glm::vec3& specAlbedo) {
	// SG-warped NDF approximation (BakingLab SpecularTermSGWarp core).
	const float m = std::clamp(roughness, 0.045f, 1.0f);
	const float m2 = m * m;
	// DistributionTermSG: NDF as an SG around n, sharpness 2/m^2,
	// amplitude 1/(pi*m^2).
	const float ndfSharp = 2.0f / m2;
	const float ndfAmp = 1.0f / (kPi * m2);
	// WarpDistributionSG: rotate the NDF SG into the lighting domain
	// (half-angle -> reflection), amplitude preserved, sharpness scaled.
	const float nDotV = std::max(glm::dot(n, view), 0.0f);
	glm::vec3 warpAxis = glm::reflect(-view, n);
	const float warpLen = glm::length(warpAxis);
	warpAxis = warpLen > 1e-6f ? warpAxis / warpLen : n;
	const float warpSharp = ndfSharp / (4.0f * std::max(nDotV, 0.1f));
	// SGInnerProduct(warpedNDF-as-SG, lobe): both SGs, closed form.
	// warpedNDF SG = (amp=ndfAmp, axis=warpAxis, sharp=warpSharp).
	const glm::vec3 sum = warpSharp * warpAxis + sharpness * axis;
	const float lp = glm::length(sum);
	if (lp < 1e-6f)
		return glm::vec3(0.0f);
	const float expo = std::exp(lp - warpSharp - sharpness);
	const float other = 1.0f - std::exp(-2.0f * lp);
	const glm::vec3 inner = ndfAmp * amp * expo * (2.0f * kPi * other / lp);
	// Visibility + Fresnel + cosine at the warped lobe center.
	const float nDotL = std::clamp(glm::dot(n, warpAxis), 0.0f, 1.0f);
	glm::vec3 h = warpAxis + view;
	const float hLen = glm::length(h);
	h = hLen > 1e-6f ? h / hLen : n;
	glm::vec3 out = inner;
	out *= GgxV1(m2, nDotL) * GgxV1(m2, nDotV);
	out *= specAlbedo + (glm::vec3(1.0f) - specAlbedo)
		* std::pow(1.0f - std::clamp(glm::dot(warpAxis, h), 0.0f, 1.0f), 5.0f);
	// Fade out spec below 0.1% albedo (BakingLab), then the cosine term.
	out *= std::clamp(Luminance(specAlbedo) * 333.0f, 0.0f, 1.0f);
	out *= nDotL;
	return glm::max(out, glm::vec3(0.0f));
}

glm::vec3 SgLightmapDiffuse(const glm::vec3* amps, uint32_t lobeCount,
	const glm::vec3& n) {
	const SgBasisTable* t = BasisFor(lobeCount);
	if (!t)
		return glm::vec3(0.0f);
	glm::vec3 sum(0.0f);
	for (uint32_t i = 0; i < t->lobeCount; ++i)
		sum += SgLightmapIrradianceFitted(amps[i], t->axes[i], t->sharpness, n);
	return sum;
}

glm::vec3 SgLightmapSpecular(const glm::vec3* amps, uint32_t lobeCount,
	const glm::vec3& n, const glm::vec3& view, float roughness,
	const glm::vec3& specAlbedo) {
	const SgBasisTable* t = BasisFor(lobeCount);
	if (!t)
		return glm::vec3(0.0f);
	glm::vec3 sum(0.0f);
	for (uint32_t i = 0; i < t->lobeCount; ++i)
		sum += SgLightmapSpecularLobe(amps[i], t->axes[i], t->sharpness,
			n, view, roughness, specAlbedo);
	return sum;
}

// ---------------------------------------------------------------------------
// Fitting
// ---------------------------------------------------------------------------

SgLightmapFitAccumulator::SgLightmapFitAccumulator(uint32_t lobeCount)
	: lobeCount_(lobeCount) {
	const SgBasisTable* t = BasisFor(lobeCount);
	sharpness_ = t ? t->sharpness : 4.0f;
}

void SgLightmapFitAccumulator::AddSample(const glm::vec3& dir,
	const glm::vec3& radiance, float weight) {
	if (!std::isfinite(radiance.x + radiance.y + radiance.z))
		return;
	const SgBasisTable* t = BasisFor(lobeCount_);
	if (!t)
		return;
	for (uint32_t i = 0; i < lobeCount_; ++i) {
		const float b = std::exp(t->sharpness
			* (glm::dot(t->axes[i], dir) - 1.0f));
		rhs_[i] += radiance * (b * weight);
	}
	++sampleCount_;
}

// Diagonal projection solve from raw rhs (stable, but double-counts
// shared lobe energy — flat/ambient fields overshoot; kept as the warm
// start of the production solve below and for non-lightmap callers).
static void SolveRhs(const glm::vec3* rhs, uint32_t lobeCount,
	glm::vec3* outAmps) {
	const SgBasisTable* t = BasisFor(lobeCount);
	if (!t) {
		for (uint32_t i = 0; i < lobeCount; ++i)
			outAmps[i] = glm::vec3(0.0f);
		return;
	}
	// Diagonal projection (production path): amp = rhs / selfEnergy with
	// selfEnergy = pi/lambda * (1 - exp(-4 lambda)) — analytic, same
	// convention as SgSelfEnergy in sg.h.
	const float selfEnergy = kPi / t->sharpness
		* (1.0f - std::exp(-4.0f * t->sharpness));
	const float inv = 1.0f / std::max(selfEnergy, 1e-8f);
	for (uint32_t i = 0; i < lobeCount; ++i) {
		glm::vec3 a = rhs[i] * inv;
		// fp16 ceiling per channel (payload stores halves).
		a = glm::clamp(a, glm::vec3(0.0f), glm::vec3(65504.0f));
		if (!std::isfinite(a.x + a.y + a.z))
			a = glm::vec3(0.0f);
		outAmps[i] = a;
	}
}

void SgLightmapFitAccumulator::Solve(glm::vec3* outAmps) const {
	SolveRhs(rhs_, lobeCount_, outAmps);
}

// ---------------------------------------------------------------------------
// Gram solve: projected coordinate descent (NNLS) on the basis's ANALYTIC
// Gram. The diagonal projection double-counts shared lobe energy, so flat
// fields overshoot 1.6-2.4x depending on the count (measured — the 9-lobe
// constant-white fit lands at 4.95 vs the true pi*1); the historical
// unconstrained coupled solver fixed that but was abandoned for blowing
// up (near-singular Gram + sun pools). Coordinate descent is the middle
// path: warm-started at the bounded diagonal solution, every update is
// the exact bounded coordinate minimizer of the non-negative
// least-squares objective, so the objective decreases MONOTONICALLY and
// no amplitude can spike. Constant fields come out exact at every count
// — the property the per-mesh lobe-count selection measures against.
// ---------------------------------------------------------------------------

// Candidate lobe counts for the per-mesh selection (and the Gram cache
// slots — 5/6/9/12 up to the caller's preferred count).
static constexpr uint32_t kLobeCandidates[4] = { 5u, 6u, 9u, 12u };
static constexpr uint32_t kLobeCandidateSlots = 4;

struct LmGram {
	float g[kLightmapMaxLobes][kLightmapMaxLobes] = {};
	uint32_t n = 0;
};

static const LmGram& GramForCount(uint32_t lobeCount) {
	static LmGram cache[kLobeCandidateSlots];
	static std::once_flag once[kLobeCandidateSlots];
	uint32_t slot = 0;
	switch (lobeCount) {
		case 5: slot = 0; break;
		case 6: slot = 1; break;
		case 9: slot = 2; break;
		default: slot = 3; break;
	}
	std::call_once(once[slot], [&] {
		const SgBasisTable* t = BasisFor(lobeCount);
		LmGram& G = cache[slot];
		G.n = t ? t->lobeCount : 0;
		if (!t)
			return;
		const float lambda = t->sharpness;
		for (uint32_t i = 0; i < G.n; ++i) {
			for (uint32_t j = i; j < G.n; ++j) {
				// Closed-form int K_i K_j domega for two SGs:
				//   exp(-(li+lj) + |li ai + lj aj|) * (2pi/|..|)(1-exp(-2|..|))
				const glm::vec3 c = lambda * t->axes[i]
					+ lambda * t->axes[j];
				const float len = glm::length(c);
				float v;
				if (len < 1e-6f) {
					// Parallel-antiparallel degenerate: int of a constant
					// exp(-2 lambda) over the sphere.
					v = std::exp(-2.0f * lambda) * 4.0f * kPi;
				} else {
					v = std::exp(-2.0f * lambda + len)
						* (2.0f * kPi / len) * (1.0f - std::exp(-2.0f * len));
				}
				G.g[i][j] = v;
				G.g[j][i] = v;
			}
		}
	});
	return cache[slot];
}

// Shared production lightmap solve (rhs -> amplitudes).
static void SolveRhsGram(const glm::vec3* rhs, uint32_t lobeCount,
	glm::vec3* outAmps) {
	const LmGram& G = GramForCount(lobeCount);
	if (G.n == 0 || G.n != lobeCount) {
		for (uint32_t i = 0; i < lobeCount; ++i)
			outAmps[i] = glm::vec3(0.0f);
		return;
	}
	// Warm start: diagonal projection (bounded, stable).
	SolveRhs(rhs, lobeCount, outAmps);
	// Projected coordinate descent, per channel. Each update is the exact
	// minimizer of the NNLS objective along coordinate j (clamped at 0),
	// so the objective decreases monotonically — no spike class. Sweeps
	// are fixed: partial convergence still strictly improves the warm
	// start, and the cost stays deterministic.
	constexpr uint32_t kSweeps = 24;
	for (uint32_t sweep = 0; sweep < kSweeps; ++sweep) {
		for (uint32_t j = 0; j < G.n; ++j) {
			const float gjj = std::max(G.g[j][j], 1e-8f);
			glm::vec3 sum(0.0f);
			for (uint32_t i = 0; i < G.n; ++i)
				sum += G.g[j][i] * outAmps[i];
			const glm::vec3 num = rhs[j] - (sum - G.g[j][j] * outAmps[j]);
			glm::vec3 a = num / gjj;
			a = glm::clamp(a, glm::vec3(0.0f), glm::vec3(65504.0f));
			if (!std::isfinite(a.x + a.y + a.z))
				a = outAmps[j];
			outAmps[j] = a;
		}
	}
}

void SgLightmapFitAccumulator::SolveGram(glm::vec3* outAmps) const {
	SolveRhsGram(rhs_, lobeCount_, outAmps);
}

// ---------------------------------------------------------------------------
// Curvature-driven density scale (see lightmap.h)
// ---------------------------------------------------------------------------
float LightmapCurvatureScale(const StaticMeshUpload& upload,
	float localTexelsPerUnit, float targetDegreesPerTexel,
	float maxScale) {
	const size_t triCount = upload.indices.size() / 3;
	if (triCount < 4 || upload.vertices.empty()
		|| !(localTexelsPerUnit > 0.0f)
		|| !(targetDegreesPerTexel > 0.0f) || !(maxScale >= 1.0f))
		return 1.0f;

	// Face normals (unnormalized cross products carry area for free).
	std::vector<glm::vec3> fn(triCount);
	for (size_t t = 0; t < triCount; ++t) {
		const glm::vec3& a = upload.vertices[upload.indices[t * 3 + 0]].pos;
		const glm::vec3& b = upload.vertices[upload.indices[t * 3 + 1]].pos;
		const glm::vec3& c = upload.vertices[upload.indices[t * 3 + 2]].pos;
		fn[t] = glm::cross(b - a, c - a);
	}

	// Shared-edge dihedral angles -> curvature samples. The bend rate is
	// dihedral / BEND STEP, where the step is the mean height of the two
	// triangles over the edge (distance of the opposite vertices to the
	// edge line) — the edge's OWN length is irrelevant: a cylinder's seam
	// edges run ALONG the bend axis, where dihedral/edge-length measures
	// wall height, not curvature. Only the SMOOTH band counts: under
	// 1 deg is flat, over 45 deg is an authored crease (a hard edge is
	// infinitely curved — density cannot fix it, chart seams own those
	// edges). Samples are weighted by edge length x step (the surface
	// area whose bend the edge describes).
	std::unordered_map<uint64_t, uint32_t> firstTri;
	firstTri.reserve(upload.indices.size());
	std::vector<std::pair<float, float>> smooth; // (area weight, curvature)
	auto heightOver = [&](uint32_t pIdx, const glm::vec3& a, const glm::vec3& b) {
		const glm::vec3 ab = b - a;
		const float l = glm::length(ab);
		if (!(l > 1e-6f))
			return 0.0f;
		return glm::length(glm::cross(upload.vertices[pIdx].pos - a, ab)) / l;
	};
	for (size_t t = 0; t < triCount; ++t) {
		for (uint32_t k = 0; k < 3; ++k) {
			const uint32_t v0 = upload.indices[t * 3 + k];
			const uint32_t v1 = upload.indices[t * 3 + (k + 1) % 3];
			const uint64_t key = uint64_t(std::min(v0, v1)) << 32
				| std::max(v0, v1);
			auto it = firstTri.find(key);
			if (it == firstTri.end()) {
				firstTri.emplace(key, uint32_t(t));
				continue;
			}
			const uint32_t t0 = it->second;
			if (t0 == t)
				continue; // degenerate repeat
			glm::vec3 n0 = fn[t0];
			glm::vec3 n1 = fn[t];
			const float l0 = glm::length(n0);
			const float l1 = glm::length(n1);
			if (!(l0 > 1e-20f) || !(l1 > 1e-20f))
				continue;
			n0 /= l0;
			n1 /= l1;
			const float cosA = glm::clamp(glm::dot(n0, n1), -1.0f, 1.0f);
			const float deg = glm::degrees(std::acos(cosA));
			if (deg <= 1.0f || deg >= 45.0f)
				continue;
			// Opposite vertices of the two triangles (the bend step is
			// the mean of their heights over the edge line).
			uint32_t opp0 = 0xFFFFFFFFu;
			for (uint32_t j = 0; j < 3; ++j) {
				const uint32_t v = upload.indices[t0 * 3 + j];
				if (v != v0 && v != v1) {
					opp0 = v;
					break;
				}
			}
			const uint32_t opp1 = upload.indices[t * 3 + (k + 2) % 3];
			if (opp0 == 0xFFFFFFFFu)
				continue;
			const glm::vec3& e0 = upload.vertices[v0].pos;
			const glm::vec3& e1 = upload.vertices[v1].pos;
			const float step = 0.5f
				* (heightOver(opp0, e0, e1) + heightOver(opp1, e0, e1));
			const float edgeLen = glm::length(e1 - e0);
			if (!(step > 1e-6f) || !(edgeLen > 1e-6f))
				continue;
			const float curv = glm::radians(deg) / step;
			if (!(curv > 0.0f) || !std::isfinite(curv) || curv > 1000.0f)
				continue;
			smooth.push_back({ edgeLen * step, curv });
		}
	}
	if (smooth.empty())
		return 1.0f;

	// Area-weighted 90th percentile: walk from the sharpest curvature
	// down, accumulating the edge-governed area, until 10% is covered —
	// the curvature at that point is the bend rate of the sharpest
	// smoothly-bending tenth of the mesh (robust against stray slivers,
	// responsive to real ornament).
	double totalArea = 0.0;
	for (const auto& s : smooth)
		totalArea += double(s.first);
	std::sort(smooth.begin(), smooth.end(),
		[](const std::pair<float, float>& a, const std::pair<float, float>& b) {
			return a.second > b.second; // sharpest first
		});
	double acc = 0.0;
	float curv = smooth.back().second;
	for (const auto& s : smooth) {
		acc += double(s.first);
		curv = s.second;
		if (acc >= totalArea * 0.10)
			break;
	}

	// Scale = current normal-turn per texel / target.
	const float anglePerTexel = curv / localTexelsPerUnit; // radians
	const float target = glm::radians(targetDegreesPerTexel);
	return std::clamp(anglePerTexel / target, 1.0f, maxScale);
}

// ---------------------------------------------------------------------------
// Charts: greedy planar unwrap, per-vertex UV2 (no geometry replacement)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Authored-UV2 charts (external unwrapper layout in texCoord2)
// ---------------------------------------------------------------------------

// Islands are UV-connected triangle groups of the authored lightmap
// layout (RizomUV-packed glTF TEXCOORD_1): two triangles join across a
// shared canonical-position edge only when BOTH shared endpoints' uv2
// agree — a baked-in UV seam breaks the chain exactly where the layout
// was cut, so the islands reproduce the unwrapper's packing 1:1 (welded
// corners return equal uv2 and merge; packed-apart corners stay apart).
// Rects are island uv2 bboxes; the layout's own uv-to-world scale
// (sqrt(Auv/A3d) over the mesh) maps opts.texelsPerMeter to texels
// across the [0,1] uv range, so rect sizes rescale with the atlas
// budget search without re-unwrapping. No island splitting (no cap): an
// oversized rect simply fails PackRects' atlas width and the search
// lowers the density, unlike the planar path whose growth is capped.
static bool GenerateLightmapChartsAuthored(
	const StaticMeshUpload& upload,
	const SgLightmapChartOptions& opts,
	SgLightmapChartResult& out, std::string& err)
{
	const uint32_t triCount = uint32_t(upload.indices.size() / 3);
	const uint32_t margin = opts.gutterTexels;

	// 3D canonical dedup (same quantization as the planar path: unwelded
	// corner duplicates connect through coincident positions).
	std::unordered_map<uint64_t, uint32_t> posCanonical;
	posCanonical.reserve(upload.vertices.size() * 2);
	std::vector<uint32_t> canonical(upload.vertices.size(), 0);
	for (size_t v = 0; v < upload.vertices.size(); ++v) {
		const glm::vec3& p = upload.vertices[v].pos;
		const int32_t qx = int32_t(std::clamp(p.x * 8192.0f, -1048576.0f, 1048575.0f));
		const int32_t qy = int32_t(std::clamp(p.y * 8192.0f, -1048576.0f, 1048575.0f));
		const int32_t qz = int32_t(std::clamp(p.z * 8192.0f, -1048576.0f, 1048575.0f));
		const uint64_t key2 = uint64_t(uint32_t(qx))
			| (uint64_t(uint32_t(qy)) << 21)
			| (uint64_t(uint32_t(qz)) << 42);
		auto [it, inserted] = posCanonical.insert({ key2, uint32_t(v) });
		canonical[v] = it->second;
	}
	// Per-triangle canonical corners (tri*3 layout, reused by the BFS).
	std::vector<uint32_t> triCanon(size_t(triCount) * 3, 0);
	// Edge -> triangles (canonical ids; degenerate triangles excluded).
	std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
	edges.reserve(size_t(triCount) * 3);
	double area3d = 0.0, areaUv = 0.0;
	for (uint32_t t = 0; t < triCount; ++t) {
		const uint32_t i0 = upload.indices[t * 3 + 0];
		const uint32_t i1 = upload.indices[t * 3 + 1];
		const uint32_t i2 = upload.indices[t * 3 + 2];
		if (i0 == i1 || i1 == i2 || i0 == i2)
			continue; // degenerate: no edges, stays uncharted
		const uint32_t c0 = canonical[i0];
		const uint32_t c1 = canonical[i1];
		const uint32_t c2 = canonical[i2];
		triCanon[t * 3 + 0] = c0;
		triCanon[t * 3 + 1] = c1;
		triCanon[t * 3 + 2] = c2;
		const uint64_t e0 = uint64_t(std::min(c0, c1)) | (uint64_t(std::max(c0, c1)) << 32);
		const uint64_t e1 = uint64_t(std::min(c1, c2)) | (uint64_t(std::max(c1, c2)) << 32);
		const uint64_t e2 = uint64_t(std::min(c2, c0)) | (uint64_t(std::max(c2, c0)) << 32);
		edges[e0].push_back(t);
		edges[e1].push_back(t);
		edges[e2].push_back(t);
		const glm::vec3& p0 = upload.vertices[i0].pos;
		const glm::vec3& p1 = upload.vertices[i1].pos;
		const glm::vec3& p2 = upload.vertices[i2].pos;
		area3d += double(glm::length(glm::cross(p1 - p0, p2 - p0)));
		const glm::vec2& u0 = upload.vertices[i0].texCoord2;
		const glm::vec2& u1 = upload.vertices[i1].texCoord2;
		const glm::vec2& u2 = upload.vertices[i2].texCoord2;
		areaUv += double(std::abs((u1.x - u0.x) * (u2.y - u0.y)
			- (u2.x - u0.x) * (u1.y - u0.y)));
	}
	if (areaUv <= 1e-12) {
		err = "GenerateLightmapCharts: authored uv2 layout has no area";
		return false;
	}
	// Layout scale: uv units per local meter. texelsPerMeter (local
	// texels/m) -> texels across the full [0,1] uv range.
	const double texelsAcrossLayout = std::max(
		double(opts.texelsPerMeter) * std::sqrt(area3d / areaUv), 1.0);

	// Island BFS: uv2 agreement at both shared endpoints (eps: uv units;
	// pack padding separates islands by orders of magnitude more).
	struct Island {
		glm::vec2 pMin{ 0.0f }, pMax{ 0.0f };
		std::vector<uint32_t> tris;
		uint32_t w = 0, h = 0;
	};
	std::vector<Island> islands;
	std::vector<uint32_t> chartOf(triCount, 0xFFFFFFFFu);
	std::vector<uint32_t> frontier;
	if (opts.authoredOneChartPerLayout) {
		// PRE-PACKED layout (RizomUV Pack + normalize): the mesh's whole
		// [0,1]^2 uv2 IS the island packing. One chart per layout — per
		// island rects would re-pack an already-packed layout and pay
		// ~4 gutter texels per side on every island (bistro's rizom set
		// is >1M islands: 37M+ gutter texels, over any atlas budget
		// before content). The synthetic island spans the full range so
		// every downstream step (rect sizing, chart-local uv, texel
		// rasterization) runs unchanged; chart-local uv is the authored
		// uv2 verbatim.
		Island& isl = islands.emplace_back();
		isl.pMin = glm::vec2(0.0f);
		isl.pMax = glm::vec2(1.0f);
		isl.tris.reserve(triCount);
		for (uint32_t t = 0; t < triCount; ++t)
			isl.tris.push_back(t);
	} else {
		// Island BFS: uv2 agreement at both shared endpoints (eps: uv
		// units; pack padding separates islands by orders of magnitude
		// more).
		constexpr float kUvSeamEps = 1e-5f;
		auto cornerUv2 = [&](uint32_t t, uint32_t c) -> const glm::vec2& {
			for (int k = 0; k < 3; ++k)
				if (triCanon[t * 3 + k] == c)
					return upload.vertices[upload.indices[t * 3 + k]].texCoord2;
			return upload.vertices[0].texCoord2; // unreachable
		};
		for (uint32_t seed = 0; seed < triCount; ++seed) {
			if (chartOf[seed] != 0xFFFFFFFFu)
				continue;
			const uint32_t idx = uint32_t(islands.size());
			Island& isl = islands.emplace_back();
			isl.pMin = glm::vec2(std::numeric_limits<float>::max());
			isl.pMax = glm::vec2(std::numeric_limits<float>::lowest());
			frontier.clear();
			frontier.push_back(seed);
			while (!frontier.empty()) {
				const uint32_t t = frontier.back();
				frontier.pop_back();
				if (chartOf[t] != 0xFFFFFFFFu)
					continue;
				chartOf[t] = idx;
				isl.tris.push_back(t);
				for (int k = 0; k < 3; ++k) {
					const glm::vec2& u =
						upload.vertices[upload.indices[t * 3 + k]].texCoord2;
					isl.pMin = glm::min(isl.pMin, u);
					isl.pMax = glm::max(isl.pMax, u);
				}
				const uint32_t ca = triCanon[t * 3 + 0];
				const uint32_t cb = triCanon[t * 3 + 1];
				const uint32_t cc = triCanon[t * 3 + 2];
				const uint64_t eKeys[3] = {
					uint64_t(std::min(ca, cb)) | (uint64_t(std::max(ca, cb)) << 32),
					uint64_t(std::min(cb, cc)) | (uint64_t(std::max(cb, cc)) << 32),
					uint64_t(std::min(cc, ca)) | (uint64_t(std::max(cc, ca)) << 32),
				};
				for (uint64_t e : eKeys)
					for (uint32_t nb : edges[e]) {
						if (nb == t || chartOf[nb] != 0xFFFFFFFFu)
							continue;
						// Shared canonical endpoints of this edge.
						const uint32_t lo = uint32_t(e & 0xFFFFFFFFull);
						const uint32_t hi = uint32_t(e >> 32);
						const glm::vec2& ta = cornerUv2(t, lo);
						const glm::vec2& tb = cornerUv2(t, hi);
						const glm::vec2& na = cornerUv2(nb, lo);
						const glm::vec2& nbv = cornerUv2(nb, hi);
						if (std::abs(ta.x - na.x) + std::abs(ta.y - na.y) > kUvSeamEps)
							continue;
						if (std::abs(tb.x - nbv.x) + std::abs(tb.y - nbv.y) > kUvSeamEps)
							continue;
						frontier.push_back(nb);
					}
			}
		}
	}

	// Island rects: uv bbox at the layout density (+ inner margins).
	uint64_t meshTexelTotal = 0;
	for (Island& isl : islands) {
		const glm::vec2 ext = glm::max(isl.pMax - isl.pMin, glm::vec2(1e-6f));
		isl.w = std::max<uint32_t>(uint32_t(std::ceil(ext.x * float(texelsAcrossLayout))), 1u) + 2u * margin;
		isl.h = std::max<uint32_t>(uint32_t(std::ceil(ext.y * float(texelsAcrossLayout))), 1u) + 2u * margin;
		meshTexelTotal += uint64_t(isl.w) * isl.h;
	}
	if (opts.maxChartTexelsTotal > 0
		&& meshTexelTotal > opts.maxChartTexelsTotal) {
		err = "GenerateLightmapCharts: mesh chart texels exceed budget "
			"(lower --lm-density)";
		return false;
	}

	// Island-local uv: (uv2 - pMin)/ext clamped to [0,1] over the inner
	// rect. Resolution-independent: the rect resizes around the same
	// [0,1] parameterization as the density search scales.
	auto islandUv = [&](const Island& isl, const glm::vec2& u) -> glm::vec2 {
		const glm::vec2 ext = glm::max(isl.pMax - isl.pMin, glm::vec2(1e-6f));
		return glm::clamp((u - isl.pMin) / ext, glm::vec2(0.0f), glm::vec2(1.0f));
	};

	// Per-ORIGINAL-vertex output (first island to touch a vertex claims
	// it — the authored layout makes every duplicated corner carry its
	// own island's uv, so this is exact for all referenced vertices).
	out.charts.resize(islands.size());
	out.vertexChart.assign(upload.vertices.size(), 0xFFFFFFFFu);
	out.chartLocalUv.assign(upload.vertices.size(), glm::vec2(0.0f));
	for (uint32_t ci = 0; ci < uint32_t(islands.size()); ++ci) {
		for (uint32_t t : islands[ci].tris) {
			for (int k = 0; k < 3; ++k) {
				const uint32_t v = upload.indices[t * 3 + k];
				if (out.vertexChart[v] == 0xFFFFFFFFu) {
					out.vertexChart[v] = ci;
					out.chartLocalUv[v] =
						islandUv(islands[ci], upload.vertices[v].texCoord2);
				}
			}
		}
	}
	// Per-triangle output (seam-correct): every corner through its OWN
	// island, original triangle order.
	out.triChart.assign(triCount, 0xFFFFFFFFu);
	out.cornerChartUv.assign(size_t(triCount) * 3, glm::vec2(0.0f));
	for (uint32_t ci = 0; ci < uint32_t(islands.size()); ++ci) {
		for (uint32_t t : islands[ci].tris) {
			out.triChart[t] = ci;
			for (int k = 0; k < 3; ++k) {
				const uint32_t v = upload.indices[t * 3 + k];
				out.cornerChartUv[size_t(t) * 3 + k] =
					islandUv(islands[ci], upload.vertices[v].texCoord2);
			}
		}
	}

	// Bake texels per island: rasterize every island triangle over the
	// inner region, first-writer-owns (mirrors the planar path's claim
	// map so overlapping uv never duplicates texels). uv is rect-local
	// ([margin, size - margin) texels).
	for (uint32_t ci = 0; ci < uint32_t(islands.size()); ++ci) {
		const Island& isl = islands[ci];
		SgLightmapChartResult::Chart& oc = out.charts[ci];
		oc.width = isl.w;
		oc.height = isl.h;
		const uint32_t iw = std::max<uint32_t>(isl.w - 2u * margin, 1u);
		const uint32_t ih = std::max<uint32_t>(isl.h - 2u * margin, 1u);
		std::vector<uint32_t> claim(size_t(iw) * ih, 0xFFFFFFFFu);
		for (uint32_t t : isl.tris) {
			const uint32_t i0 = upload.indices[t * 3 + 0];
			const uint32_t i1 = upload.indices[t * 3 + 1];
			const uint32_t i2 = upload.indices[t * 3 + 2];
			const glm::vec3& p0 = upload.vertices[i0].pos;
			const glm::vec3& p1 = upload.vertices[i1].pos;
			const glm::vec3& p2 = upload.vertices[i2].pos;
			const glm::vec3& n0 = upload.vertices[i0].normal;
			const glm::vec3& n1 = upload.vertices[i1].normal;
			const glm::vec3& n2 = upload.vertices[i2].normal;
			const glm::vec2 u0 = islandUv(isl, upload.vertices[i0].texCoord2);
			const glm::vec2 u1 = islandUv(isl, upload.vertices[i1].texCoord2);
			const glm::vec2 u2 = islandUv(isl, upload.vertices[i2].texCoord2);
			const glm::vec2 e0 = u1 - u0;
			const glm::vec2 e1 = u2 - u0;
			const float det = e0.x * e1.y - e0.y * e1.x;
			if (std::abs(det) < 1e-12f)
				continue; // zero-uv-area sliver: no texels (dilation covers)
			const glm::vec2 tMin = glm::min(u0, glm::min(u1, u2));
			const glm::vec2 tMax = glm::max(u0, glm::max(u1, u2));
			uint32_t tx0 = std::min(uint32_t(std::max(0.0f, tMin.x * float(iw) - 0.5f)), iw - 1u);
			uint32_t tx1 = std::min(uint32_t(std::min(float(iw), tMax.x * float(iw) + 0.5f)), iw - 1u);
			uint32_t ty0 = std::min(uint32_t(std::max(0.0f, tMin.y * float(ih) - 0.5f)), ih - 1u);
			uint32_t ty1 = std::min(uint32_t(std::min(float(ih), tMax.y * float(ih) + 0.5f)), ih - 1u);
			for (uint32_t y = ty0; y <= ty1; ++y) {
				for (uint32_t x = tx0; x <= tx1; ++x) {
					const size_t idx = size_t(y) * iw + x;
					if (claim[idx] != 0xFFFFFFFFu)
						continue;
					const float u = (float(x) + 0.5f) / float(iw);
					const float v = (float(y) + 0.5f) / float(ih);
					const glm::vec2 d(u - u0.x, v - u0.y);
					const float b1 = (d.x * e1.y - d.y * e1.x) / det;
					const float b2 = (e0.x * d.y - e0.y * d.x) / det;
					const float b0 = 1.0f - b1 - b2;
					if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
						continue;
					claim[idx] = 1u;
					SgLightmapTexel tx{};
					tx.position = p0 * b0 + p1 * b1 + p2 * b2;
					tx.normal = glm::normalize(n0 * b0 + n1 * b1 + n2 * b2);
					tx.uv = glm::vec2(float(margin + x) + 0.5f,
						float(margin + y) + 0.5f);
					oc.texels.push_back(tx);
				}
			}
		}
	}
	out.usedAuthoredUv2 = true;
	return true;
}

bool GenerateLightmapCharts(const StaticMeshUpload& upload,
	const SgLightmapChartOptions& opts, SgLightmapChartResult& out,
	std::string& err) {
	out = SgLightmapChartResult{};
	const uint32_t triCount = uint32_t(upload.indices.size() / 3);
	if (triCount == 0) {
		err = "GenerateLightmapCharts: mesh has no triangles";
		return false;
	}
	if (!(opts.texelsPerMeter > 0.0f) || opts.gutterTexels == 0 || opts.gutterTexels > 4) {
		err = "GenerateLightmapCharts: bad options";
		return false;
	}
	const uint32_t margin = opts.gutterTexels; // inner margin per chart

	// Authored-UV2 mode: use the external unwrapper's islands when the
	// mesh carries a non-degenerate texCoord2 layout (any spread at all
	// around the mean); fall through to the planar unwrapper otherwise
	// (all-equal texCoord2 = engine zeros, nothing authored).
	if (opts.authoredUv2) {
		glm::vec2 mean(0.0f);
		for (const StaticVertex& v : upload.vertices)
			mean += v.texCoord2;
		if (!upload.vertices.empty())
			mean /= float(upload.vertices.size());
		bool hasSpread = false;
		for (const StaticVertex& v : upload.vertices) {
			if (std::abs(v.texCoord2.x - mean.x) > 1e-6f
				|| std::abs(v.texCoord2.y - mean.y) > 1e-6f) {
				hasSpread = true;
				break;
			}
		}
		if (hasSpread)
			return GenerateLightmapChartsAuthored(upload, opts, out, err);
	}
	out.usedAuthoredUv2 = false;

	// ------------------------------------------------------------------
	// Greedy planar-chart unwrap (per-triangle bbox charts waste ~2-4x on
	// large/dense meshes; grouped charts waste ~1.2x):
	//   1. Face normal -> dominant axis plane per triangle.
	//   2. BFS region-grow triangles into charts while plane + normal
	//      agree (near-planar charts project coherently).
	//   3. Chart UV = continuous projection over the chart's bbox
	//      (shared edges inside a chart map to the same UV).
	//   4. Shelf-pack chart rects; per-ORIGINAL-vertex UVs out (the
	//      cook fills texCoord2; mesh geometry is untouched).
	// ------------------------------------------------------------------
	struct Plane {
		int ax, ay; // chart axis indices
	};
	auto planeFor = [](const glm::vec3& n, Plane& p) {
		const glm::vec3 a = glm::abs(n);
		if (a.x >= a.y && a.x >= a.z)      p = { 2, 1 }; // X dominant
		else if (a.y >= a.z)               p = { 0, 2 }; // Y dominant
		else                               p = { 0, 1 }; // Z dominant
	};

	// Face normals + planes.
	std::vector<glm::vec3> faceNormal(triCount, glm::vec3(0.0f, 1.0f, 0.0f));
	std::vector<Plane> facePlane(triCount, Plane{ 0, 1 });
	for (uint32_t t = 0; t < triCount; ++t) {
		const uint32_t i0 = upload.indices[t * 3 + 0];
		const uint32_t i1 = upload.indices[t * 3 + 1];
		const uint32_t i2 = upload.indices[t * 3 + 2];
		const glm::vec3& p0 = upload.vertices[i0].pos;
		const glm::vec3& p1 = upload.vertices[i1].pos;
		const glm::vec3& p2 = upload.vertices[i2].pos;
		glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
		const float fnLen = glm::length(fn);
		if (fnLen > 1e-12f) {
			faceNormal[t] = fn / fnLen;
			planeFor(faceNormal[t], facePlane[t]);
		}
	}

	// Position-deduped vertices for adjacency (welded meshes share indices
	// already; unwelded ones still connect through coincident corners).
	std::unordered_map<uint64_t, uint32_t> posCanonical;
	posCanonical.reserve(upload.vertices.size() * 2);
	std::vector<uint32_t> canonical(upload.vertices.size(), 0);
	for (size_t v = 0; v < upload.vertices.size(); ++v) {
		const glm::vec3& p = upload.vertices[v].pos;
		const uint64_t key = uint64_t(int32_t(p.x * 8192.0f))
			| (uint64_t(uint32_t(int32_t(p.y * 8192.0f))) << 21)
			| (uint64_t(uint32_t(int32_t(p.z * 8192.0f))) << 42);
		// 21-bit signed quantization (~1/8192 m grid, +/-256 m range).
		const int32_t qx = int32_t(std::clamp(p.x * 8192.0f, -1048576.0f, 1048575.0f));
		const int32_t qy = int32_t(std::clamp(p.y * 8192.0f, -1048576.0f, 1048575.0f));
		const int32_t qz = int32_t(std::clamp(p.z * 8192.0f, -1048576.0f, 1048575.0f));
		(void)key;
		const uint64_t key2 = uint64_t(uint32_t(qx))
			| (uint64_t(uint32_t(qy)) << 21)
			| (uint64_t(uint32_t(qz)) << 42);
		auto [it, inserted] = posCanonical.insert({ key2, uint32_t(v) });
		canonical[v] = it->second;
	}
	// Edge -> triangle adjacency (undirected, canonical corner ids).
	std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
	edges.reserve(size_t(triCount) * 3);
	for (uint32_t t = 0; t < triCount; ++t) {
		const uint32_t c0 = canonical[upload.indices[t * 3 + 0]];
		const uint32_t c1 = canonical[upload.indices[t * 3 + 1]];
		const uint32_t c2 = canonical[upload.indices[t * 3 + 2]];
		const uint64_t e0 = uint64_t(std::min(c0, c1)) | (uint64_t(std::max(c0, c1)) << 32);
		const uint64_t e1 = uint64_t(std::min(c1, c2)) | (uint64_t(std::max(c1, c2)) << 32);
		const uint64_t e2 = uint64_t(std::min(c2, c0)) | (uint64_t(std::max(c2, c0)) << 32);
		edges[e0].push_back(t);
		edges[e1].push_back(t);
		edges[e2].push_back(t);
	}
	auto neighborsOf = [&](uint32_t t, std::vector<uint32_t>& outN) {
		outN.clear();
		const uint32_t c0 = canonical[upload.indices[t * 3 + 0]];
		const uint32_t c1 = canonical[upload.indices[t * 3 + 1]];
		const uint32_t c2 = canonical[upload.indices[t * 3 + 2]];
		const uint64_t e0 = uint64_t(std::min(c0, c1)) | (uint64_t(std::max(c0, c1)) << 32);
		const uint64_t e1 = uint64_t(std::min(c1, c2)) | (uint64_t(std::max(c1, c2)) << 32);
		const uint64_t e2 = uint64_t(std::min(c2, c0)) | (uint64_t(std::max(c2, c0)) << 32);
		for (uint64_t e : { e0, e1, e2 })
			for (uint32_t nb : edges[e])
				if (nb != t)
					outN.push_back(nb);
	};

	// Chart cap: projected area beyond this splits (keeps charts packable).
	constexpr float kChartSplitNormalDot = 0.80f; // ~37 deg agreement
	constexpr uint64_t kChartMaxTexels = 256u * 256;

	struct Chart {
		Plane plane{ 0, 1 };
		glm::vec3 refNormal{ 0.0f, 1.0f, 0.0f };
		glm::vec2 pMin{ 0.0f };  // projected bbox (meters)
		glm::vec2 pMax{ 0.0f };
		uint64_t texels = 0;
		std::vector<uint32_t> tris;
		uint32_t x0 = 0, y0 = 0, w = 0, h = 0; // shelf placement
	};
	std::vector<Chart> charts;
	std::vector<uint32_t> chartOf(triCount, 0xFFFFFFFFu);
	// Vertex -> owning chart (canonical ids; first claim wins). Chart
	// seams approximate: a vertex shared by two charts maps through its
	// first chart, so the other chart's seam triangle samples this
	// chart's margin — a diluted, mild artifact the bake dilation keeps
	// plausible. NO vertex duplication: the mesh's vertex count and
	// index buffer are untouched.
	std::vector<uint32_t> vertexChartOf(upload.vertices.size(), 0xFFFFFFFFu);
	std::vector<uint32_t> frontier;
	std::vector<uint32_t> nbBuf;
	for (uint32_t seed = 0; seed < triCount; ++seed) {
		if (chartOf[seed] != 0xFFFFFFFFu)
			continue;
		const uint32_t chartIdx = uint32_t(charts.size());
		Chart& ch = charts.emplace_back();
		ch.plane = facePlane[seed];
		ch.refNormal = faceNormal[seed];
		frontier.clear();
		frontier.push_back(seed);
		while (!frontier.empty()) {
			const uint32_t t = frontier.back();
			frontier.pop_back();
			if (chartOf[t] != 0xFFFFFFFFu)
				continue;
			// Plane + normal agreement keeps the shared projection sane.
			if (facePlane[t].ax != ch.plane.ax || facePlane[t].ay != ch.plane.ay)
				continue;
			if (glm::dot(faceNormal[t], ch.refNormal) < kChartSplitNormalDot)
				continue;
			chartOf[t] = chartIdx;
			ch.tris.push_back(t);
			// Claim the triangle's vertices for this chart (first chart
			// to touch a vertex owns its lightmap UV).
			for (int k = 0; k < 3; ++k) {
				const uint32_t c = canonical[upload.indices[t * 3 + k]];
				if (vertexChartOf[c] == 0xFFFFFFFFu)
					vertexChartOf[c] = chartIdx;
			}
			// Extend the projected bbox with this triangle.
			for (int k = 0; k < 3; ++k) {
				const glm::vec3& p =
					upload.vertices[upload.indices[t * 3 + k]].pos;
				const glm::vec2 q(p[ch.plane.ax], p[ch.plane.ay]);
				if (ch.tris.size() == 1 && k == 0) {
					ch.pMin = ch.pMax = q;
				} else {
					ch.pMin = glm::min(ch.pMin, q);
					ch.pMax = glm::max(ch.pMax, q);
				}
			}
			const glm::vec2 ext = glm::max(ch.pMax - ch.pMin, glm::vec2(1e-4f));
			ch.texels = uint64_t(ext.x * float(opts.texelsPerMeter) + 1.0f)
				* uint64_t(ext.y * float(opts.texelsPerMeter) + 1.0f);
			if (ch.texels <= kChartMaxTexels) {
				neighborsOf(t, nbBuf);
				for (uint32_t nb : nbBuf)
					if (chartOf[nb] == 0xFFFFFFFFu)
						frontier.push_back(nb);
			}
			// Over-cap charts stop growing (their bbox already includes
			// the offending triangle: the split is approximate, not
			// exact-area; acceptable for a v1 unwrapper).
		}
	}
	// Chart rect sizes from projected bboxes (+ inner margins). No shelf
	// pack here and NO failure modes: the ATLAS packs chart rects (a mesh
	// owns many small rects), and the chart-size cap keeps every rect
	// small, so no mesh can bust a region budget.
	uint64_t meshTexelTotal = 0;
	for (Chart& ch : charts) {
		const glm::vec2 ext = glm::max(ch.pMax - ch.pMin, glm::vec2(1e-4f));
		ch.w = std::max<uint32_t>(uint32_t(std::ceil(ext.x * float(opts.texelsPerMeter))) + 2u * margin, 2u * margin + 1u);
		ch.h = std::max<uint32_t>(uint32_t(std::ceil(ext.y * float(opts.texelsPerMeter))) + 2u * margin, 2u * margin + 1u);
		meshTexelTotal += uint64_t(ch.w) * ch.h;
	}
	// Per-mesh TOTAL budget: a mesh with thousands of chart rects
	// produces an unbounded bake-point count otherwise (GBs before the
	// atlas rounds ever run). Checked BEFORE rasterization so the
	// failing call costs only the (cheap) topology pass; the cook halves
	// the density and retries.
	if (opts.maxChartTexelsTotal > 0
		&& meshTexelTotal > opts.maxChartTexelsTotal) {
		err = "GenerateLightmapCharts: mesh chart texels exceed budget "
			"(lower --lm-density)";
		return false;
	}

	// Chart-UV for a triangle corner (continuous projection over the
	// chart bbox; clamped to [0,1] over the chart's inner rect).
	auto chartUv = [&](const Chart& c, const glm::vec3& p) -> glm::vec2 {
		const glm::vec2 q(p[c.plane.ax], p[c.plane.ay]);
		const glm::vec2 ext = glm::max(c.pMax - c.pMin, glm::vec2(1e-6f));
		return glm::clamp((q - c.pMin) / ext, glm::vec2(0.0f), glm::vec2(1.0f));
	};

	// Per-ORIGINAL-vertex output: owning chart + chart-local uv (the cook
	// maps these to atlas space AFTER the atlas pack, via AtlasUv).
	// NOTE: first-chart-wins — seam corners shared by two charts get ONE
	// uv here, which is why the per-triangle view below exists.
	out.charts.resize(charts.size());
	out.vertexChart.assign(upload.vertices.size(), 0xFFFFFFFFu);
	out.chartLocalUv.assign(upload.vertices.size(), glm::vec2(0.0f));
	for (size_t v = 0; v < upload.vertices.size(); ++v) {
		const uint32_t chartIdx = vertexChartOf[canonical[v]];
		if (chartIdx == 0xFFFFFFFFu)
			continue; // unclaimed (degenerate tri corner): (0,0), no chart
		const Chart& c = charts[chartIdx];
		out.vertexChart[v] = chartIdx;
		out.chartLocalUv[v] = chartUv(c, upload.vertices[v].pos);
	}

	// Per-triangle output (seam-correct): every triangle's corners in the
	// triangle's OWN chart projection, original triangle order. A seam
	// triangle (corners shared with a neighboring chart) still gets all
	// three corners inside its own chart rect — the cook stamps these
	// per-corner and duplicates seam vertices, so no triangle ever
	// interpolates across two atlas rects.
	out.triChart.assign(triCount, 0xFFFFFFFFu);
	out.cornerChartUv.assign(size_t(triCount) * 3, glm::vec2(0.0f));
	for (uint32_t t = 0; t < triCount; ++t) {
		const uint32_t chartIdx = chartOf[t];
		if (chartIdx == 0xFFFFFFFFu)
			continue; // degenerate (zero-area): keeps (0,0)
		const Chart& c = charts[chartIdx];
		out.triChart[t] = chartIdx;
		for (int k = 0; k < 3; ++k) {
			const glm::vec3& p =
				upload.vertices[upload.indices[t * 3 + k]].pos;
			out.cornerChartUv[size_t(t) * 3 + k] = chartUv(c, p);
		}
	}

	// Bake texels per chart: rasterize every chart triangle over the
	// chart's inner region. Texel ownership: first writer wins (a flat
	// w*h claim map — overlapping triangles of the same chart must NOT
	// push duplicate texels, or big-chart meshes explode quadratically);
	// shared-edge texels carry the first triangle's interpolation
	// (sub-texel difference, invisible after the dilation pass). uv is
	// in the chart's rect coords ([margin, size-margin) texels).
	for (uint32_t ci = 0; ci < charts.size(); ++ci) {
		const Chart& c = charts[ci];
		SgLightmapChartResult::Chart& oc = out.charts[ci];
		oc.width = c.w;
		oc.height = c.h;
		const uint32_t iw = std::max<uint32_t>(c.w - 2u * margin, 1u);
		const uint32_t ih = std::max<uint32_t>(c.h - 2u * margin, 1u);
		std::vector<uint32_t> claim(size_t(iw) * ih, 0xFFFFFFFFu);
		for (uint32_t t : c.tris) {
			const uint32_t i0 = upload.indices[t * 3 + 0];
			const uint32_t i1 = upload.indices[t * 3 + 1];
			const uint32_t i2 = upload.indices[t * 3 + 2];
			const glm::vec3& p0 = upload.vertices[i0].pos;
			const glm::vec3& p1 = upload.vertices[i1].pos;
			const glm::vec3& p2 = upload.vertices[i2].pos;
			const glm::vec3& n0 = upload.vertices[i0].normal;
			const glm::vec3& n1 = upload.vertices[i1].normal;
			const glm::vec3& n2 = upload.vertices[i2].normal;
			const glm::vec2 u0 = chartUv(c, p0);
			const glm::vec2 u1 = chartUv(c, p1);
			const glm::vec2 u2 = chartUv(c, p2);
			const glm::vec2 e0 = u1 - u0;
			const glm::vec2 e1 = u2 - u0;
			const float det = e0.x * e1.y - e0.y * e1.x;
			if (std::abs(det) < 1e-12f)
				continue;
			// Triangle's uv bbox -> texel bounds inside the chart.
			const glm::vec2 tMin = glm::min(u0, glm::min(u1, u2));
			const glm::vec2 tMax = glm::max(u0, glm::max(u1, u2));
			uint32_t tx0 = std::min(uint32_t(std::max(0.0f, tMin.x * float(iw) - 0.5f)), iw - 1u);
			uint32_t tx1 = std::min(uint32_t(std::min(float(iw), tMax.x * float(iw) + 0.5f)), iw - 1u);
			uint32_t ty0 = std::min(uint32_t(std::max(0.0f, tMin.y * float(ih) - 0.5f)), ih - 1u);
			uint32_t ty1 = std::min(uint32_t(std::min(float(ih), tMax.y * float(ih) + 0.5f)), ih - 1u);
			for (uint32_t y = ty0; y <= ty1; ++y) {
				for (uint32_t x = tx0; x <= tx1; ++x) {
					const size_t idx = size_t(y) * iw + x;
					if (claim[idx] != 0xFFFFFFFFu)
						continue; // first writer owns this texel
					const float u = (float(x) + 0.5f) / float(iw);
					const float v = (float(y) + 0.5f) / float(ih);
					const glm::vec2 d(u - u0.x, v - u0.y);
					const float b1 = (d.x * e1.y - d.y * e1.x) / det;
					const float b2 = (e0.x * d.y - e0.y * d.x) / det;
					const float b0 = 1.0f - b1 - b2;
					if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
						continue;
					claim[idx] = 1u;
					SgLightmapTexel tx{};
					tx.position = p0 * b0 + p1 * b1 + p2 * b2;
					tx.normal = glm::normalize(n0 * b0 + n1 * b1 + n2 * b2);
					// Rect-local texel coords + margin offset.
					tx.uv = glm::vec2(float(margin + x) + 0.5f,
						float(margin + y) + 0.5f);
					oc.texels.push_back(tx);
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Xatlas brute-force chart + pack (see lightmap.h)
// ---------------------------------------------------------------------------

namespace {

// Inter-rect separation when stacking xatlas levels into a shared atlas
// (matches PackRects' kPackSep: gutter + sep + gutter island padding).
constexpr uint32_t kXatlasStackSep = 2;

struct XatlasTriKey {
	uint32_t a = 0, b = 0, c = 0;
	bool operator==(const XatlasTriKey& o) const {
		return a == o.a && b == o.b && c == o.c;
	}
};
struct XatlasTriKeyHash {
	size_t operator()(const XatlasTriKey& k) const noexcept {
		// 64-bit FNV-1a over the sorted triple.
		size_t h = 1469598103934665603ull;
		h ^= k.a;
		h *= 1099511628211ull;
		h ^= k.b;
		h *= 1099511628211ull;
		h ^= k.c;
		h *= 1099511628211ull;
		return h;
	}
};

} // namespace

bool GenerateLightmapChartsXatlas(
	const std::vector<SgLightmapXatlasMeshInput>& meshes,
	const SgLightmapXatlasOptions& opts, SgLightmapXatlasLevel& out,
	std::string& err) {
	out = SgLightmapXatlasLevel{};
	if (meshes.empty()) {
		err = "GenerateLightmapChartsXatlas: no meshes";
		return false;
	}
	if (!(opts.texelsPerMeter > 0.0f) || opts.gutterTexels == 0
		|| opts.gutterTexels > 4) {
		err = "GenerateLightmapChartsXatlas: bad options";
		return false;
	}
	const uint32_t margin = opts.gutterTexels;

	// Valid jobs (meshes with triangles); out.meshes stays parallel to the
	// inputs so the caller can index by mesh position (ok=false when empty).
	struct Job {
		size_t inputIdx = 0;
		const StaticMeshUpload* up = nullptr;
		std::vector<glm::vec3> posScaled;
		std::vector<glm::vec3> nrm;
	};
	std::vector<Job> jobs;
	out.meshes.resize(meshes.size());
	for (size_t i = 0; i < meshes.size(); ++i) {
		const SgLightmapXatlasMeshInput& in = meshes[i];
		if (!in.upload || in.upload->indices.size() < 3
			|| in.upload->vertices.empty())
			continue;
		Job j;
		j.inputIdx = i;
		j.up = in.upload;
		const float s = (in.densityScale > 1e-6f) ? in.densityScale : 1.0f;
		j.posScaled.reserve(in.upload->vertices.size());
		j.nrm.reserve(in.upload->vertices.size());
		for (const StaticVertex& v : in.upload->vertices) {
			j.posScaled.push_back(v.pos * s);
			const float l = glm::length(v.normal);
			j.nrm.push_back(l > 1e-6f ? v.normal / l
			                          : glm::vec3(0.0f, 1.0f, 0.0f));
		}
		jobs.push_back(std::move(j));
	}
	if (jobs.empty()) {
		err = "GenerateLightmapChartsXatlas: all meshes empty";
		return false;
	}

	xatlas::Atlas* xa = xatlas::Create();
	for (const Job& j : jobs) {
		xatlas::MeshDecl decl;
		decl.vertexCount = uint32_t(j.posScaled.size());
		decl.vertexPositionData = j.posScaled.data();
		decl.vertexPositionStride = sizeof(glm::vec3);
		decl.vertexNormalData = j.nrm.data();
		decl.vertexNormalStride = sizeof(glm::vec3);
		decl.indexData = j.up->indices.data();
		decl.indexCount = uint32_t(j.up->indices.size());
		decl.indexFormat = xatlas::IndexFormat::UInt32;
		const xatlas::AddMeshError ae =
			xatlas::AddMesh(xa, decl, 1);
		if (ae != xatlas::AddMeshError::Success) {
			xatlas::Destroy(xa);
			err = std::string("GenerateLightmapChartsXatlas: AddMesh failed: ")
				+ xatlas::StringForEnum(ae);
			return false;
		}
	}
	xatlas::ChartOptions co;
	co.fixWinding = true;
	xatlas::PackOptions po;
	po.texelsPerUnit = std::max(opts.texelsPerMeter, 1e-3f);
	// Pack gap: our per-chart margin ring must land in EMPTY pack space on
	// both sides (margin + sep + margin island-to-island, same budget as
	// the shelf packer's gutter + kPackSep + gutter).
	po.padding = margin + kXatlasStackSep;
	po.bilinear = true;
	po.bruteForce = true; // ALWAYS: this path exists for best packing.
	po.blockAlign = false;
	po.rotateCharts = true;
	po.rotateChartsToAxis = true;
	po.resolution = 0; // single atlas sized to content (caller budgets it)
	po.createImage = false;
	xatlas::Generate(xa, co, po);
	if (xa->width == 0 || xa->height == 0 || xa->meshCount == 0) {
		xatlas::Destroy(xa);
		err = "GenerateLightmapChartsXatlas: xatlas produced an empty atlas";
		return false;
	}
	if (xa->meshCount != uint32_t(jobs.size())) {
		xatlas::Destroy(xa);
		err = "GenerateLightmapChartsXatlas: xatlas mesh count mismatch";
		return false;
	}

	// Per job: map output triangles back to input triangles through the
	// xref triple (output order is chart-major, NOT input order; winding
	// may be fixed and corners rotated, so match as a multiset and bind
	// corners positionally).
	for (size_t ji = 0; ji < jobs.size(); ++ji) {
		const Job& job = jobs[ji];
		const StaticMeshUpload& upload = *job.up;
		SgLightmapXatlasMeshOutput& mo = out.meshes[job.inputIdx];
		const xatlas::Mesh& xm = xa->meshes[ji];
		const uint32_t triCount = uint32_t(upload.indices.size() / 3);
		if (xm.chartCount == 0 || xm.indexCount != triCount * 3
			|| xm.vertexCount == 0) {
			mo.ok = false;
			continue;
		}
		// Sorted-triple -> input triangles (first wins; exact-duplicate
		// faces share UVs anyway).
		std::unordered_map<XatlasTriKey, std::vector<uint32_t>,
			XatlasTriKeyHash> faceMap;
		faceMap.reserve(size_t(triCount) * 2);
		for (uint32_t t = 0; t < triCount; ++t) {
			uint32_t ix[3] = { upload.indices[t * 3 + 0],
				upload.indices[t * 3 + 1], upload.indices[t * 3 + 2] };
			std::sort(ix, ix + 3);
			faceMap[{ ix[0], ix[1], ix[2] }].push_back(t);
		}
		std::vector<char> assigned(triCount, 0);
		std::vector<uint32_t> triChart(triCount, 0xFFFFFFFFu);
		std::vector<glm::vec2> cornerAtlasUv(size_t(triCount) * 3,
			glm::vec2(0.0f));
		const uint32_t outTris = xm.indexCount / 3;
		for (uint32_t ot = 0; ot < outTris; ++ot) {
			const uint32_t ov[3] = { xm.indexArray[ot * 3 + 0],
				xm.indexArray[ot * 3 + 1], xm.indexArray[ot * 3 + 2] };
			if (ov[0] >= xm.vertexCount || ov[1] >= xm.vertexCount
				|| ov[2] >= xm.vertexCount)
				continue;
			uint32_t xr[3] = { xm.vertexArray[ov[0]].xref,
				xm.vertexArray[ov[1]].xref, xm.vertexArray[ov[2]].xref };
			uint32_t key[3] = { xr[0], xr[1], xr[2] };
			std::sort(key, key + 3);
			auto it = faceMap.find({ key[0], key[1], key[2] });
			if (it == faceMap.end())
				continue;
			uint32_t t = 0xFFFFFFFFu;
			for (uint32_t cand : it->second) {
				if (!assigned[cand]) {
					t = cand;
					break;
				}
			}
			if (t == 0xFFFFFFFFu)
				continue; // exact-duplicate face already placed
			assigned[t] = 1;
			// Bind output corners to input corners positionally.
			bool used[3] = { false, false, false };
			int32_t chart = -1;
			for (int k = 0; k < 3; ++k) {
				int pick = -1;
				for (int p = 0; p < 3; ++p) {
					if (!used[p]
						&& upload.indices[size_t(t) * 3 + p] == xr[k]) {
						pick = p;
						break;
					}
				}
				if (pick < 0) {
					for (int p = 0; p < 3; ++p) {
						if (!used[p]) {
							pick = p;
							break;
						}
					}
				}
				if (pick < 0)
					continue;
				used[pick] = true;
				cornerAtlasUv[size_t(t) * 3 + pick] =
					glm::vec2(xm.vertexArray[ov[k]].uv[0],
						xm.vertexArray[ov[k]].uv[1]);
				if (chart < 0)
					chart = xm.vertexArray[ov[k]].chartIndex;
			}
			if (chart >= 0)
				triChart[t] = uint32_t(chart);
		}
		// Chart id compaction: xatlas chartIndex values are dense
		// 0..chartCount-1 already; keep them (unassigned triangles stay
		// 0xFFFFFFFF = degenerate/uncharted, same convention as planar).
		const uint32_t chartCount = xm.chartCount;
		std::vector<glm::vec2> cMin(chartCount,
			glm::vec2(std::numeric_limits<float>::max()));
		std::vector<glm::vec2> cMax(chartCount,
			glm::vec2(std::numeric_limits<float>::lowest()));
		std::vector<char> cUsed(chartCount, 0);
		for (uint32_t t = 0; t < triCount; ++t) {
			if (triChart[t] == 0xFFFFFFFFu || triChart[t] >= chartCount)
				continue;
			cUsed[triChart[t]] = 1;
			for (int k = 0; k < 3; ++k) {
				const glm::vec2& u = cornerAtlasUv[size_t(t) * 3 + k];
				cMin[triChart[t]] = glm::min(cMin[triChart[t]], u);
				cMax[triChart[t]] = glm::max(cMax[triChart[t]], u);
			}
		}
		// Rects: snapped content bbox + margin ring (the ring lands in
		// xatlas pack padding, see po.padding above).
		mo.rects.clear();
		mo.chart.charts.clear();
		mo.chart.charts.reserve(chartCount);
		std::vector<uint32_t> chartRemap(chartCount, 0xFFFFFFFFu);
		uint32_t dense = 0;
		for (uint32_t c = 0; c < chartCount; ++c) {
			if (!cUsed[c])
				continue;
			chartRemap[c] = dense++;
			const int32_t x0 = std::max<int32_t>(
				int32_t(std::floor(cMin[c].x)) - int32_t(margin), 0);
			const int32_t y0 = std::max<int32_t>(
				int32_t(std::floor(cMin[c].y)) - int32_t(margin), 0);
			const int32_t x1 = int32_t(std::ceil(cMax[c].x))
				+ int32_t(margin);
			const int32_t y1 = int32_t(std::ceil(cMax[c].y))
				+ int32_t(margin);
			SgLightmapXatlasMeshOutput::Rect r;
			r.minX = float(x0);
			r.minY = float(y0);
			r.maxX = float(std::max(x1, x0 + int32_t(2 * margin + 1)));
			r.maxY = float(std::max(y1, y0 + int32_t(2 * margin + 1)));
			mo.rects.push_back(r);
			mo.chart.charts.push_back({});
		}
		if (dense == 0) {
			mo.ok = false;
			continue;
		}
		for (uint32_t t = 0; t < triCount; ++t) {
			if (triChart[t] != 0xFFFFFFFFu && triChart[t] < chartCount)
				triChart[t] = chartRemap[triChart[t]];
			else
				triChart[t] = 0xFFFFFFFFu;
		}
		// Chart-local corner UVs: (atlasUv - innerMin) / innerSize.
		std::vector<glm::vec2> cornerLocal(size_t(triCount) * 3,
			glm::vec2(0.0f));
		for (uint32_t t = 0; t < triCount; ++t) {
			if (triChart[t] == 0xFFFFFFFFu)
				continue;
			const auto& r = mo.rects[triChart[t]];
			const glm::vec2 innerMin(r.minX + float(margin),
				r.minY + float(margin));
			const glm::vec2 innerSize = glm::max(
				glm::vec2(r.maxX - r.minX - 2.0f * float(margin),
					r.maxY - r.minY - 2.0f * float(margin)),
				glm::vec2(1e-6f));
			for (int k = 0; k < 3; ++k) {
				cornerLocal[size_t(t) * 3 + k] = glm::clamp(
					(cornerAtlasUv[size_t(t) * 3 + k] - innerMin) / innerSize,
					glm::vec2(0.0f), glm::vec2(1.0f));
			}
		}
		// Per-vertex outputs (first chart to touch a vertex wins) +
		// per-triangle seam-correct view (same contract as planar).
		mo.chart.vertexChart.assign(upload.vertices.size(), 0xFFFFFFFFu);
		mo.chart.chartLocalUv.assign(upload.vertices.size(),
			glm::vec2(0.0f));
		for (uint32_t t = 0; t < triCount; ++t) {
			if (triChart[t] == 0xFFFFFFFFu)
				continue;
			for (int k = 0; k < 3; ++k) {
				const uint32_t v = upload.indices[t * 3 + k];
				if (v < mo.chart.vertexChart.size()
					&& mo.chart.vertexChart[v] == 0xFFFFFFFFu) {
					mo.chart.vertexChart[v] = triChart[t];
					mo.chart.chartLocalUv[v] =
						cornerLocal[size_t(t) * 3 + k];
				}
			}
		}
		mo.chart.triChart = triChart;
		mo.chart.cornerChartUv = std::move(cornerLocal);
		mo.chart.usedAuthoredUv2 = false;
		// Bake texels per chart (same rasterizer contract as the planar
		// path: first-writer-owns claim map, rect-local uv + margin).
		for (uint32_t ci = 0; ci < mo.chart.charts.size(); ++ci) {
			const auto& r = mo.rects[ci];
			SgLightmapChartResult::Chart& oc = mo.chart.charts[ci];
			oc.width = std::max<uint32_t>(
				uint32_t(r.maxX - r.minX), 2u * margin + 1u);
			oc.height = std::max<uint32_t>(
				uint32_t(r.maxY - r.minY), 2u * margin + 1u);
			const uint32_t iw = std::max<uint32_t>(oc.width - 2u * margin, 1u);
			const uint32_t ih =
				std::max<uint32_t>(oc.height - 2u * margin, 1u);
			std::vector<uint32_t> claim(size_t(iw) * ih, 0xFFFFFFFFu);
			for (uint32_t t = 0; t < triCount; ++t) {
				if (triChart[t] != ci)
					continue;
				const uint32_t i0 = upload.indices[t * 3 + 0];
				const uint32_t i1 = upload.indices[t * 3 + 1];
				const uint32_t i2 = upload.indices[t * 3 + 2];
				const glm::vec3& p0 = upload.vertices[i0].pos;
				const glm::vec3& p1 = upload.vertices[i1].pos;
				const glm::vec3& p2 = upload.vertices[i2].pos;
				const glm::vec3& n0 = upload.vertices[i0].normal;
				const glm::vec3& n1 = upload.vertices[i1].normal;
				const glm::vec3& n2 = upload.vertices[i2].normal;
				const glm::vec2 u0 = mo.chart.cornerChartUv[size_t(t) * 3 + 0];
				const glm::vec2 u1 = mo.chart.cornerChartUv[size_t(t) * 3 + 1];
				const glm::vec2 u2 = mo.chart.cornerChartUv[size_t(t) * 3 + 2];
				const glm::vec2 e0 = u1 - u0;
				const glm::vec2 e1 = u2 - u0;
				const float det = e0.x * e1.y - e0.y * e1.x;
				if (std::abs(det) < 1e-12f)
					continue;
				const glm::vec2 tMin = glm::min(u0, glm::min(u1, u2));
				const glm::vec2 tMax = glm::max(u0, glm::max(u1, u2));
				uint32_t tx0 = std::min(uint32_t(std::max(0.0f,
					tMin.x * float(iw) - 0.5f)), iw - 1u);
				uint32_t tx1 = std::min(uint32_t(std::min(float(iw),
					tMax.x * float(iw) + 0.5f)), iw - 1u);
				uint32_t ty0 = std::min(uint32_t(std::max(0.0f,
					tMin.y * float(ih) - 0.5f)), ih - 1u);
				uint32_t ty1 = std::min(uint32_t(std::min(float(ih),
					tMax.y * float(ih) + 0.5f)), ih - 1u);
				for (uint32_t y = ty0; y <= ty1; ++y) {
					for (uint32_t x = tx0; x <= tx1; ++x) {
						const size_t idx = size_t(y) * iw + x;
						if (claim[idx] != 0xFFFFFFFFu)
							continue;
						const float u = (float(x) + 0.5f) / float(iw);
						const float v = (float(y) + 0.5f) / float(ih);
						const glm::vec2 d(u - u0.x, v - u0.y);
						const float b1 = (d.x * e1.y - d.y * e1.x) / det;
						const float b2 = (e0.x * d.y - e0.y * d.x) / det;
						const float b0 = 1.0f - b1 - b2;
						if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
							continue;
						claim[idx] = 1u;
						SgLightmapTexel tx{};
						tx.position = p0 * b0 + p1 * b1 + p2 * b2;
						tx.normal = glm::normalize(n0 * b0 + n1 * b1 + n2 * b2);
						tx.uv = glm::vec2(float(margin + x) + 0.5f,
							float(margin + y) + 0.5f);
						oc.texels.push_back(tx);
					}
				}
			}
		}
		mo.ok = true;
	}
	out.atlasWidth = xa->width + 1u; // +1 sampling pad (PackRects parity)
	out.atlasHeight = xa->height + 1u;
	xatlas::Destroy(xa);
	return true;
}

bool AppendXatlasLevelToAtlas(SgLightmapAtlas& atlas,
	const SgLightmapXatlasLevel& level, const std::vector<uint32_t>& meshKeys,
	uint32_t preferredLobes, uint32_t atlasWidthCap, uint32_t maxHeight,
	std::vector<uint32_t>& outChunkBase, std::string& err) {
	outChunkBase.assign(level.meshes.size(), 0xFFFFFFFFu);
	if (!LightmapLobeCountValid(preferredLobes)) {
		err = "AppendXatlasLevelToAtlas: bad lobe count";
		return false;
	}
	if (atlasWidthCap == 0 || maxHeight == 0) {
		err = "AppendXatlasLevelToAtlas: bad atlas caps";
		return false;
	}
	// Vertical stacking origin: fresh atlas starts at 0, appends continue
	// past the +1 sampling pad with the pack separator (content of the
	// previous level ends at height-1, so the gap is sep+1 >= 3).
	const float offY = atlas.chunks.empty()
		? 0.0f : float(atlas.height) + float(kXatlasStackSep);
	float needH = offY;
	uint32_t newW = atlas.width;
	for (size_t i = 0; i < level.meshes.size(); ++i) {
		const SgLightmapXatlasMeshOutput& mo = level.meshes[i];
		if (!mo.ok)
			continue;
		for (const auto& r : mo.rects) {
			const float w = r.maxX - r.minX;
			const float h = r.maxY - r.minY;
			if (r.minX + w > float(atlasWidthCap) + 1e-3f) {
				err = "AppendXatlasLevelToAtlas: chart wider than the atlas "
					"(lower --lm-density)";
				return false;
			}
			needH = std::max(needH, offY + (r.maxY - 0.0f));
			newW = std::max(newW, uint32_t(std::ceil(r.maxX)));
		}
	}
	// +1 sampling pad, like PackRects.
	uint32_t finalH = std::min(uint32_t(std::ceil(needH)) + 1u, maxHeight + 1u);
	if (float(finalH - 1u) < needH - 1e-3f) {
		err = "AppendXatlasLevelToAtlas: atlas height overflow "
			"(lower --lm-density or raise the budget)";
		return false;
	}
	newW = std::min(newW + 1u, atlasWidthCap);
	atlas.width = std::max(atlas.width, newW);
	atlas.height = finalH;
	// Emit chunk/payload shells (zeroed, preferred-count stride; the bake
	// resizes to the per-mesh chosen count).
	for (size_t i = 0; i < level.meshes.size(); ++i) {
		const SgLightmapXatlasMeshOutput& mo = level.meshes[i];
		if (!mo.ok || mo.rects.empty())
			continue;
		outChunkBase[i] = uint32_t(atlas.chunks.size());
		const uint32_t meshKey = i < meshKeys.size() ? meshKeys[i] : 0u;
		for (size_t c = 0; c < mo.rects.size(); ++c) {
			const auto& r = mo.rects[c];
			const uint32_t w = std::max<uint32_t>(
				uint32_t(r.maxX - r.minX), 1u);
			const uint32_t h = std::max<uint32_t>(
				uint32_t(r.maxY - r.minY), 1u);
			SgLightmapChunk chunk;
			chunk.lobeCount = preferredLobes;
			chunk.meshKey = meshKey;
			chunk.rectMin[0] = r.minX;
			chunk.rectMin[1] = offY + r.minY;
			chunk.rectMax[0] = r.minX + float(w);
			chunk.rectMax[1] = offY + r.minY + float(h);
			atlas.chunks.push_back(chunk);
			SgLightmapAtlas::ChunkPayload p;
			p.width = w;
			p.height = h;
			p.coeffs.assign(size_t(w) * h * preferredLobes,
				glm::vec3(0.0f));
			atlas.payloads.push_back(std::move(p));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Atlas: pack, sample, layers, IO
// ---------------------------------------------------------------------------

glm::vec2 SgLightmapAtlas::AtlasUv(uint32_t chunk, uint32_t margin,
	const glm::vec2& chartLocalUv) const {
	// Chart-local [0,1] -> inner rect (margin .. size-margin) -> atlas
	// texel -> normalized atlas uv (what the cook writes into texCoord2;
	// the runtime shader samples the atlas directly at it).
	if (chunk >= chunks.size())
		return glm::vec2(0.0f);
	const SgLightmapChunk& c = chunks[chunk];
	const float w = std::max(c.rectMax[0] - c.rectMin[0], 1.0f);
	const float h = std::max(c.rectMax[1] - c.rectMin[1], 1.0f);
	const float innerW = std::max(w - 2.0f * float(margin), 1.0f);
	const float innerH = std::max(h - 2.0f * float(margin), 1.0f);
	const glm::vec2 texel(c.rectMin[0] + float(margin)
		+ glm::clamp(chartLocalUv.x, 0.0f, 1.0f) * innerW,
		c.rectMin[1] + float(margin)
		+ glm::clamp(chartLocalUv.y, 0.0f, 1.0f) * innerH);
	return texel / glm::vec2(float(std::max(width, 1u)), float(std::max(height, 1u)));
}

bool SgLightmapAtlas::PackRects(const std::vector<RectRequest>& requests,
	uint32_t atlasWidth, uint32_t maxHeight) {
	chunks.clear();
	payloads.clear();
	width = 0;
	height = 0;
	if (requests.empty() || atlasWidth == 0)
		return false;
	for (const RectRequest& r : requests)
		if (!LightmapLobeCountValid(r.lobeCount) || r.size.x == 0 || r.size.y == 0)
			return false;
	chunks.resize(requests.size());
	std::vector<uint32_t> order(requests.size());
	for (uint32_t i = 0; i < requests.size(); ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		if (requests[a].size.y != requests[b].size.y)
			return requests[a].size.y > requests[b].size.y;
		return requests[a].size.x > requests[b].size.x;
	});
	// Inter-rect separation (texels): charts carry their own inner gutter
	// (gutterTexels per side, see SgLightmapChartOptions), so a fixed gap
	// here is what keeps bilinear at one chart's content edge from ever
	// touching the NEXT chart's content. gutter + sep + gutter = the
	// island-to-island padding (1 + 2 + 1 = 4 texels at gutter 1).
	// Packing flush (sep 0) relied on both gutters alone, which is fine
	// for large charts but wastes half the atlas on the tiny ornament
	// charts (12k+ on sponza): per-chart gutter scales with chart COUNT,
	// the pack gap is paid once between rects.
	constexpr uint32_t kPackSep = 2;
	uint32_t rowX = 0, rowY = 0, rowH = 0;
	for (uint32_t idx : order) {
		const uint32_t w = requests[idx].size.x;
		const uint32_t h = requests[idx].size.y;
		if (w > atlasWidth)
			return false;
		if (rowX > 0 && rowX + kPackSep + w > atlasWidth) {
			rowX = 0;
			rowY += rowH + kPackSep;
			rowH = 0;
		}
		if (rowY + h > maxHeight)
			return false;
		SgLightmapChunk& c = chunks[idx];
		c.rectMin[0] = float(rowX);
		c.rectMin[1] = float(rowY);
		c.rectMax[0] = float(rowX + w);
		c.rectMax[1] = float(rowY + h);
		c.lobeCount = requests[idx].lobeCount;
		c.meshKey = requests[idx].meshKey;
		rowX += w + kPackSep;
		rowH = std::max(rowH, h);
		width = std::max(width, rowX > kPackSep ? rowX - kPackSep : rowX);
		height = std::max(height, rowY + rowH);
	}
	// One texel of padding at the pack edges keeps bilinear clamps from
	// touching neighbor chunks; empty texels stay zero.
	height = std::min(height + 1u, maxHeight);
	width = std::min(width + 1u, atlasWidth);
	payloads.resize(requests.size());
	for (size_t i = 0; i < requests.size(); ++i) {
		payloads[i].width = requests[i].size.x;
		payloads[i].height = requests[i].size.y;
		payloads[i].coeffs.assign(size_t(requests[i].size.x)
			* requests[i].size.y * requests[i].lobeCount, glm::vec3(0.0f));
	}
	return true;
}

bool SgLightmapAtlas::Sample(uint32_t chunkIdx, const glm::vec2& uv,
	glm::vec3* outAmps) const {
	for (uint32_t i = 0; i < kLightmapMaxLobes; ++i)
		outAmps[i] = glm::vec3(0.0f);
	if (chunkIdx >= chunks.size() || payloads.empty())
		return false;
	const SgLightmapChunk& c = chunks[chunkIdx];
	const ChunkPayload& p = payloads[chunkIdx];
	const uint32_t n = std::min<uint32_t>(c.lobeCount, kLightmapMaxLobes);
	if (p.width == 0 || p.height == 0 || n == 0)
		return false;
	const float gx = glm::clamp(uv.x, 0.0f, 1.0f) * float(p.width) - 0.5f;
	const float gy = glm::clamp(uv.y, 0.0f, 1.0f) * float(p.height) - 0.5f;
	const int x0 = int(std::floor(gx));
	const int y0 = int(std::floor(gy));
	const float fx = glm::clamp(gx - float(x0), 0.0f, 1.0f);
	const float fy = glm::clamp(gy - float(y0), 0.0f, 1.0f);
	auto at = [&](int x, int y, uint32_t lobe) -> glm::vec3 {
		x = glm::clamp(x, 0, int(p.width) - 1);
		y = glm::clamp(y, 0, int(p.height) - 1);
		return p.coeffs[(size_t(y) * p.width + size_t(x)) * c.lobeCount + lobe];
	};
	for (uint32_t l = 0; l < n; ++l) {
		const glm::vec3 a = glm::mix(at(x0, y0, l), at(x0 + 1, y0, l), fx);
		const glm::vec3 b = glm::mix(at(x0, y0 + 1, l), at(x0 + 1, y0 + 1, l), fx);
		outAmps[l] = glm::mix(a, b, fy);
	}
	return true;
}

void SgLightmapAtlas::PackGpuLayers(std::vector<uint16_t> out[kLightmapLayerCount]) const {
	// RGBA16F texel = 4 halves; layers are w*h*4 entries (8 B/texel).
	for (uint32_t t = 0; t < kLightmapLayerCount; ++t)
		out[t].assign(size_t(width) * height * 4, uint16_t(0));
	for (size_t ci = 0; ci < chunks.size(); ++ci) {
		const SgLightmapChunk& c = chunks[ci];
		const ChunkPayload& p = payloads[ci];
		const uint32_t n = std::min<uint32_t>(c.lobeCount, kLightmapMaxLobes);
		const int ox = int(c.rectMin[0]);
		const int oy = int(c.rectMin[1]);
		for (uint32_t y = 0; y < p.height; ++y) {
			for (uint32_t x = 0; x < p.width; ++x) {
				// 36-slot lobe-major RGB half stream (slot n..35 stay 0).
				uint16_t halves[kLightmapMaxLobes * 3 + 4] = {};
				for (uint32_t l = 0; l < n; ++l) {
					const glm::vec3& a =
						p.coeffs[(size_t(y) * p.width + size_t(x)) * c.lobeCount + l];
					halves[l * 3 + 0] = PackHalf(a.r);
					halves[l * 3 + 1] = PackHalf(a.g);
					halves[l * 3 + 2] = PackHalf(a.b);
				}
				const size_t dst = size_t(oy + int(y)) * width + size_t(ox + int(x));
				for (uint32_t t = 0; t < kLightmapLayerCount; ++t)
					for (uint32_t k = 0; k < 4; ++k)
						out[t][dst * 4 + k] = halves[t * 4 + k];
			}
		}
	}
}

size_t SgLightmapAtlas::GpuBytes() const {
	return size_t(width) * height * kLightmapLayerCount * 8; // 9 RGBA16F texels
}

bool SgLightmapAtlas::WriteFile(const std::string& path, std::string& err) const {
	SglmFileHeader hdr{};
	hdr.atlasWidth = width;
	hdr.atlasHeight = height;
	hdr.chunkCount = uint32_t(chunks.size());

	// Post-header section: chunk table (fixed stride) + payloads.
	std::vector<uint8_t> section;
	section.reserve(chunks.size() * 32 + 1024);
	auto putU32 = [&](uint32_t v) {
		uint8_t b[4];
		std::memcpy(b, &v, 4);
		section.insert(section.end(), b, b + 4);
	};
	auto putF32 = [&](float v) {
		uint8_t b[4];
		std::memcpy(b, &v, 4);
		section.insert(section.end(), b, b + 4);
	};
	for (const SgLightmapChunk& c : chunks) {
		putU32(c.lobeCount);
		putU32(c.meshKey);
		putF32(c.rectMin[0]);
		putF32(c.rectMin[1]);
		putF32(c.rectMax[0]);
		putF32(c.rectMax[1]);
	}
	// Payloads: fixed kLightmapMaxLobes stride per texel keeps the reader
	// simple; unused lobes store zero halves.
	for (size_t ci = 0; ci < chunks.size(); ++ci) {
		const SgLightmapChunk& c = chunks[ci];
		const ChunkPayload& p = payloads[ci];
		const uint32_t n = std::min<uint32_t>(c.lobeCount, kLightmapMaxLobes);
		putU32(p.width);
		putU32(p.height);
		// Fixed kLightmapMaxLobes stride per texel keeps the reader simple;
		// unused lobes store zero halves.
		const size_t texels = size_t(p.width) * p.height;
		section.resize(section.size() + texels * kLightmapMaxLobes * 6, 0);
		uint16_t* dst = reinterpret_cast<uint16_t*>(section.data()
			+ section.size() - texels * kLightmapMaxLobes * 6);
		for (size_t i = 0; i < texels; ++i) {
			for (uint32_t l = 0; l < n; ++l) {
				const glm::vec3& a = p.coeffs[i * c.lobeCount + l];
				dst[(i * kLightmapMaxLobes + l) * 3 + 0] = PackHalf(a.r);
				dst[(i * kLightmapMaxLobes + l) * 3 + 1] = PackHalf(a.g);
				dst[(i * kLightmapMaxLobes + l) * 3 + 2] = PackHalf(a.b);
			}
		}
	}

	// Section complete (chunk table + payloads; lightmap UVs live in the
	// COOKED pak mesh vertices — the .sglm carries no geometry).
	if (section.size() > 0xFFFFFFFFull) {
		err = "SgLightmapAtlas: section overflows u32 payloadBytes";
		return false;
	}
	hdr.payloadBytes = uint32_t(section.size());
	// Segmented LZ4 (v2): the full section of a 16M-texel atlas (~1.2 GB)
	// cannot pass the single-block codec (kMaxBlobBytes), so it is
	// compressed in kSglmSegmentBytes slices with a size table.
	hdr.compressed = 2;
	const size_t segCount = (section.size() + kSglmSegmentBytes - 1)
		/ kSglmSegmentBytes;
	std::vector<std::vector<uint8_t>> segs(segCount);
	for (size_t s = 0; s < segCount; ++s) {
		const size_t off = s * kSglmSegmentBytes;
		const size_t len = std::min<size_t>(kSglmSegmentBytes,
			section.size() - off);
		if (!Lz4Compress(std::span<const uint8_t>(section.data() + off, len),
				segs[s], 1)) {
			err = "SgLightmapAtlas: LZ4 compression failed (segment "
				+ std::to_string(s) + ")";
			return false;
		}
	}
	std::vector<uint8_t> file;
	size_t segBytes = 0;
	for (const auto& s : segs)
		segBytes += s.size();
	file.reserve(sizeof(hdr) + 4 + segCount * 16 + segBytes);
	file.insert(file.end(), reinterpret_cast<const uint8_t*>(&hdr),
		reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
	auto putU32v = [&](uint32_t v) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
		file.insert(file.end(), p, p + 4);
	};
	auto putU64v = [&](uint64_t v) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
		file.insert(file.end(), p, p + 8);
	};
	putU32v(uint32_t(segCount));
	for (size_t s = 0; s < segCount; ++s) {
		const size_t rawLen = std::min<size_t>(kSglmSegmentBytes,
			section.size() - s * kSglmSegmentBytes);
		putU64v(uint64_t(rawLen));
		putU64v(uint64_t(segs[s].size()));
	}
	for (const auto& s : segs)
		file.insert(file.end(), s.begin(), s.end());

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "SgLightmapAtlas: cannot open output " + path;
		return false;
	}
	const size_t written = std::fwrite(file.data(), 1, file.size(), f);
	std::fclose(f);
	if (written != file.size()) {
		err = "SgLightmapAtlas: short write to " + path;
		return false;
	}
	return true;
}

bool SgLightmapAtlas::ReadFromMemory(std::span<const uint8_t> bytes, std::string& err) {
	SglmFileHeader hdr{};
	if (bytes.size() < sizeof(hdr)) {
		err = "sglm: file too small";
		return false;
	}
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	if (std::memcmp(hdr.magic, "SGLM", 4) != 0) {
		err = "sglm: bad magic";
		return false;
	}
	if (hdr.version != 1 && hdr.version != kSglmVersion) {
		err = "sglm: unsupported version " + std::to_string(hdr.version);
		return false;
	}
	if (hdr.atlasWidth == 0 || hdr.atlasHeight == 0
		|| hdr.chunkCount == 0 || hdr.chunkCount > 1048576
		|| hdr.payloadBytes == 0) {
		err = "sglm: bad header";
		return false;
	}
	std::vector<uint8_t> section;
	const std::span<const uint8_t> stored(bytes.data() + sizeof(hdr),
		bytes.size() - sizeof(hdr));
	if (hdr.compressed == 2) {
		// Segmented LZ4 (v2): u32 segCount + (u64 raw, u64 comp) table +
		// segCount blocks. Raw sizes must tile payloadBytes exactly.
		if (stored.size() < 4)
			return err = "sglm: truncated segment table", false;
		uint32_t segCount = 0;
		std::memcpy(&segCount, stored.data(), 4);
		const size_t tableEnd = 4 + size_t(segCount) * 16;
		if (segCount == 0 || segCount > 64 || stored.size() < tableEnd)
			return err = "sglm: bad segment table", false;
		section.resize(hdr.payloadBytes);
		size_t fileOff = tableEnd;
		size_t rawOff = 0;
		for (uint32_t s = 0; s < segCount; ++s) {
			uint64_t rawLen = 0, compLen = 0;
			std::memcpy(&rawLen, stored.data() + 4 + size_t(s) * 16, 8);
			std::memcpy(&compLen, stored.data() + 4 + size_t(s) * 16 + 8, 8);
			if (rawLen == 0 || rawLen > kSglmSegmentBytes
				|| rawOff + rawLen > section.size()
				|| compLen == 0 || fileOff + compLen > stored.size())
				return err = "sglm: bad segment table entry", false;
			std::vector<uint8_t> rawSeg;
			if (!Lz4Decompress(
					std::span<const uint8_t>(stored.data() + fileOff,
						size_t(compLen)),
					size_t(rawLen), rawSeg))
				return err = "sglm: segment LZ4 decompression failed", false;
			std::memcpy(section.data() + rawOff, rawSeg.data(), size_t(rawLen));
			rawOff += size_t(rawLen);
			fileOff += size_t(compLen);
		}
		if (rawOff != section.size())
			return err = "sglm: segment sizes do not tile the payload", false;
	}
	else if (hdr.compressed == 1) {
		// v1 single LZ4 block (files written before the segment format;
		// sections over kMaxBlobBytes cannot exist in this layout).
		if (!Lz4Decompress(stored, hdr.payloadBytes, section)) {
			err = "sglm: LZ4 decompression failed";
			return false;
		}
	}
	else {
		if (stored.size() != hdr.payloadBytes)
			return err = "sglm: raw payload size mismatch", false;
		section.assign(stored.begin(), stored.end());
	}
	size_t off = 0;
	auto take = [&](void* dst, size_t n) -> bool {
		if (off + n > section.size())
			return false;
		std::memcpy(dst, section.data() + off, n);
		off += n;
		return true;
	};
	width = hdr.atlasWidth;
	height = hdr.atlasHeight;
	chunks.resize(hdr.chunkCount);
	for (SgLightmapChunk& c : chunks) {
		struct Row { uint32_t lobeCount, meshKey; float r[4]; } row{};
		if (!take(&row, sizeof(row)))
			return err = "sglm: truncated chunk table", false;
		c.lobeCount = row.lobeCount;
		c.meshKey = row.meshKey;
		c.rectMin[0] = row.r[0];
		c.rectMin[1] = row.r[1];
		c.rectMax[0] = row.r[2];
		c.rectMax[1] = row.r[3];
		if (!LightmapLobeCountValid(c.lobeCount))
			return err = "sglm: bad lobe count", false;
	}
	payloads.resize(hdr.chunkCount);
	for (size_t ci = 0; ci < chunks.size(); ++ci) {
		uint32_t dims[2]{};
		if (!take(dims, sizeof(dims)))
			return err = "sglm: truncated payload", false;
		ChunkPayload& p = payloads[ci];
		p.width = dims[0];
		p.height = dims[1];
		const uint32_t n = chunks[ci].lobeCount;
		const size_t texels = size_t(p.width) * p.height;
		if (texels == 0 || texels > 64u * 1024 * 1024)
			return err = "sglm: bad payload dims", false;
		p.coeffs.assign(texels * n, glm::vec3(0.0f));
		std::vector<uint16_t> raw(texels * kLightmapMaxLobes * 3);
		if (!take(raw.data(), raw.size() * sizeof(uint16_t)))
			return err = "sglm: truncated payload data", false;
		for (size_t i = 0; i < texels; ++i)
			for (uint32_t l = 0; l < n; ++l)
				p.coeffs[i * n + l] = glm::vec3(
					UnpackHalf(raw[(i * kLightmapMaxLobes + l) * 3 + 0]),
					UnpackHalf(raw[(i * kLightmapMaxLobes + l) * 3 + 1]),
					UnpackHalf(raw[(i * kLightmapMaxLobes + l) * 3 + 2]));
	}
	return true;
}

bool SgLightmapAtlas::ReadFile(const std::string& path, SgLightmapAtlas& out,
	std::string& err) {
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
// Bake
// ---------------------------------------------------------------------------

namespace {

// Rect-local texel ownership from the bake texel list (uv in rect-local
// texel coords): first writer wins, matching the chart rasterizer.
static void BuildTexelOf(const std::vector<SgLightmapTexel>& texels,
	uint32_t w, uint32_t h, std::vector<uint32_t>& texelOf) {
	std::fill(texelOf.begin(), texelOf.end(), 0xFFFFFFFFu);
	for (uint32_t i = 0; i < texels.size(); ++i) {
		const int x = int(std::floor(texels[i].uv.x));
		const int y = int(std::floor(texels[i].uv.y));
		if (x < 0 || y < 0 || x >= int(w) || y >= int(h))
			continue;
		texelOf[size_t(y) * w + size_t(x)] = i;
	}
}

// Shared payload tail: amplitude denoise (over baked texels only) +
// gutter/hole dilation. Runs after the coeffs hold the fit at the
// chunk's lobeCount stride.
static void FinishChunkPayload(SgLightmapAtlas::ChunkPayload& out,
	const std::vector<uint32_t>& texelOf, uint32_t lobeCount) {
	const uint32_t w = out.width;
	const uint32_t h = out.height;
	const size_t total = size_t(w) * h;

	// Gutter + hole dilation: every texel that did not bake takes the
	// average of its valid 4-neighbours (Jacobi passes until stable).
	std::vector<char> valid(total, 0);
	for (size_t i = 0; i < total; ++i)
		valid[i] = texelOf[i] != 0xFFFFFFFFu ? 1 : 0;

	// One amplitude smoothing pass over BAKED texels (SG coefficient
	// denoise, BakingLab-style): per-texel fits are independent Monte
	// Carlo estimates, and at low densities the per-texel noise reads
	// as speckle/faceting on smooth surfaces. Center-weighted (4x) 3x3
	// average over valid neighbours only — the SG basis keeps the
	// directional detail, the texel-to-texel estimate noise goes away.
	// Runs BEFORE dilation so gutters continue the smoothed content.
	{
		std::vector<glm::vec3> smooth(total * lobeCount);
		static const int kSx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		static const int kSy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		static const float kSw[8] = { 1, 1, 1, 1, 0.5f, 0.5f, 0.5f, 0.5f };
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x) {
				const size_t v = size_t(y) * w + x;
				if (!valid[v])
					continue;
				glm::vec3 acc[kLightmapMaxLobes] = {};
				float wsum = 4.0f;
				for (uint32_t l = 0; l < lobeCount; ++l)
					acc[l] = 4.0f * out.coeffs[v * lobeCount + l];
				for (int k = 0; k < 8; ++k) {
					const int nx = int(x) + kSx[k];
					const int ny = int(y) + kSy[k];
					if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h))
						continue;
					const size_t u = size_t(ny) * w + size_t(nx);
					if (!valid[u])
						continue;
					for (uint32_t l = 0; l < lobeCount; ++l)
						acc[l] += kSw[k] * out.coeffs[u * lobeCount + l];
					wsum += kSw[k];
				}
				for (uint32_t l = 0; l < lobeCount; ++l)
					smooth[v * lobeCount + l] = acc[l] / wsum;
			}
		for (size_t i = 0; i < total; ++i)
			if (valid[i])
				for (uint32_t l = 0; l < lobeCount; ++l)
					out.coeffs[i * lobeCount + l] = smooth[i * lobeCount + l];
	}

	for (int pass = 0; pass < 16; ++pass) {
		bool anyFill = false;
		std::vector<glm::vec3> fill(total * lobeCount);
		std::vector<char> fillMask(total, 0);
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x) {
				const size_t v = size_t(y) * w + x;
				if (valid[v])
					continue;
				glm::vec3 acc[kLightmapMaxLobes] = {};
				int cnt = 0;
				static const int kDx[4] = { 1, -1, 0, 0 };
				static const int kDy[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; ++k) {
					const int nx = int(x) + kDx[k];
					const int ny = int(y) + kDy[k];
					if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h))
						continue;
					const size_t u = size_t(ny) * w + size_t(nx);
					if (!valid[u])
						continue;
					for (uint32_t l = 0; l < lobeCount; ++l)
						acc[l] += out.coeffs[u * lobeCount + l];
					++cnt;
				}
				if (cnt > 0) {
					for (uint32_t l = 0; l < lobeCount; ++l)
						fill[v * lobeCount + l] = acc[l] / float(cnt);
					fillMask[v] = 1;
					anyFill = true;
				}
			}
		for (size_t i = 0; i < total; ++i)
			if (fillMask[i]) {
				for (uint32_t l = 0; l < lobeCount; ++l)
					out.coeffs[i * lobeCount + l] = fill[i * lobeCount + l];
				valid[i] = 1;
			}
		if (!anyFill)
			break;
	}
}

// Core: trace + fit every covered texel of ONE chunk payload, using the
// caller's pool (shared across the batch). Covered texels come from
// `texels` (uv in rect-local texel coords); dilation fills the rest.
void BakeChunkCore(const GiTraceScene& scene, uint32_t lobeCount,
	const std::vector<SgLightmapTexel>& texels,
	const SgLightmapBakeConfig& cfg, const std::vector<glm::vec3>& fibDirs,
	const float weight, SgLightmapAtlas::ChunkPayload& out,
	std::vector<uint32_t>& texelOf, ThreadPool& pool,
	std::atomic<uint64_t>& baked, std::atomic<uint64_t>& clamped,
	uint64_t chunkSeed) {
	const uint32_t w = out.width;
	const uint32_t h = out.height;
	BuildTexelOf(texels, w, h, texelOf);
	constexpr float kSurfEps = 1e-3f; // matches the probe bakers
	const size_t total = size_t(w) * h;
	std::vector<std::future<void>> futures;
	for (size_t idx = 0; idx < total; ++idx) {
		const uint32_t texelIdx = texelOf[idx];
		if (texelIdx == 0xFFFFFFFFu)
			continue;
		futures.push_back(pool.enqueue([&, idx, texelIdx] {
			const SgLightmapTexel& tx = texels[texelIdx];
			const uint64_t seed = (uint64_t(cfg.seed) << 32)
				^ (chunkSeed * 0x9e3779b97f4a7c15ull)
				^ (uint64_t(texelIdx) * 0xbf58476d1ce4e5b9ull);
			Pcg32 rng(seed);
			struct RaySample { glm::vec3 dir; glm::vec3 L; };
			std::vector<RaySample> samples;
			samples.reserve(fibDirs.size());
			double meanLum = 0.0;
			const glm::vec3 origin = tx.position + tx.normal * kSurfEps;
			for (const glm::vec3& d : fibDirs) {
				const glm::vec3 L = scene.TraceIncoming(origin, d, rng,
					cfg.maxBounces);
				samples.push_back({ d, L });
				meanLum += Luminance(L);
			}
			meanLum /= double(std::max<size_t>(fibDirs.size(), 1));
			const float cap = std::max(float(meanLum) * 8.0f, 1e-6f);
			SgLightmapFitAccumulator acc(lobeCount);
			uint64_t localClamped = 0;
			for (const RaySample& s : samples) {
				glm::vec3 L = s.L;
				const float l = Luminance(L);
				if (l > cap && l > 0.0f) {
					L *= cap / l;
					++localClamped;
				}
				acc.AddSample(s.dir, L, weight);
			}
			clamped += localClamped;
			glm::vec3 amps[kLightmapMaxLobes];
			acc.SolveGram(amps);
			for (uint32_t l = 0; l < lobeCount; ++l)
				out.coeffs[idx * lobeCount + l] = amps[l];
			++baked;
		}));
	}
	for (auto& f : futures)
		f.get();
	FinishChunkPayload(out, texelOf, lobeCount);
}

// ---------------------------------------------------------------------------
// Per-mesh lobe-count selection (Order:1886-style): fit ALL candidate
// bases per texel (5/6/9/12 up to the preferred count) with the Gram
// solve, score each candidate's irradiance response against the traced
// probes, keep the smallest count under the relative-error budget, then
// solve + finish every chunk of the mesh at that count. One count per
// MESH: the runtime draw ring carries a single lobe count per draw
// (GfxDraw.misc.w), so all chart chunks of the mesh share it.
// ---------------------------------------------------------------------------

// Probe-normal count for the lobe-count selection: the fitted irradiance
// E(n) is scored against the traced irradiance over a small hemisphere of
// directions around the texel normal (the shading directions that
// matter — the runtime convolves the SG field with exactly these
// responses, so a count that reconstructs the probes reconstructs the
// view).
static constexpr uint32_t kLmProbeCount = 6;

// Deterministic probe hemisphere around a texel normal: the normal plus
// five ~54.7-degree tilts (0.577 = 1/sqrt(3)) spanning the tangent plane.
// Identical normals in the accumulation (ground truth) and the scoring —
// the selection would be inconsistent otherwise.
static void LmProbeNormals(const glm::vec3& n, glm::vec3 out[kLmProbeCount]) {
	out[0] = n;
	const glm::vec3 helper = std::abs(n.y) < 0.9f
		? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	glm::vec3 t1 = glm::cross(n, helper);
	t1 = glm::length(t1) > 1e-6f ? t1 / glm::length(t1) : glm::vec3(1.0f, 0.0f, 0.0f);
	const glm::vec3 t2 = glm::cross(n, t1);
	constexpr float k = 0.577f;
	out[1] = glm::normalize(n + t1 * k);
	out[2] = glm::normalize(n - t1 * k);
	out[3] = glm::normalize(n + t2 * k);
	out[4] = glm::normalize(n - t2 * k);
	out[5] = glm::normalize(n + (t1 + t2) * k);
}

struct LmFitTexel {
	// rhs of each candidate count's basis (only the count's prefix is
	// touched; kLightmapMaxLobes slots keep indexing trivial).
	glm::vec3 rhs[kLobeCandidateSlots][kLightmapMaxLobes] = {};
	// Ground-truth diffuse irradiance at the probe hemisphere
	// (sum w * L * max(dot(d, n_k), 0) — what the runtime's Hill-fitted
	// cosine convolution must reproduce).
	glm::vec3 eTrue[kLmProbeCount] = {};
	// Diagnostics + selection score, computed in the parallel pass:
	//  - err2/ref2: candidate response vs the TRACED probes. Dominated by
	//    the ground truth's own Monte-Carlo noise at practical ray
	//    counts (measured: ~20-80% at 128 rays, nearly count-INdependent),
	//    so it is reported but never thresholded.
	//  - deg2/refBest2: candidate response vs the PREFERRED-count
	//    reconstruction — both smooth fits of the SAME samples, so the
	//    MC noise cancels and the number measures pure representation
	//    loss. This drives the selection.
	double err2[kLobeCandidateSlots] = {};
	double ref2 = 0.0;
	double deg2[kLobeCandidateSlots] = {};
	double refBest2 = 0.0;
};

void BakeMeshGroupLobeSelect(const GiTraceScene& scene,
	const SgLightmapBakeConfig& cfg, uint32_t preferredLobeCount,
	const std::vector<glm::vec3>& fibDirs, const float weight,
	std::span<const SgLightmapChunkBatch> group, uint64_t groupSeedBase,
	ThreadPool& pool, std::atomic<uint64_t>& baked,
	std::atomic<uint64_t>& clamped, SgLightmapBakeStats* stats) {
	// Candidates: 5/6/9/12 up to the preferred count.
	uint32_t cand[kLobeCandidateSlots];
	uint32_t candCount = 0;
	for (uint32_t c : kLobeCandidates)
		if (c <= preferredLobeCount)
			cand[candCount++] = c;
	if (candCount == 0)
		return;

	// Flatten the group's bake texels (chunk-major, original order).
	struct ChunkRef {
		const SgLightmapChunkBatch* batch = nullptr;
		size_t texelBase = 0;
		uint64_t chunkSeed = 0;
		uint32_t covered = 0;
	};
	std::vector<ChunkRef> refs;
	std::vector<uint32_t> texelChunk;
	size_t totalTexels = 0;
	refs.reserve(group.size());
	for (size_t ci = 0; ci < group.size(); ++ci) {
		const SgLightmapChunkBatch& b = group[ci];
		if (!b.payload || !b.texels || b.payload->width == 0
			|| b.payload->height == 0)
			continue;
		ChunkRef r;
		r.batch = &b;
		r.texelBase = totalTexels;
		r.chunkSeed = groupSeedBase + uint64_t(ci) + 1;
		r.covered = uint32_t(b.texels->size());
		totalTexels += b.texels->size();
		texelChunk.insert(texelChunk.end(), b.texels->size(), uint32_t(refs.size()));
		refs.push_back(r);
	}
	if (refs.empty() || totalTexels == 0)
		return;
	std::vector<LmFitTexel> fit(totalTexels);

	// Pass 1 (parallel): trace + accumulate every candidate basis, then
	// Gram-solve each candidate IN-TASK and score its irradiance response
	// against the traced probes (the solve is the expensive part — folding
	// it into the trace tasks keeps the pool saturated instead of paying a
	// serial 4x-Gram pass over every texel).
	constexpr float kSurfEps = 1e-3f;
	std::vector<std::future<void>> futures;
	futures.reserve(totalTexels);
	for (size_t t = 0; t < totalTexels; ++t) {
		futures.push_back(pool.enqueue([&, t] {
			const ChunkRef& r = refs[texelChunk[t]];
			const SgLightmapTexel& tx = (*r.batch->texels)[t - r.texelBase];
			const uint64_t seed = (uint64_t(cfg.seed) << 32)
				^ (r.chunkSeed * 0x9e3779b97f4a7c15ull)
				^ (uint64_t(t - r.texelBase) * 0xbf58476d1ce4e5b9ull);
			Pcg32 rng(seed);
			const glm::vec3 origin = tx.position + tx.normal * kSurfEps;
			double meanLum = 0.0;
			std::vector<glm::vec3> Ls;
			Ls.reserve(fibDirs.size());
			for (const glm::vec3& d : fibDirs) {
				const glm::vec3 L = scene.TraceIncoming(origin, d, rng,
					cfg.maxBounces);
				Ls.push_back(L);
				meanLum += Luminance(L);
			}
			meanLum /= double(std::max<size_t>(fibDirs.size(), 1));
			const float cap = std::max(float(meanLum) * 8.0f, 1e-6f);
			LmFitTexel& f = fit[t];
			glm::vec3 probes[kLmProbeCount];
			LmProbeNormals(tx.normal, probes);
			uint64_t localClamped = 0;
			for (size_t ri = 0; ri < Ls.size(); ++ri) {
				glm::vec3 L = Ls[ri];
				const float l = Luminance(L);
				if (l > cap && l > 0.0f) {
					L *= cap / l;
					++localClamped;
				}
				const glm::vec3& d = fibDirs[ri];
				for (uint32_t s = 0; s < candCount; ++s) {
					const SgBasisTable* bt = BasisFor(cand[s]);
					for (uint32_t i = 0; i < cand[s]; ++i) {
						const float b = std::exp(bt->sharpness
							* (glm::dot(bt->axes[i], d) - 1.0f));
						f.rhs[s][i] += L * (b * weight);
					}
				}
				for (uint32_t k = 0; k < kLmProbeCount; ++k) {
					const float c = std::max(glm::dot(d, probes[k]), 0.0f);
					if (c > 0.0f)
						f.eTrue[k] += L * (c * weight);
				}
			}
			clamped += localClamped;
			// Score every candidate. Preferred first: its response at the
			// probes is the degradation reference (best available
			// reconstruction of these samples).
			glm::vec3 amps[kLightmapMaxLobes];
			glm::vec3 eBest[kLmProbeCount] = {};
			SolveRhsGram(f.rhs[candCount - 1], cand[candCount - 1], amps);
			for (uint32_t k = 0; k < kLmProbeCount; ++k) {
				eBest[k] = SgLightmapDiffuse(amps, cand[candCount - 1], probes[k]);
				f.refBest2 += double(glm::dot(eBest[k], eBest[k]));
			}
			for (uint32_t s = 0; s < candCount; ++s) {
				SolveRhsGram(f.rhs[s], cand[s], amps);
				for (uint32_t k = 0; k < kLmProbeCount; ++k) {
					const glm::vec3 eFit =
						SgLightmapDiffuse(amps, cand[s], probes[k]);
					const glm::vec3 diff = eFit - f.eTrue[k];
					f.err2[s] += double(glm::dot(diff, diff));
					const glm::vec3 deg = eFit - eBest[k];
					f.deg2[s] += double(glm::dot(deg, deg));
				}
			}
			for (uint32_t k = 0; k < kLmProbeCount; ++k)
				f.ref2 += double(glm::dot(f.eTrue[k], f.eTrue[k]));
		}));
	}
	for (auto& f : futures)
		f.get();

	// Pass 2 (serial reduce): choose the smallest count whose response
	// degrades less than the budget relative to the PREFERRED-count
	// reconstruction. Degradation, not against-truth: the traced probe
	// ground truth carries Monte-Carlo noise that dominates any practical
	// ray count (measured ~20-80% at 128 rays, nearly count-independent),
	// while candidate-vs-preferred compares two smooth fits of the SAME
	// samples — pure representation loss, noise-free. The chosen slot
	// doubles as the failure default (preferred count); the per-pick
	// diagnostics make "preferred genuinely won" vs "nothing passed"
	// distinguishable.
	double sumRef2 = 0.0, sumRefBest2 = 0.0;
	double sumErr2[kLobeCandidateSlots] = {};
	double sumDeg2[kLobeCandidateSlots] = {};
	for (size_t t = 0; t < totalTexels; ++t) {
		sumRef2 += fit[t].ref2;
		sumRefBest2 += fit[t].refBest2;
		for (uint32_t s = 0; s < candCount; ++s) {
			sumErr2[s] += fit[t].err2[s];
			sumDeg2[s] += fit[t].deg2[s];
		}
	}
	uint32_t chosenSlot = candCount - 1; // preferred count (last candidate)
	if (sumRefBest2 > 1e-12) {
		const double relBudget = double(cfg.lobeRelError) * double(cfg.lobeRelError);
		for (uint32_t s = 0; s < candCount; ++s) {
			if (sumDeg2[s] <= relBudget * sumRefBest2) {
				chosenSlot = s;
				break;
			}
		}
	} else {
		chosenSlot = 0; // unlit mesh: nothing to represent, 5 lobes suffice
	}
	const uint32_t chosen = cand[chosenSlot];
	if (stats) {
		SgLightmapBakeStats::LobePick pick{};
		pick.meshKey = group.front().meshKey;
		pick.chosen = chosen;
		pick.candCount = candCount;
		for (uint32_t s = 0; s < candCount; ++s) {
			pick.candCounts[s] = cand[s];
			pick.relErr[s] = sumRef2 > 1e-12
				? std::sqrt(sumErr2[s] / sumRef2) : 0.0;
			pick.degErr[s] = sumRefBest2 > 1e-12
				? std::sqrt(sumDeg2[s] / sumRefBest2) : 0.0;
		}
		stats->lobePicks.push_back(pick);
	}

	// Pass 3 (parallel per chunk, then serial finish): solve the chosen
	// count and write every chunk's payload.
	std::vector<uint32_t> texelOf;
	for (const ChunkRef& r : refs) {
		SgLightmapAtlas::ChunkPayload& p = *r.batch->payload;
		const uint32_t w = p.width;
		const uint32_t h = p.height;
		p.coeffs.assign(size_t(w) * h * chosen, glm::vec3(0.0f));
		texelOf.assign(size_t(w) * h, 0xFFFFFFFFu);
		BuildTexelOf(*r.batch->texels, w, h, texelOf);
		const size_t total = size_t(w) * h;
		std::vector<std::future<void>> wf;
		for (size_t idx = 0; idx < total; ++idx) {
			const uint32_t texelIdx = texelOf[idx];
			if (texelIdx == 0xFFFFFFFFu)
				continue;
			wf.push_back(pool.enqueue([&, idx, texelIdx] {
				const size_t t = r.texelBase + texelIdx;
				glm::vec3 amps[kLightmapMaxLobes];
				SolveRhsGram(fit[t].rhs[chosenSlot], chosen, amps);
				for (uint32_t l = 0; l < chosen; ++l)
					p.coeffs[idx * chosen + l] = amps[l];
			}));
		}
		for (auto& f : wf)
			f.get();
		baked += uint64_t(texelOf.size() > 0
			? size_t(std::count_if(texelOf.begin(), texelOf.end(),
				[](uint32_t v) { return v != 0xFFFFFFFFu; })) : 0);
		FinishChunkPayload(p, texelOf, chosen);
		if (r.batch->chunkMeta)
			r.batch->chunkMeta->lobeCount = chosen;
	}
}

} // namespace

bool BakeSgLightmapChunks(const GiTraceScene& scene, uint32_t lobeCount,
	std::span<const SgLightmapChunkBatch> batches,
	const SgLightmapBakeConfig& configIn, SgLightmapBakeStats& stats,
	GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	if (!LightmapLobeCountValid(lobeCount)) {
		if (progress)
			progress("lightmap bake: bad lobe count\n");
		return false;
	}
	if (batches.empty()) {
		if (progress)
			progress("lightmap bake: no chunks\n");
		return false;
	}
	SgLightmapBakeConfig cfg = configIn;
	cfg.raysPerTexel = std::max(cfg.raysPerTexel, 16u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);
	// Shared deterministic Fibonacci sphere (correlated error reads as
	// smooth bias, not per-texel speckle).
	std::vector<glm::vec3> fibDirs;
	fibDirs.reserve(cfg.raysPerTexel);
	for (uint32_t i = 0; i < cfg.raysPerTexel; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(cfg.raysPerTexel);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		fibDirs.push_back(glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r));
	}
	const float weight = 4.0f * kPi / float(cfg.raysPerTexel);
	const uint32_t threadCount = cfg.threadCount
		? cfg.threadCount : std::thread::hardware_concurrency();
	ThreadPool pool(threadCount ? threadCount : 1);
	std::atomic<uint64_t> baked{ 0 };
	std::atomic<uint64_t> clamped{ 0 };
	size_t maxRect = 0;
	for (const SgLightmapChunkBatch& b : batches)
		if (b.payload)
			maxRect = std::max(maxRect, size_t(b.payload->width)
				* b.payload->height);
	std::vector<uint32_t> texelOf(maxRect);
	// Progress: start line + ~2s-cadence updates with texel count,
	// elapsed and eta (the old fixed every-256-chunks cadence went
	// silent for minutes on low-chunk/high-texel scenes).
	uint64_t totalTexels = 0;
	for (const SgLightmapChunkBatch& b : batches)
		if (b.payload)
			totalTexels += uint64_t(b.payload->width) * b.payload->height;
	std::chrono::steady_clock::time_point lastReport =
		std::chrono::steady_clock::now();
	if (progress) {
		char line[192];
		std::snprintf(line, sizeof(line),
			"lightmap bake: %llu chunks, %llu texels, %u rays/texel, %u threads\n",
			(unsigned long long)batches.size(),
			(unsigned long long)totalTexels,
			cfg.raysPerTexel, threadCount ? threadCount : 1);
		progress(line);
	}
	auto reportProgress = [&](size_t chunksDone) {
		if (!progress)
			return;
		const auto now = std::chrono::steady_clock::now();
		if (now - lastReport < std::chrono::seconds(2))
			return;
		lastReport = now;
		const double elapsed =
			std::chrono::duration<double>(now - t0).count();
		const double frac = batches.size()
			? double(chunksDone) / double(batches.size()) : 1.0;
		char line[192];
		std::snprintf(line, sizeof(line),
			"lightmap bake: %llu/%llu chunks (%.0f%%), %llu texels, %.1fs, eta %.0fs\n",
			(unsigned long long)chunksDone,
			(unsigned long long)batches.size(),
			frac * 100.0,
			(unsigned long long)baked.load(),
			elapsed,
			frac > 0.005 ? elapsed / frac * (1.0 - frac) : 0.0);
		progress(line);
	};
	size_t chunksDone = 0;
	size_t bi = 0;
	while (bi < batches.size()) {
		const SgLightmapChunkBatch& head = batches[bi];
		if (!head.payload || !head.texels || head.payload->width == 0
			|| head.payload->height == 0) {
			++bi;
			continue;
		}
		// Group contiguous batches sharing a meshKey (mesh-major cook
		// order gives one group per mesh; 0/missing meta degrades to
		// the fixed-count path).
		size_t bj = bi + 1;
		if (cfg.chooseLobeCount && head.chunkMeta && head.meshKey != 0) {
			while (bj < batches.size() && batches[bj].meshKey == head.meshKey
				&& batches[bj].chunkMeta && batches[bj].payload
				&& batches[bj].texels && batches[bj].payload->width != 0)
				++bj;
			BakeMeshGroupLobeSelect(scene, cfg, lobeCount, fibDirs, weight,
				std::span<const SgLightmapChunkBatch>(batches).subspan(bi, bj - bi),
				uint64_t(bi), pool, baked, clamped, &stats);
			chunksDone += bj - bi;
			reportProgress(chunksDone);
			bi = bj;
			continue;
		}
		// Fixed-count fallback (no selection data or selection off).
		SgLightmapChunkBatch b = head;
		if (b.payload->coeffs.size() != size_t(b.payload->width)
			* b.payload->height * lobeCount)
			b.payload->coeffs.assign(size_t(b.payload->width)
				* b.payload->height * lobeCount, glm::vec3(0.0f));
		texelOf.resize(size_t(b.payload->width) * b.payload->height);
		BakeChunkCore(scene, lobeCount, *b.texels, cfg, fibDirs, weight,
			*b.payload, texelOf, pool, baked, clamped, uint64_t(bi) + 1);
		++chunksDone;
		reportProgress(chunksDone);
		++bi;
	}
	stats.texelsBaked = baked.load();
	stats.pathsTraced = uint64_t(baked.load()) * cfg.raysPerTexel;
	stats.samplesClamped = clamped.load();
	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	if (progress) {
		char line[192];
		std::snprintf(line, sizeof(line),
			"lightmap bake: done: %llu chunks, %llu texels in %.1fs (%llu clamped)\n",
			(unsigned long long)batches.size(),
			(unsigned long long)stats.texelsBaked,
			stats.seconds, (unsigned long long)stats.samplesClamped);
		progress(line);
	}
	return true;
}

bool BakeSgLightmapChunk(const GiTraceScene& scene, uint32_t lobeCount,
	const std::vector<SgLightmapTexel>& texels, const SgLightmapBakeConfig& configIn,
	SgLightmapAtlas::ChunkPayload& out, SgLightmapBakeStats& stats,
	GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	if (!LightmapLobeCountValid(lobeCount)) {
		if (progress)
			progress("lightmap bake: bad lobe count\n");
		return false;
	}
	SgLightmapBakeConfig cfg = configIn;
	cfg.raysPerTexel = std::max(cfg.raysPerTexel, 16u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);
	if (out.width == 0 || out.height == 0) {
		if (progress)
			progress("lightmap bake: empty chunk rect\n");
		return false;
	}
	if (out.coeffs.size() != size_t(out.width) * out.height * lobeCount)
		out.coeffs.assign(size_t(out.width) * out.height * lobeCount,
			glm::vec3(0.0f));

	auto say = [&](const char* line) {
		if (progress)
			progress(line);
	};

// Shared deterministic Fibonacci sphere (correlated error reads as
// smooth bias, not speckle).
	std::vector<glm::vec3> fibDirs;
	fibDirs.reserve(cfg.raysPerTexel);
	for (uint32_t i = 0; i < cfg.raysPerTexel; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(cfg.raysPerTexel);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		fibDirs.push_back(glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r));
	}
	const float weight = 4.0f * kPi / float(cfg.raysPerTexel);
	const uint32_t threadCount = cfg.threadCount
		? cfg.threadCount : std::thread::hardware_concurrency();
	ThreadPool pool(threadCount ? threadCount : 1);
	std::atomic<uint64_t> baked{ 0 };
	std::atomic<uint64_t> clamped{ 0 };
	std::vector<uint32_t> texelOf(size_t(out.width) * out.height);
	BakeChunkCore(scene, lobeCount, texels, cfg, fibDirs, weight,
		out, texelOf, pool, baked, clamped, 1);

	stats.texelsBaked = baked.load();
	stats.pathsTraced = uint64_t(baked.load()) * cfg.raysPerTexel;
	stats.samplesClamped = clamped.load();
	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	if (progress) {
		char line[192];
		std::snprintf(line, sizeof(line),
			"lightmap bake: done: %llu texels in %.1fs (%llu clamped)%s",
			(unsigned long long)stats.texelsBaked,
			stats.seconds, (unsigned long long)stats.samplesClamped, "\n");
		progress(line);
	}
	return true;
}
} // namespace smvk::gi
