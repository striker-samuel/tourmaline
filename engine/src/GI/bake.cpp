// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/GI/bake.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <thread>

#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>
#include <Engine/Utils/thread_pool.h>

#include "ktx.h"

namespace smvk::gi {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxRayDist = 1.0e5f;
constexpr float kSurfEps = 1.0e-3f; // ray offset along the shading normal (m)

inline float Luminance(const glm::vec3& c) {
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

glm::vec3 UniformSphere(Pcg32& rng) {
	const glm::vec2 u = rng.Float2();
	const float z = 1.0f - 2.0f * u.x;
	const float r = SafeSqrt(1.0f - z * z);
	const float phi = 2.0f * kPi * u.y;
	return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

inline void Onb(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
	const glm::vec3 up = std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	t = glm::normalize(glm::cross(up, n));
	b = glm::cross(n, t);
}

glm::vec3 CosineHemisphere(Pcg32& rng, const glm::vec3& n) {
	const glm::vec2 u = rng.Float2();
	const float r = std::sqrt(u.x);
	const float phi = 2.0f * kPi * u.y;
	glm::vec3 t, b;
	Onb(n, t, b);
	return glm::normalize(t * (r * std::cos(phi)) + n * std::sqrt(1.0f - u.x) + b * (r * std::sin(phi)));
}

} // namespace

// ---------------------------------------------------------------------------
// GiTraceScene
// ---------------------------------------------------------------------------

void GiTraceScene::Finalize() {
	// Validate the hand-populatable soup before the BVH build: a bad
	// vertex index OOB-reads in TriangleVertices, a bad material index
	// OOB-reads at every shading vertex (garbage albedo -> NaN
	// throughput -> non-finite radiance). Malformed entries are
	// repaired/dropped with a loud stderr report (GiTests'
	// TestGiMalformedScene pins this behavior); consistent scenes built
	// by BuildTraceSceneFromCluster never print.
	if (indices.size() % 3 != 0) {
		std::fprintf(stderr,
			"GI FINALIZE: %zu indices not a multiple of 3; truncating\n",
			indices.size());
		indices.resize(indices.size() / 3 * 3);
	}
	const size_t triCount = indices.size() / 3;
	if (triCount > 0 && materials.empty()) {
		std::fprintf(stderr,
			"GI FINALIZE: geometry with no materials; injecting a default\n");
		materials.push_back(GiTraceMaterial{});
	}
	if (triMaterial.size() != triCount) {
		std::fprintf(stderr,
			"GI FINALIZE: material map mismatch: %zu triangles, %zu entries\n",
			triCount, triMaterial.size());
		triMaterial.resize(triCount, 0u);
	}
	{
		size_t dropped = 0;
		std::vector<uint32_t> keptIndices;
		std::vector<uint32_t> keptTriMaterial;
		keptIndices.reserve(indices.size());
		keptTriMaterial.reserve(triCount);
		for (size_t t = 0; t < triCount; ++t) {
			const bool validMat = triMaterial[t] < materials.size();
			const bool validIdx = indices[t * 3 + 0] < positions.size()
				&& indices[t * 3 + 1] < positions.size()
				&& indices[t * 3 + 2] < positions.size();
			if (!validMat || !validIdx) {
				++dropped;
				continue;
			}
			keptIndices.push_back(indices[t * 3 + 0]);
			keptIndices.push_back(indices[t * 3 + 1]);
			keptIndices.push_back(indices[t * 3 + 2]);
			keptTriMaterial.push_back(triMaterial[t]);
		}
		if (dropped > 0) {
			std::fprintf(stderr,
				"GI FINALIZE: dropped %zu of %zu triangles (bad vertex/material index)\n",
				dropped, triCount);
			indices.swap(keptIndices);
			triMaterial.swap(keptTriMaterial);
		}
	}

	bvh_.Build(positions, indices, traceBackend_);
	const uint32_t triCountFinal = uint32_t(indices.size() / 3);

	emissiveTris_.clear();
	emissiveCdf_.clear();
	emissiveTotal_ = 0.0f;
	for (uint32_t k = 0; k < triCountFinal; ++k) {
		const GiTraceMaterial& m = materials[triMaterial[k]];
		const float lum = Luminance(m.emissive);
		if (lum <= 1e-6f)
			continue;
		glm::vec3 v0, v1, v2;
		bvh_.TriangleVertices(k, v0, v1, v2);
		const float area = 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
		if (area <= 1e-9f)
			continue;
		EmissiveTri e{};
		e.tri = k;
		e.area = area;
		e.radiance = m.emissive;
		emissiveTotal_ += area * lum;
		emissiveTris_.push_back(e);
	}
	if (!emissiveTris_.empty()) {
		emissiveCdf_.resize(emissiveTris_.size());
		float cdf = 0.0f;
		for (size_t i = 0; i < emissiveTris_.size(); ++i) {
			cdf += emissiveTris_[i].area * Luminance(emissiveTris_[i].radiance);
			emissiveCdf_[i] = cdf;
		}
	}
}

glm::vec3 GiTraceScene::SkyRadiance(const glm::vec3& dir) const {
	const float exponent = std::max(skyExponent, 0.01f);
	const glm::vec3 zenith = skyZenith * skyIntensity;
	const glm::vec3 horizon = skyHorizon * skyIntensity;
	const glm::vec3 groundCol = skyGround * skyIntensity;
	const float t = std::clamp(dir.y, 0.0f, 1.0f);
	glm::vec3 sky = glm::mix(horizon, zenith, std::pow(t, exponent));
	if (dir.y < 0.0f)
		sky = glm::mix(horizon, groundCol, std::clamp(-dir.y * 3.0f, 0.0f, 1.0f));
	return sky;
}

glm::vec3 GiTraceScene::SampleEmissive(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m, Pcg32& rng) const {
	if (emissiveTris_.empty() || emissiveTotal_ <= 0.0f)
		return glm::vec3(0.0f);

	const float u = rng.Float() * emissiveTotal_;
	const auto it = std::lower_bound(emissiveCdf_.begin(), emissiveCdf_.end(), u);
	size_t idx = size_t(it - emissiveCdf_.begin());
	idx = std::min(idx, emissiveTris_.size() - 1);
	const EmissiveTri& e = emissiveTris_[idx];
	const float pSelect = (e.area * Luminance(e.radiance)) / emissiveTotal_;

	glm::vec3 v0, v1, v2;
	bvh_.TriangleVertices(e.tri, v0, v1, v2);
	const glm::vec2 r = rng.Float2();
	const float s = std::sqrt(r.x);
	const float a = 1.0f - s;
	const float b = s * (1.0f - r.y);
	const float c = s * r.y;
	const glm::vec3 y = v0 * a + v1 * b + v2 * c;

	const glm::vec3 w = y - pos;
	const float dist2 = glm::dot(w, w);
	const float dist = std::sqrt(dist2);
	if (dist < 1e-4f)
		return glm::vec3(0.0f);
	const glm::vec3 dirTo = w / dist;

	// Lambertian receiver: no light from below the hemisphere.
	const float cosSurf = glm::dot(dirTo, n);
	if (cosSurf <= 0.0f)
		return glm::vec3(0.0f);

	const glm::vec3 triN = bvh_.TriangleNormal(e.tri);
	const float cosTri = std::abs(glm::dot(triN, dirTo));
	if (cosTri < 1e-4f)
		return glm::vec3(0.0f);

	const glm::vec3 origin = pos + n * kSurfEps;
	if (bvh_.AnyHit(origin, dirTo, dist - 2.0f * kSurfEps))
		return glm::vec3(0.0f);

	// Joint area pdf = pSelect / A_e; solid-angle pdf = area pdf * d^2 / cosTri.
	const float pdfSa = pSelect * dist2 / (e.area * cosTri);
	const float brdf = cosSurf * (1.0f / kPi);
	return e.radiance * m.albedo * (brdf / pdfSa);
}

glm::vec3 GiTraceScene::SampleSun(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m) const {
	return SampleSun(pos, n, m, m.albedo);
}

glm::vec3 GiTraceScene::SampleSun(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m, const glm::vec3& vertexAlbedo) const {
	const float ndl = glm::dot(n, sunDirection);
	if (ndl <= 0.0f)
		return glm::vec3(0.0f);
	if (bvh_.AnyHit(pos + n * kSurfEps, sunDirection, kMaxRayDist))
		return glm::vec3(0.0f);
	return sunColor * (sunIntensity * ndl) * vertexAlbedo * (1.0f / kPi);
}

glm::vec3 GiTraceScene::SampleSpots(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m) const {
	return SampleSpots(pos, n, m, m.albedo);
}

glm::vec3 GiTraceScene::SampleSpots(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m, const glm::vec3& vertexAlbedo) const {
	// Mirrors EvaluateForwardPlusLight (default_lit_body.hlsli): Frostbite
	// windowed inverse-square falloff, candela convention, cone
	// smoothstep — so baked spots match the runtime direct path. Returns
	// the Lambertian reflected term (same convention as SampleSun).
	if (spots.empty())
		return glm::vec3(0.0f);
	glm::vec3 sum(0.0f);
	for (const GiTraceSpot& s : spots) {
		glm::vec3 toLight = s.position - pos;
		const float dist2 = glm::dot(toLight, toLight);
		if (s.range > 0.0f && dist2 > s.range * s.range)
			continue;
		const float dist = std::sqrt(dist2);
		const glm::vec3 dirTo = toLight / std::max(dist, 1e-6f);
		const float ndl = glm::dot(n, dirTo);
		if (ndl <= 0.0f)
			continue;
		// Cone falloff (axis points at the aim point).
		const float cosA = glm::dot(-dirTo, glm::normalize(s.direction));
		if (cosA < s.cosOuter)
			continue;
		const float t = std::clamp(
			(cosA - s.cosOuter) / std::max(s.cosInner - s.cosOuter, 1e-6f),
			0.0f, 1.0f);
		const float ang = t * t * (3.0f - 2.0f * t);
		// Windowed inverse square (candela convention).
		float window_ = 1.0f;
		if (s.range > 0.0f) {
			const float x = dist / s.range;
			window_ = std::pow(std::clamp(1.0f - x * x * x * x, 0.0f, 1.0f), 2.0f);
		}
		const float atten = window_ / std::max(4.0f * kPi * dist2, 1e-2f);
		if (bvh_.AnyHit(pos + n * kSurfEps, dirTo, dist - 2.0f * kSurfEps))
			continue;
		sum += s.color * (ndl * atten * ang);
	}
	return sum * vertexAlbedo * (1.0f / kPi);
}

glm::vec3 GiTraceScene::AlbedoAt(const GiTraceMaterial& m, uint32_t tri,
	const glm::vec2& baryUV) const {
	// Textured path: the material carries a decoded baseColor texture AND
	// the soup carries UVs parallel to positions. Anything missing falls
	// back to the flat averaged albedo (the pre-textured behavior — the
	// same scene can mix both when only some textures decoded).
	if (m.baseTexId == 0xFFFFFFFFu)
		return m.albedo;
	const GiTraceTexelData* data = nullptr;
	glm::vec2 uv(0.0f);
	if (!ResolveTexelAndUv(m.baseTexId, tri, baryUV, data, uv))
		return m.albedo;
	const glm::vec4 t = SampleTexelBilinear(*data, uv);
	return m.baseTexMul * glm::vec3(t);
}

glm::vec3 GiTraceScene::TransmissionAt(const GiTraceMaterial& m, uint32_t tri,
	const glm::vec2& baryUV) const {
	// Textured path (the weave-transmissive cloth): the pass-through tints
	// by the fabric texel the ray passes. MASK-cutout texels (the holes
	// between the cloth shapes) pass UNOBSTRUCTED (open air — the alpha
	// below the cutoff means the geometry is not there at all).
	if (m.transTexId == 0xFFFFFFFFu)
		return m.transmission;
	const GiTraceTexelData* data = nullptr;
	glm::vec2 uv(0.0f);
	if (!ResolveTexelAndUv(m.transTexId, tri, baryUV, data, uv))
		return m.transmission;
	const glm::vec4 t = SampleTexelBilinear(*data, uv);
	if (t.w < m.transAlphaCutoff)
		return glm::vec3(1.0f);
	return m.transTexMul * glm::vec3(t);
}

bool GiTraceScene::ResolveTexelAndUv(uint32_t texId, uint32_t tri,
	const glm::vec2& baryUV, const GiTraceTexelData*& outData,
	glm::vec2& outUv) const {
	auto it = textures.find(texId);
	if (it == textures.end())
		return false;
	const GiTraceTexelData& t = it->second;
	if (t.width == 0 || t.height == 0
		|| t.texels.size() < size_t(t.width) * size_t(t.height))
		return false;
	if (uvs.size() < positions.size() || indices.size() < size_t(tri) * 3 + 3)
		return false;
	const uint32_t i0 = indices[tri * 3 + 0];
	const uint32_t i1 = indices[tri * 3 + 1];
	const uint32_t i2 = indices[tri * 3 + 2];
	// Hit barycentrics: baryUV = (v, w); u = 1 - v - w (TriangleBvh::Hit).
	const float u = 1.0f - baryUV.x - baryUV.y;
	outUv = uvs[i0] * u + uvs[i1] * baryUV.x + uvs[i2] * baryUV.y;
	outData = &t;
	return true;
}

glm::vec4 GiTraceScene::SampleTexelBilinear(const GiTraceTexelData& t,
	glm::vec2 uv) const {
	// Bilinear, REPEAT wrap (glTF baseColor default; tiling walls are the
	// norm). A clamp-authored atlas tiles in the bake: known bake-side
	// approximation (the runtime's own repeat-defaulted sampling makes
	// the same choice for unconverted samplers). The caller resolved a
	// VALIDATED store (ResolveTexelAndUv checked dims + capacity).
	const float fx = uv.x * float(t.width) - 0.5f;
	const float fy = uv.y * float(t.height) - 0.5f;
	const int32_t x0 = int32_t(std::floor(fx));
	const int32_t y0 = int32_t(std::floor(fy));
	const float tx = fx - float(x0);
	const float ty = fy - float(y0);
	auto wrap = [&](int32_t v, uint32_t mod) {
		int32_t r = v % int32_t(mod);
		if (r < 0)
			r += int32_t(mod);
		return uint32_t(r);
	};
	const uint32_t w = t.width, h = t.height;
	const uint32_t xa = wrap(x0, w), xb = wrap(x0 + 1, w);
	const uint32_t ya = wrap(y0, h), yb = wrap(y0 + 1, h);
	const glm::vec4& t00 = t.texels[size_t(ya) * w + xa];
	const glm::vec4& t10 = t.texels[size_t(ya) * w + xb];
	const glm::vec4& t01 = t.texels[size_t(yb) * w + xa];
	const glm::vec4& t11 = t.texels[size_t(yb) * w + xb];
	const glm::vec4 a = glm::mix(t00, t10, tx);
	const glm::vec4 b = glm::mix(t01, t11, tx);
	return glm::mix(a, b, ty);
}

glm::vec3 GiTraceScene::SkyAmbient() const {
	// Fixed 16 upper-hemisphere directions (deterministic; the runtime's
	// analytic sky IBL ambient covers the same average). Cosine-weighted
	// mean: irradiance / pi == radiance mean for the Lambertian ambient.
	static const glm::vec3 kDirs[16] = {
		{ 0.38268, 0.92388, 0.0f }, { -0.38268, 0.92388, 0.0f },
		{ 0.0f, 0.92388, 0.38268 }, { 0.0f, 0.92388, -0.38268 },
		{ 0.59501, 0.70711, 0.38268 }, { -0.59501, 0.70711, 0.38268 },
		{ 0.59501, 0.70711, -0.38268 }, { -0.59501, 0.70711, -0.38268 },
		{ 0.92388, 0.38268, 0.0f }, { -0.92388, 0.38268, 0.0f },
		{ 0.0f, 0.38268, 0.92388 }, { 0.0f, 0.38268, -0.92388 },
		{ 0.77448, 0.44783, 0.44783 }, { -0.77448, 0.44783, 0.44783 },
		{ 0.77448, 0.44783, -0.44783 }, { -0.77448, 0.44783, -0.44783 },
	};
	glm::vec3 sum(0.0f);
	for (const glm::vec3& d : kDirs)
		sum += SkyRadiance(glm::normalize(d));
	return sum * (1.0f / 16.0f);
}

glm::vec3 GiTraceScene::TraceIncoming(const glm::vec3& origin, const glm::vec3& dir,
	Pcg32& rng, uint32_t maxDepth, float* outPrimaryDist,
	glm::vec3* outFirstAlbedo) const {
	// Baked field: sun + static spot NEE at every diffuse vertex
	// (including the primary hit — the light a lit surface reflects toward
	// the probe IS indirect at the probe), plus the analytic sky on miss
	// (SkyRadiance, gradient only, NO sun disc). Baking the sky makes
	// every baked source the COMPLETE ambient for its covered pixels —
	// the runtime gate (default_lit_body.hlsli) never adds the analytic
	// sky diffuse on top of a covered pixel (it is the fallback for
	// uncovered pixels only). This does not double-count the direct sun:
	// the cascaded shadow maps evaluate it at the shaded surface itself.
	// Transmissive surfaces pass rays through, tinting the throughput (no
	// bounce, no NEE at the pass-through vertex); emissive stays
	// runtime-only.
	glm::vec3 L(0.0f);
	glm::vec3 throughput(1.0f);
	glm::vec3 p = origin;
	glm::vec3 d = glm::normalize(dir);

	for (uint32_t depth = 0; depth <= maxDepth; ++depth) {
		TriangleBvh::Hit h;
		if (!bvh_.Raycast(p, d, kMaxRayDist, h)) {
			// Open sky: analytic sky IBL radiance along the ray
			// (throughput-tinted after transmissive pass-throughs).
			L += throughput * SkyRadiance(d);
			break;
		}
		// First-hit distance for the capture path (transmissive surfaces
		// count: they are geometry; see the header note).
		if (depth == 0 && outPrimaryDist)
			*outPrimaryDist = h.t;
		const glm::vec3 pos = p + d * h.t;
		glm::vec3 tn = bvh_.TriangleNormal(h.tri);
		const glm::vec3 n = glm::dot(tn, d) < 0.0f ? tn : -tn;
		const GiTraceMaterial& m = materials[triMaterial[h.tri]];
		// The VERTEX albedo (textured at the interpolated hit UV, or the
		// flat averaged color): every term below (the NEE direct, the
		// bounce throughput, the ambient-closure outFirstAlbedo) shades
		// the actual surface texel, not the material's average — the
		// "captures see textured albedo" contract.
		const glm::vec3 vertexAlbedo = AlbedoAt(m, h.tri, h.baryUV);
		// First non-transmissive hit's albedo: the ambient-closure blend
		// (the cubemap baker) keys on the visible surface's bounce.
		if (outFirstAlbedo && m.transmission.x + m.transmission.y + m.transmission.z <= 1e-4f) {
			*outFirstAlbedo = vertexAlbedo;
			outFirstAlbedo = nullptr; // only the first
		}

		// Transmissive surfaces: rays continue THROUGH the surface. Two
		// flavors share one branch (the "embree material representation"
		// upgrade), split by the EXPLICIT weave marker (an albedo
		// inference breaks exactly on the metallic-1 cloth: base x
		// (1-metallic) collapses to 0):
		//  - GLASS (Blend, weave false, albedo ~0): the pure pass-through
		//    attenuator — rays continue, tinted by the flat averaged
		//    baseColor; no diffuse bounce at this vertex (bake-side only;
		//    runtime cost unchanged). Rays never refract.
		//  - WEAVE CLOTH (MASK + maskTransmissive, weave true, albedo
		//    whatever the metallic leaves): the vertex SHADES like an
		//    opaque one (the sun/spot NEE at its textured albedo — 0 for
		//    the metallic-1 sponza banners: pure tinted pass-through, the
		//    metallic fabric's look) AND the ray passes on, tinted per
		//    texel by the fabric it passes (TransmissionAt; cutout holes
		//    below the alpha cutoff pass unobstructed — open air). The
		//    fabric's own bounce vertex is skipped (second-order next to
		//    its NEE look; the albedo+transmission split is a documented
		//    approximation, kMaskWeaveTransmittance).
		if (m.transmission.x + m.transmission.y + m.transmission.z > 1e-4f) {
			if (m.weave) {
				L += throughput * (SampleSun(pos, n, m, vertexAlbedo)
					+ SampleSpots(pos, n, m, vertexAlbedo));
				throughput *= TransmissionAt(m, h.tri, h.baryUV);
			} else {
				throughput *= m.transmission;
			}
			if (Luminance(throughput) < 1e-4f)
				break;
			p = pos + d * kSurfEps;
			continue;
		}

		// Diffuse vertex: sun + spot NEE (both carry the vertex albedo
		// and the Lambertian 1/pi; the runtime shader re-applies its local
		// albedo for the lightmap diffuse, matching the probe path).
		L += throughput * (SampleSun(pos, n, m, vertexAlbedo)
			+ SampleSpots(pos, n, m, vertexAlbedo));

		throughput *= vertexAlbedo;
		if (Luminance(throughput) < 1e-4f)
			break;
		d = CosineHemisphere(rng, n);
		p = pos + n * kSurfEps;
	}
	return L;
}

glm::vec3 GiTraceScene::TraceIrradiance(const glm::vec3& origin, const glm::vec3& n,
	Pcg32& rng, uint32_t sampleCount, uint32_t maxDepth) const {
	glm::vec3 sum(0.0f);
	const glm::vec3 p = origin + n * kSurfEps;
	for (uint32_t i = 0; i < sampleCount; ++i)
		sum += TraceIncoming(p, CosineHemisphere(rng, n), rng, maxDepth);
	// Cosine-hemisphere sampling: E = pi * mean(L).
	return sum * (kPi / float(std::max(sampleCount, 1u)));
}

// ---------------------------------------------------------------------------
// Cluster extraction
// ---------------------------------------------------------------------------

std::optional<glm::vec3> GiAverageKtxTexel(std::span<const uint8_t> ktxBytes) {
	// Decode on the CPU (libktx), average a small mip in linear space:
	// baked albedo/emissive/metallic should include texture content.
	if (ktxBytes.empty())
		return std::nullopt;
	ktxTexture2* raw = nullptr;
	if (ktxTexture2_CreateFromMemory(
			reinterpret_cast<const ktx_uint8_t*>(ktxBytes.data()),
			static_cast<ktx_size_t>(ktxBytes.size()),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw) != KTX_SUCCESS)
		return std::nullopt;
	struct Guard {
		ktxTexture2* t;
		~Guard() { if (t) ktxTexture2_Destroy(t); }
	} guard{ raw };
	if (ktxTexture2_NeedsTranscoding(raw)) {
		if (ktxTexture2_TranscodeBasis(raw, KTX_TTF_RGBA32, 0) != KTX_SUCCESS)
			return std::nullopt;
	}
	const ktx_uint32_t baseWidth = raw->baseWidth;
	const ktx_uint32_t baseHeight = std::max<ktx_uint32_t>(raw->baseHeight, 1);
	const ktx_uint32_t levels = std::max<ktx_uint32_t>(raw->numLevels, 1);
	ktx_uint32_t level = 0;
	while (level + 1 < levels && (baseWidth >> (level + 1)) > 64)
		++level; // pick a small mip so averaging is cheap
	const ktx_uint32_t w = std::max<ktx_uint32_t>(baseWidth >> level, 1);
	const ktx_uint32_t h = std::max<ktx_uint32_t>(baseHeight >> level, 1);
	ktx_size_t offset = 0;
	if (ktxTexture2_GetImageOffset(raw, level, 0, 0, &offset) != KTX_SUCCESS)
		return std::nullopt;
	const ktx_size_t rowPitch = ktxTexture_GetRowPitch(
		reinterpret_cast<ktxTexture*>(raw), level);
	const ktx_uint32_t components = ktxTexture2_GetNumComponents(raw);
	if (components < 3 || rowPitch < ktx_size_t(w) * components)
		return std::nullopt;
	const bool srgb = ktxTexture2_GetTransferFunction_e(raw) == KHR_DF_TRANSFER_SRGB;
	const uint8_t* base = reinterpret_cast<const uint8_t*>(raw->pData) + offset;
	glm::dvec3 sum(0.0);
	uint64_t count = 0;
	for (ktx_uint32_t y = 0; y < h; ++y) {
		const uint8_t* row = base + y * rowPitch;
		for (ktx_uint32_t x = 0; x < w; ++x) {
			const uint8_t* px = row + x * components;
			auto chan = [&](uint32_t c) {
				const float v = float(px[c]) / 255.0f;
				return srgb
					? ((v <= 0.04045f) ? v / 12.92f
						: std::pow((v + 0.055f) / 1.055f, 2.4f))
					: v;
			};
			sum += glm::dvec3(chan(0), chan(1), chan(2));
			++count;
		}
	}
	if (count == 0)
		return std::nullopt;
	return glm::vec3(sum / double(count));
}

std::optional<GiTraceTexelData> GiDecodeKtxTexelData(
	std::span<const uint8_t> ktxBytes, uint32_t maxDim) {
	// Same decode + acceptance rules as GiAverageKtxTexel (libktx CPU
	// decode; Basis transcoded to RGBA32), but keeps the full capped-res
	// texels (linear floats) instead of averaging them: the trace
	// scene's per-hit textured-albedo path (AlbedoAt) samples these at
	// the interpolated hit UV.
	if (ktxBytes.empty() || maxDim == 0)
		return std::nullopt;
	ktxTexture2* raw = nullptr;
	if (ktxTexture2_CreateFromMemory(
			reinterpret_cast<const ktx_uint8_t*>(ktxBytes.data()),
			static_cast<ktx_size_t>(ktxBytes.size()),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw) != KTX_SUCCESS)
		return std::nullopt;
	struct Guard {
		ktxTexture2* t;
		~Guard() { if (t) ktxTexture2_Destroy(t); }
	} guard{ raw };
	if (ktxTexture2_NeedsTranscoding(raw)) {
		if (ktxTexture2_TranscodeBasis(raw, KTX_TTF_RGBA32, 0) != KTX_SUCCESS)
			return std::nullopt;
	}
	const ktx_uint32_t baseWidth = raw->baseWidth;
	const ktx_uint32_t baseHeight = std::max<ktx_uint32_t>(raw->baseHeight, 1);
	const ktx_uint32_t levels = std::max<ktx_uint32_t>(raw->numLevels, 1);
	// The smallest mip that fits maxDim (mip 0 when the base fits —
	// never UPSCALE a small texture).
	ktx_uint32_t level = 0;
	while (level + 1 < levels && (baseWidth >> (level + 1)) > maxDim)
		++level;
	const ktx_uint32_t w = std::max<ktx_uint32_t>(baseWidth >> level, 1);
	const ktx_uint32_t h = std::max<ktx_uint32_t>(baseHeight >> level, 1);
	ktx_size_t offset = 0;
	if (ktxTexture2_GetImageOffset(raw, level, 0, 0, &offset) != KTX_SUCCESS)
		return std::nullopt;
	const ktx_size_t rowPitch = ktxTexture_GetRowPitch(
		reinterpret_cast<ktxTexture*>(raw), level);
	const ktx_uint32_t components = ktxTexture2_GetNumComponents(raw);
	if (components < 3 || rowPitch < ktx_size_t(w) * components)
		return std::nullopt;
	const bool srgb = ktxTexture2_GetTransferFunction_e(raw) == KHR_DF_TRANSFER_SRGB;
	const uint8_t* base = reinterpret_cast<const uint8_t*>(raw->pData) + offset;
	GiTraceTexelData out;
	out.width = w;
	out.height = h;
	out.texels.resize(size_t(w) * size_t(h));
	size_t k = 0;
	for (ktx_uint32_t y = 0; y < h; ++y) {
		const uint8_t* row = base + y * rowPitch;
		for (ktx_uint32_t x = 0; x < w; ++x, ++k) {
			const uint8_t* px = row + x * components;
			auto chan = [&](uint32_t c) {
				const float v = float(px[c]) / 255.0f;
				return srgb
					? ((v <= 0.04045f) ? v / 12.92f
						: std::pow((v + 0.055f) / 1.055f, 2.4f))
					: v;
			};
			// RGB linear-converted; alpha RAW (0..1, no transfer) — the
			// MASK-cutout transmission test (weave cloth holes). RGB-only
			// sources decode alpha 1 (fully covered).
			out.texels[k] = glm::vec4(chan(0), chan(1), chan(2),
				components >= 4 ? float(px[3]) / 255.0f : 1.0f);
		}
	}
	return out;
}

GiStaticLightCounts CollectStaticBakeLights(const Cluster& cluster, GiTraceScene& out,
	const glm::vec3& aimAxis) {
	const ::Scene& scene = cluster.scene;
	GiStaticLightCounts counts{};

	// Deterministic node order (hash-map iteration is not).
	std::vector<uint32_t> nodes;
	nodes.reserve(scene.lightForNode.size());
	for (const auto& [node, lightIdx] : scene.lightForNode)
		nodes.push_back(node);
	std::sort(nodes.begin(), nodes.end());

	for (uint32_t node : nodes) {
		const auto lit = scene.lightForNode.find(node);
		if (lit == scene.lightForNode.end())
			continue;
		if (node >= scene.globalTransform.size())
			continue;
		const ClusterLightHandle handle = cluster.lights.getHandle(lit->second);
		if (!handle.valid())
			continue;
		const SceneLight* lp = cluster.lights.get(handle);
		if (!lp || (lp->flags & SceneLight::kSceneLightFlag_Static) == 0)
			continue; // runtime-only light: never baked
		const SceneLight& L = *lp;
		const glm::mat4& G = scene.globalTransform[node];
		// Aim: the glTF -Z axis (aimAxis) mapped to world by the node's
		// global transform — raw locals (the coordFix basis rides the
		// imported chain's top node), so the plain -Z read is exact.
		// Same convention as the runtime resolved-light path
		// (ResolveSceneLights).
		glm::vec3 emis = glm::vec3(G * glm::vec4(aimAxis, 0.0f));
		const glm::vec3 pos = glm::vec3(G[3]);

		if (L.type == SceneLight::Type::Directional) {
			// First static directional seeds the bake sun (lux units,
			// direction TO the sun — mirrors the runtime env seeding).
			if (counts.suns > 0)
				continue;
			if (glm::dot(emis, emis) < 1e-6f)
				continue;
			out.sunDirection = -glm::normalize(emis);
			out.sunColor = L.color;
			out.sunIntensity = L.intensity;
			++counts.suns;
		}
		else if (L.type == SceneLight::Type::Spot) {
			if (glm::dot(emis, emis) < 1e-6f)
				continue;
			GiTraceSpot s{};
			s.position = pos;
			s.direction = glm::normalize(emis); // TO the aim point
			s.range = L.range;                  // 0 = infinite
			s.color = L.color * L.intensity;    // candela -> radiance scale
			s.cosInner = std::cos(std::min(L.innerConeAngle, L.outerConeAngle));
			s.cosOuter = std::cos(std::max(L.innerConeAngle, L.outerConeAngle));
			out.spots.push_back(s);
			++counts.spots;
		}
		else if (L.type == SceneLight::Type::Point) {
			// GiTraceSpot is cone-only: a point light is a 180-degree
			// spot. cosInner == cosOuter == -1 makes the cone falloff
			// evaluate to 1 in SampleSpots (except the measure-zero
			// exact back axis), leaving the Frostbite windowed
			// inverse-square term — exactly the runtime point path.
			GiTraceSpot s{};
			s.position = pos;
			s.direction = { 0.0f, -1.0f, 0.0f }; // unused (full sphere)
			s.range = L.range;
			s.color = L.color * L.intensity;
			s.cosInner = -1.0f;
			s.cosOuter = -1.0f;
			out.spots.push_back(s);
			++counts.points;
		}
	}
	return counts;
}

void BuildTraceSceneFromCluster(Cluster& cluster, const GiSceneBuildOptions& opts,
	GiTraceScene& out, std::string& err) {
	::Scene& scene = cluster.scene;
	recalculateGlobalTransforms(scene);

	out.positions.clear();
	out.indices.clear();
	out.materials.clear();
	out.triMaterial.clear();

	// Node-material pairs first so materials vector indices are stable.
	auto internMaterial = [&](const Material& mat) -> uint32_t {
		GiTraceMaterial gm{};
		// metallic = factor * MR-texture B (the runtime shader
		// convention: glTF metallicFactor defaults to 1 with the real
		// value in the metallicRoughness texture's blue channel).
		float metallic = std::clamp(mat.values.params.y, 0.0f, 1.0f);
		if (mat.values.textures.y >= 0) {
			if (auto it = opts.texelAvg.find(uint32_t(mat.values.textures.y));
				it != opts.texelAvg.end())
				metallic *= it->second.b;
		}
		metallic = std::clamp(metallic, 0.0f, 1.0f);
		glm::vec3 base = glm::vec3(mat.values.diffuse);
		if (mat.values.textures.x >= 0) {
			if (auto it = opts.texelAvg.find(uint32_t(mat.values.textures.x));
				it != opts.texelAvg.end())
				base *= it->second;
		}
		// The decodable baseColor texture id + the per-hit sampling slots
		// (shared by the albedo + the transmission paths below). The
		// textured ALBEDO slots only live outside white-world (the
		// uniform albedo is white-world's whole point); the textured
		// TRANSMISSION slots live in BOTH (the tint through the cloth
		// stays real in the GI bake - see GiSceneBuildOptions::
		// maskTransmissive).
		const uint32_t baseTexId = (mat.values.textures.x >= 0
			&& opts.texelData.find(uint32_t(mat.values.textures.x))
				!= opts.texelData.end())
			? uint32_t(mat.values.textures.x) : 0xFFFFFFFFu;
		const bool blendGlass = (mat.alphaMode == AlphaMode::Blend);
		const bool weaveCloth = opts.maskTransmissive
			&& (mat.alphaMode == AlphaMode::Mask);
		if (blendGlass) {
			// Transmissive (glass): the trace scene carries it as a
			// pass-through attenuator — rays continue through the
			// surface, tinted by baseColor (bake-side only; runtime
			// cost unchanged). No diffuse bounce for these.
			gm.transmission = base;
			gm.albedo = glm::vec3(0.0f);
			gm.emissive = glm::vec3(0.0f);
		} else {
			gm.albedo = base * (1.0f - metallic);
			gm.emissive = opts.includeEmissive
				? glm::vec3(mat.values.emissiveFactor) * mat.values.params2.z
				: glm::vec3(0.0f);
			if (opts.includeEmissive && mat.values.textures2.x >= 0) {
				if (auto it = opts.texelAvg.find(uint32_t(mat.values.textures2.x));
					it != opts.texelAvg.end())
					gm.emissive *= it->second;
			}
			// Textured path: a decodable baseColor texture with
			// decoded texel data switches the trace to per-hit
			// sampling (AlbedoAt interpolates the hit UV + samples
			// bilinearly). baseTexMul = baseColor * (1 - metallic)
			// WITHOUT the average (the average only tints the flat
			// fallback albedo).
			if (!opts.uniformAlbedo && baseTexId != 0xFFFFFFFFu) {
				gm.baseTexId = baseTexId;
				gm.baseTexMul = glm::vec3(mat.values.diffuse)
					* (1.0f - metallic);
			}
			if (weaveCloth) {
				// Weave-transmissive cloth (MASK + maskTransmissive): the
				// fabric keeps its albedo above (WHATEVER the metallic
				// leaves - the sponza banners' MR-B averages 1.0, so
				// their albedo is 0: pure tinted pass-through, the
				// metallic fabric's look; the marker carries the
				// weave-ness, never the albedo) and passes light through
				// the weave, tinted per texel (kMaskWeaveTransmittance
				// is the documented energy approximation). The cutout
				// holes (alpha < the material cutoff) pass unobstructed.
				gm.weave = true;
				gm.transmission = base * kMaskWeaveTransmittance;
				gm.transAlphaCutoff = mat.alphaCutoff;
				if (baseTexId != 0xFFFFFFFFu) {
					gm.transTexId = baseTexId;
					gm.transTexMul = glm::vec3(mat.values.diffuse)
						* kMaskWeaveTransmittance;
				}
			}
		}
		if (opts.uniformAlbedo) {
			// White-world mode: one known diffuse albedo everywhere, no
			// emissive. Analytically checkable transport (debug) — but
			// the TRANSMISSION tint stays REAL (the glass's tint, the
			// cloth's weave tint: the tint through a surface is the
			// visible effect the bake wants, not its diffuse albedo;
			// whitening it would grey the tinted light pools).
			if (!blendGlass) {
				gm.albedo = opts.uniformAlbedoValue;
				gm.emissive = glm::vec3(0.0f);
			}
		}
		// Deduplicate identical materials (cheap win: scenes reuse them).
		// The textured slots compare ids AND muls/cutoff — two materials
		// sharing a texture id with different diffuse factors (or cutoffs)
		// are NOT the same material.
		for (uint32_t i = 0; i < out.materials.size(); ++i) {
			const GiTraceMaterial& o = out.materials[i];
			if (glm::all(glm::equal(o.albedo, gm.albedo))
				&& glm::all(glm::equal(o.emissive, gm.emissive))
				&& glm::all(glm::equal(o.transmission, gm.transmission))
				&& o.weave == gm.weave
				&& o.baseTexId == gm.baseTexId
				&& glm::all(glm::equal(o.baseTexMul, gm.baseTexMul))
				&& o.transTexId == gm.transTexId
				&& glm::all(glm::equal(o.transTexMul, gm.transTexMul))
				&& o.transAlphaCutoff == gm.transAlphaCutoff)
				return i;
		}
		out.materials.push_back(gm);
		return uint32_t(out.materials.size() - 1);
	};

	struct NodeGeo {
		uint32_t node;
		uint32_t materialIdx;
		const StaticMeshUpload* upload;
		glm::mat4 global;
	};
	std::vector<NodeGeo> geos;

	for (const auto& [node, meshIdx] : scene.meshForNode) {
		const ClusterStaticMeshHandle meshHandle = cluster.staticMeshes.getHandle(meshIdx);
		if (!meshHandle.valid()) {
			err = "BuildTraceSceneFromCluster: stale mesh handle";
			return;
		}
		const StaticMeshUpload* upload = cluster.GetRetainedUpload(meshHandle);
		if (!upload || upload->vertices.empty() || upload->indices.empty())
			continue;

		auto matIt = scene.materialForNode.find(node);
		if (matIt == scene.materialForNode.end())
			continue;
		const ClusterMaterialHandle matHandle = cluster.materials.getHandle(matIt->second);
		if (!matHandle.valid())
			continue;
		const Material* mat = cluster.materials.get(matHandle);
		if (!mat)
			continue;
		if (!opts.includeMaskedCutout && mat->alphaMode == AlphaMode::Mask)
			continue;
		// Decal overlays (cook-stamped no-cast on non-MASK materials):
		// coplanar quads that would shadow/tint the surface behind them.
		// MASK no-cast (cutout on a decal atlas) stays — volume approximation.
		if (opts.excludeDecalOverlays && !mat->castsShadows
			&& mat->alphaMode != AlphaMode::Mask)
			continue;
		// Blend (transmissive) geometry is NOT skipped: the trace scene
		// carries it as a pass-through attenuator (see internMaterial).
		// Lightmap charting still excludes it (glass gets no charts).

		NodeGeo g{};
		g.node = node;
		g.upload = upload;
		g.global = scene.globalTransform[node];
		g.materialIdx = internMaterial(*mat);
		geos.push_back(g);
	}

	if (geos.empty()) {
		err = "BuildTraceSceneFromCluster: no renderable geometry (cook-mode cluster with retained uploads required)";
		return;
	}

	uint32_t triCount = 0;
	for (const NodeGeo& g : geos)
		triCount += uint32_t(g.upload->indices.size() / 3);
	out.positions.reserve(size_t(triCount) * 3);
	out.indices.reserve(size_t(triCount) * 3);
	out.triMaterial.reserve(triCount);

	// The textured-albedo data rides along: per-vertex UVs (only when at
	// least one interned material actually samples a texture — a no-texture
	// bake keeps the soup lean) + the decoded texel store for the USED
	// baseColor ids only (opts is const; the unused decodes — emissive/
	// metallic slots and materials dropped from the soup — never copy).
	bool anyTextured = false;
	for (const GiTraceMaterial& m : out.materials) {
		const uint32_t ids[2] = { m.baseTexId, m.transTexId };
		for (uint32_t id : ids) {
			if (id == 0xFFFFFFFFu)
				continue;
			anyTextured = true;
			if (auto it = opts.texelData.find(id); it != opts.texelData.end())
				out.textures.emplace(id, it->second);
		}
	}
	if (anyTextured)
		out.uvs.reserve(size_t(triCount) * 3);

	for (const NodeGeo& g : geos) {
		const uint32_t base = uint32_t(out.positions.size());
		for (const ImportStaticVertex& v : g.upload->vertices) {
			const glm::vec4 wp = g.global * glm::vec4(v.pos, 1.0f);
			out.positions.push_back(glm::vec3(wp));
			if (anyTextured)
				out.uvs.push_back(v.texCoord);
		}
		for (uint32_t k = 0; k < g.upload->indices.size(); ++k)
			out.indices.push_back(base + g.upload->indices[k]);
		for (uint32_t t = 0; t < g.upload->indices.size() / 3; ++t)
			out.triMaterial.push_back(g.materialIdx);
	}

	// Environment from the runtime settings (same defaults as the app UI).
	const EnvironmentSettings& env = opts.env;

	out.sunDirection = env.SunDirectionToSun();
	out.sunColor = env.sunColor;
	out.sunIntensity = env.sunIntensity;
	out.skyZenith = env.skyZenithColor;
	out.skyHorizon = env.skyHorizonColor;
	out.skyGround = env.groundColor;
	out.skyIntensity = env.skyIntensity;
	out.skyExponent = env.skyExponent;

	// Scene-authored static lights (kSceneLightFlag_Static): bake-only
	// sources — spots/points join the NEE list, the first static
	// directional overrides the env-seeded sun. Callers (PakCooker /
	// GiBaker) append their CLI spots AFTER this, so both sets bake.
	if (opts.staticLights)
		CollectStaticBakeLights(cluster, out, opts.punctualAimAxis);

	out.SetTraceBackend(opts.traceBackendEmbree ? TraceBackend::Embree : TraceBackend::Builtin);
	out.Finalize();
}


// ---------------------------------------------------------------------------
// The ADAPTIVE OCTREE BAKE (bricks, relocation, refinement waves,
// BakeSgGiVolume) is DELETED — stage 3 (data layer) of the legacy-.sggi
// retirement (tourmaline_octree_retirement_plan.md). What remains in this
// file is the SHARED trace substrate every live bake consumes: the
// GiTraceScene (transport, sky, emissive, sun, transmission, albedo
// sampling), the KTX texel decode helpers, and BuildTraceSceneFromCluster.
// The live baked-GI products (convex .givolume grids, regular lattices,
// lightmap atlases) each own their bake in their own TU
// (cubemap_volume.cpp / regular_volume.cpp / lightmap.cpp).
// ---------------------------------------------------------------------------

} // namespace smvk::gi
