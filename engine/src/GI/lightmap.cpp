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

namespace smvk::gi {

namespace {

constexpr float kPi = 3.14159265358979323846f;

constexpr uint32_t kSglmVersion = 1;

struct SglmFileHeader {
	char magic[4] = { 'S', 'G', 'L', 'M' };
	uint32_t version = kSglmVersion;
	uint32_t atlasWidth = 0;
	uint32_t atlasHeight = 0;
	uint32_t chunkCount = 0;
	uint32_t payloadBytes = 0; // uncompressed post-header section
	uint32_t compressed = 0;   // 1 = LZ4 block
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

void SgLightmapFitAccumulator::Solve(glm::vec3* outAmps) const {
	const SgBasisTable* t = BasisFor(lobeCount_);
	if (!t) {
		for (uint32_t i = 0; i < lobeCount_; ++i)
			outAmps[i] = glm::vec3(0.0f);
		return;
	}
	// Diagonal projection (production path): amp = rhs / selfEnergy with
	// selfEnergy = pi/lambda * (1 - exp(-4 lambda)) — analytic, same
	// convention as SgSelfEnergy in sg.h.
	const float selfEnergy = kPi / t->sharpness
		* (1.0f - std::exp(-4.0f * t->sharpness));
	const float inv = 1.0f / std::max(selfEnergy, 1e-8f);
	for (uint32_t i = 0; i < lobeCount_; ++i) {
		glm::vec3 a = rhs_[i] * inv;
		// fp16 ceiling per channel (payload stores halves).
		a = glm::clamp(a, glm::vec3(0.0f), glm::vec3(65504.0f));
		if (!std::isfinite(a.x + a.y + a.z))
			a = glm::vec3(0.0f);
		outAmps[i] = a;
	}
}

// ---------------------------------------------------------------------------
// Charts: greedy planar unwrap, per-vertex UV2 (no geometry replacement)
// ---------------------------------------------------------------------------

bool GenerateLightmapCharts(const StaticMeshUpload& upload,
	const SgLightmapChartOptions& opts, SgLightmapChartResult& out,
	std::string& err) {
	out = SgLightmapChartResult{};
	const uint32_t triCount = uint32_t(upload.indices.size() / 3);
	if (triCount == 0) {
		err = "GenerateLightmapCharts: mesh has no triangles";
		return false;
	}
	if (opts.texelsPerMeter == 0 || opts.gutterTexels == 0 || opts.gutterTexels > 4) {
		err = "GenerateLightmapCharts: bad options";
		return false;
	}
	const uint32_t margin = opts.gutterTexels; // inner margin per chart

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
	// Chart rect sizes from projected bboxes (+ inner margins).
	for (Chart& ch : charts) {
		const glm::vec2 ext = glm::max(ch.pMax - ch.pMin, glm::vec2(1e-4f));
		ch.w = std::max<uint32_t>(uint32_t(std::ceil(ext.x * float(opts.texelsPerMeter))) + 2u * margin, 2u * margin + 1u);
		ch.h = std::max<uint32_t>(uint32_t(std::ceil(ext.y * float(opts.texelsPerMeter))) + 2u * margin, 2u * margin + 1u);
	}

	// Shelf pack: sort by height desc, rows of maxRectWidth. Chart rects
	// TOUCH (no gutter boxes): the shared inner margins prevent bleed.
	// The texel budget (opts.maxRectTexels) caps the region area so the
	// baker can lower the density for huge surfaces.
	const uint32_t chartCount = uint32_t(charts.size());
	std::vector<uint32_t> order(chartCount);
	for (uint32_t i = 0; i < chartCount; ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		if (charts[a].h != charts[b].h)
			return charts[a].h > charts[b].h;
		return charts[a].w > charts[b].w;
	});
	uint32_t rowX = 0, rowY = 0, rowH = 0;
	uint32_t packW = 0, packH = 0;
	for (uint32_t idx : order) {
		Chart& c = charts[idx];
		if (rowX + c.w > opts.maxRectWidth) {
			rowX = 0;
			rowY += rowH;
			rowH = 0;
		}
		if (rowY + c.h > opts.maxRectHeight) {
			err = "GenerateLightmapCharts: charts exceed max rect (lower --lm-density)";
			return false;
		}
		c.x0 = rowX;
		c.y0 = rowY;
		rowX += c.w;
		rowH = std::max(rowH, c.h);
		packW = std::max(packW, rowX);
		packH = std::max(packH, rowY + rowH);
	}
	if (opts.maxRectTexels > 0
		&& uint64_t(packW) * packH > opts.maxRectTexels) {
		err = "GenerateLightmapCharts: region exceeds texel budget (lower --lm-density)";
		return false;
	}
	out.rectWidth = packW;
	out.rectHeight = packH;
	out.rectMin[0] = 0.0f;
	out.rectMin[1] = 0.0f;
	out.rectMax[0] = float(packW);
	out.rectMax[1] = float(packH);

	// Chart-UV for a triangle corner (continuous projection over the
	// chart bbox; clamped to the chart's inner region).
	auto chartUv = [&](const Chart& c, const glm::vec3& p) -> glm::vec2 {
		const glm::vec2 q(p[c.plane.ax], p[c.plane.ay]);
		const glm::vec2 ext = glm::max(c.pMax - c.pMin, glm::vec2(1e-6f));
		return glm::clamp((q - c.pMin) / ext, glm::vec2(0.0f), glm::vec2(1.0f));
	};

	// Per-ORIGINAL-vertex lightmap UVs (the cook writes these into
	// StaticVertex::texCoord2). Vertex -> owning chart (first claim) ->
	// continuous projection over that chart's FINAL bbox, mapped into
	// the chart's inner rect and normalized over the mesh region.
	out.uv2.assign(upload.vertices.size(), glm::vec2(0.0f));
	for (size_t v = 0; v < upload.vertices.size(); ++v) {
		const uint32_t chartIdx = vertexChartOf[canonical[v]];
		if (chartIdx == 0xFFFFFFFFu)
			continue; // unclaimed (degenerate tri corner): (0,0)
		const Chart& c = charts[chartIdx];
		const glm::vec2 uv = chartUv(c, upload.vertices[v].pos);
		const float innerW = float(std::max<uint32_t>(c.w - 2u * margin, 1u));
		const float innerH = float(std::max<uint32_t>(c.h - 2u * margin, 1u));
		const glm::vec2 inner0{ float(margin), float(margin) };
		out.uv2[v] = (inner0 + uv * glm::vec2(innerW, innerH))
			/ glm::vec2(float(packW), float(packH));
	}

	// Bake texels: rasterize every chart triangle over the chart's inner
	// region (bounded by the triangle's own uv bbox for speed). Texel
	// ownership: first writer wins; shared-edge texels between adjacent
	// triangles carry the first triangle's interpolation (sub-texel
	// difference, invisible after the dilation pass).
	std::unordered_map<uint64_t, uint32_t> texelIndex;
	texelIndex.reserve(size_t(packW) * packH / 2 + 16);
	for (const Chart& c : charts) {
		const uint32_t ix0 = c.x0 + margin;
		const uint32_t iy0 = c.y0 + margin;
		const uint32_t iw = std::max<uint32_t>(c.w - 2u * margin, 1u);
		const uint32_t ih = std::max<uint32_t>(c.h - 2u * margin, 1u);
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
					const float u = (float(x) + 0.5f) / float(iw);
					const float v = (float(y) + 0.5f) / float(ih);
					const glm::vec2 d(u - u0.x, v - u0.y);
					const float b1 = (d.x * e1.y - d.y * e1.x) / det;
					const float b2 = (e0.x * d.y - e0.y * d.x) / det;
					const float b0 = 1.0f - b1 - b2;
					if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
						continue;
					const uint32_t gx = ix0 + x;
					const uint32_t gy = iy0 + y;
					const uint64_t key = uint64_t(gx) | (uint64_t(gy) << 32);
					if (texelIndex.contains(key))
						continue;
					SgLightmapTexel tx{};
					tx.position = p0 * b0 + p1 * b1 + p2 * b2;
					tx.normal = glm::normalize(n0 * b0 + n1 * b1 + n2 * b2);
					tx.uv = glm::vec2(float(gx) + 0.5f, float(gy) + 0.5f);
					texelIndex[key] = uint32_t(out.texels.size());
					out.texels.push_back(tx);
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Atlas: pack, sample, layers, IO
// ---------------------------------------------------------------------------

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
	uint32_t rowX = 0, rowY = 0, rowH = 0;
	for (uint32_t idx : order) {
		const uint32_t w = requests[idx].size.x;
		const uint32_t h = requests[idx].size.y;
		if (w > atlasWidth)
			return false;
		if (rowX + w > atlasWidth) {
			rowX = 0;
			rowY += rowH;
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
		rowX += w;
		rowH = std::max(rowH, h);
		width = std::max(width, rowX);
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
	hdr.payloadBytes = uint32_t(section.size());
	hdr.compressed = 1;
	std::vector<uint8_t> compressed;
	if (!Lz4Compress(section, compressed, 1)) {
		err = "SgLightmapAtlas: LZ4 compression failed";
		return false;
	}
	std::vector<uint8_t> file(sizeof(hdr) + compressed.size());
	std::memcpy(file.data(), &hdr, sizeof(hdr));
	std::memcpy(file.data() + sizeof(hdr), compressed.data(), compressed.size());

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
	if (hdr.version != kSglmVersion) {
		err = "sglm: unsupported version " + std::to_string(hdr.version);
		return false;
	}
	if (hdr.atlasWidth == 0 || hdr.atlasHeight == 0
		|| hdr.chunkCount == 0 || hdr.chunkCount > 65536) {
		err = "sglm: bad header";
		return false;
	}
	std::vector<uint8_t> section;
	const std::span<const uint8_t> stored(bytes.data() + sizeof(hdr),
		bytes.size() - sizeof(hdr));
	if (hdr.compressed) {
		if (!Lz4Decompress(stored, hdr.payloadBytes, section)) {
			err = "sglm: LZ4 decompression failed";
			return false;
		}
	} else {
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
	// smooth bias, not per-texel speckle — same rationale as the probe
	// bakers).
	std::vector<glm::vec3> fibDirs;
	fibDirs.reserve(cfg.raysPerTexel);
	for (uint32_t i = 0; i < cfg.raysPerTexel; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(cfg.raysPerTexel);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		fibDirs.push_back(glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r));
	}

	// Texel -> payload index map. Chart texels carry RECT-LOCAL coords
	// (x+0.5, y+0.5 with the rect origin at 0), matching the payload
	// indexing below; the atlas-level PackRects offset applies only to
	// GPU layer placement, never to these coords.
	const uint32_t w = out.width;
	const uint32_t h = out.height;
	std::vector<uint32_t> texelOf(size_t(w) * h, 0xFFFFFFFFu);
	for (uint32_t i = 0; i < texels.size(); ++i) {
		const int x = int(std::floor(texels[i].uv.x));
		const int y = int(std::floor(texels[i].uv.y));
		if (x < 0 || y < 0 || x >= int(w) || y >= int(h))
			continue;
		texelOf[size_t(y) * w + size_t(x)] = i;
	}

	const uint32_t threadCount = cfg.threadCount
		? cfg.threadCount : std::thread::hardware_concurrency();
	ThreadPool pool(threadCount ? threadCount : 1);
	std::atomic<uint64_t> baked{ 0 };
	std::atomic<uint64_t> clamped{ 0 };
	const float weight = 4.0f * kPi / float(cfg.raysPerTexel);
	constexpr float kSurfEps = 1e-3f; // matches the probe bakers

	constexpr size_t kBatch = 512;
	const size_t total = size_t(w) * h;
	for (size_t base = 0; base < total; base += kBatch) {
		const size_t end = std::min(base + kBatch, total);
		std::vector<std::future<void>> futures;
		futures.reserve(end - base);
		for (size_t idx = base; idx < end; ++idx) {
			const uint32_t texelIdx = texelOf[idx];
			if (texelIdx == 0xFFFFFFFFu)
				continue;
			futures.push_back(pool.enqueue([&, idx, texelIdx] {
				const SgLightmapTexel& tx = texels[texelIdx];
				const uint64_t seed = (uint64_t(cfg.seed) << 32)
					^ (uint64_t(texelIdx) * 0x9e3779b97f4a7c15ull);
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
				acc.Solve(amps);
				for (uint32_t l = 0; l < lobeCount; ++l)
					out.coeffs[idx * lobeCount + l] = amps[l];
				++baked;
			}));
		}
		for (auto& f : futures)
			f.get();
	}

	// Gutter + hole dilation: every texel that did not bake takes the
	// average of its valid 4-neighbours (Jacobi passes until stable; the
	// per-triangle charts leave 1-2 texel gaps plus the authored gutters).
	{
		std::vector<char> valid(total, 0);
		for (size_t i = 0; i < total; ++i)
			valid[i] = texelOf[i] != 0xFFFFFFFFu ? 1 : 0;
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

	stats.texelsBaked = baked.load();
	stats.pathsTraced = uint64_t(baked.load()) * cfg.raysPerTexel;
	stats.samplesClamped = clamped.load();
	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	{
		char line[192];
		std::snprintf(line, sizeof(line),
			"lightmap bake: done: %llu/%llu texels in %.1fs (%llu clamped)\n",
			(unsigned long long)stats.texelsBaked, (unsigned long long)total,
			stats.seconds, (unsigned long long)stats.samplesClamped);
		say(line);
	}
	return true;
}

} // namespace smvk::gi
