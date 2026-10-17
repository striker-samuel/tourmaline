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
	const float ndl = glm::dot(n, sunDirection);
	if (ndl <= 0.0f)
		return glm::vec3(0.0f);
	if (bvh_.AnyHit(pos + n * kSurfEps, sunDirection, kMaxRayDist))
		return glm::vec3(0.0f);
	return sunColor * (sunIntensity * ndl) * m.albedo * (1.0f / kPi);
}

glm::vec3 GiTraceScene::SampleSpots(const glm::vec3& pos, const glm::vec3& n,
	const GiTraceMaterial& m) const {
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
	return sum * m.albedo * (1.0f / kPi);
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
		// First non-transmissive hit's albedo: the ambient-closure blend
		// (the cubemap baker) keys on the visible surface's bounce.
		if (outFirstAlbedo && m.transmission.x + m.transmission.y + m.transmission.z <= 1e-4f) {
			*outFirstAlbedo = m.albedo;
			outFirstAlbedo = nullptr; // only the first
		}

		// Transmissive (glass): pass through, tinted. No diffuse bounce at
		// this vertex; the throughput picks up the transmission color.
		// Rays never refract (straight-through) — bake-side approximation.
		if (m.transmission.x + m.transmission.y + m.transmission.z > 1e-4f) {
			throughput *= m.transmission;
			if (Luminance(throughput) < 1e-4f)
				break;
			p = pos + d * kSurfEps;
			continue;
		}

		// Diffuse vertex: sun + spot NEE (both carry the vertex albedo
		// and the Lambertian 1/pi; the runtime shader re-applies its local
		// albedo for the lightmap diffuse, matching the probe path).
		L += throughput * (SampleSun(pos, n, m) + SampleSpots(pos, n, m));

		throughput *= m.albedo;
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
		if (opts.uniformAlbedo) {
			// White-world mode: one known albedo everywhere, no emissive,
			// no transmission. Analytically checkable transport (debug).
			gm.albedo = opts.uniformAlbedoValue;
			gm.emissive = glm::vec3(0.0f);
		} else {
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
			if (mat.alphaMode == AlphaMode::Blend) {
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
			}
		}
		// Deduplicate identical materials (cheap win: scenes reuse them).
		for (uint32_t i = 0; i < out.materials.size(); ++i) {
			const GiTraceMaterial& o = out.materials[i];
			if (glm::all(glm::equal(o.albedo, gm.albedo))
				&& glm::all(glm::equal(o.emissive, gm.emissive))
				&& glm::all(glm::equal(o.transmission, gm.transmission)))
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

	for (const NodeGeo& g : geos) {
		const uint32_t base = uint32_t(out.positions.size());
		for (const StaticVertex& v : g.upload->vertices) {
			const glm::vec4 wp = g.global * glm::vec4(v.pos, 1.0f);
			out.positions.push_back(glm::vec3(wp));
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
// Bake: bricks, relocation, probes, refinement waves
// ---------------------------------------------------------------------------

namespace {

struct BakeBrick {
	glm::vec3 center{};
	float size = 0.0f;
	uint32_t level = 0;
	bool hasProbe = false;
	bool subdivided = false;
	bool nearSurface = false; // geometry-stage classification (transient)
	glm::vec3 probePos{};
	float error = 0.0f;
	glm::vec3 amps[kSgLobeCount]{};
	float vis[6] = {};
};


// Pushes a probe candidate into free space: first away from the closest
// surface point, then a fixed direction sweep. Candidates stay inside the
// brick (the probe must keep representing it).
bool RelocateProbe(const TriangleBvh& bvh, const BakeBrick& brick,
	float clearance, glm::vec3& outPos) {
	const glm::vec3& center = brick.center;
	const float half = brick.size * 0.5f;
	auto clamped = [&](const glm::vec3& p) {
		return glm::clamp(p, center - half, center + half);
	};

	glm::vec3 best = center;
	float bestDist = bvh.ClosestDistance(center);
	if (bestDist >= clearance) {
		outPos = best;
		return true;
	}

	// March away from the closest surface point.
	{
		glm::vec3 cp;
		bvh.ClosestDistance(center, &cp);
		glm::vec3 dir = center - cp;
		const float len = glm::length(dir);
		dir = len > 1e-5f ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f);
		const float step = std::max(brick.size * 0.25f, 1e-3f);
		for (float t = step; t <= half * 2.0f + 1e-6f; t += step) {
			const glm::vec3 cand = clamped(center + dir * t);
			const float d = bvh.ClosestDistance(cand);
			if (d > bestDist) {
				bestDist = d;
				best = cand;
				if (d >= clearance)
					break;
			}
		}
	}
	if (bestDist >= clearance) {
		outPos = best;
		return true;
	}

	// Direction sweep: 6 axes + 8 cube diagonals (inside-solid rescue).
	static const glm::vec3 kDirs[14] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
		glm::normalize(glm::vec3(1, 1, 1)), glm::normalize(glm::vec3(1, 1, -1)),
		glm::normalize(glm::vec3(1, -1, 1)), glm::normalize(glm::vec3(1, -1, -1)),
		glm::normalize(glm::vec3(-1, 1, 1)), glm::normalize(glm::vec3(-1, 1, -1)),
		glm::normalize(glm::vec3(-1, -1, 1)), glm::normalize(glm::vec3(-1, -1, -1)),
	};
	const float step = std::max(brick.size * 0.25f, 1e-3f);
	for (const glm::vec3& dir : kDirs) {
		for (float t = step; t <= half * 2.0f + 1e-6f; t += step) {
			const glm::vec3 cand = clamped(center + dir * t);
			const float d = bvh.ClosestDistance(cand);
			if (d > bestDist) {
				bestDist = d;
				best = cand;
			}
		}
		if (bestDist >= clearance)
			break;
	}
	// Trapped probes are DROPPED, not soft-accepted: a probe hugging
	// geometry bakes dark (occluded) radiance and records near-zero
	// visibility distances, rejecting everything around itself — dark
	// polka dots. Leaving the region to its neighbors is strictly better.
	if (bestDist >= std::max(0.03f, brick.size * 0.1f)) {
		outPos = best;
		return true;
	}
	return false;
}

// Traces + fits one probe (radiance fit + 6-axis visibility + reconstruction
// error at off-center test points). `dirs` is the SHARED deterministic
// direction set (Fibonacci sphere): every probe samples the same
// directions, so neighboring probes' estimation errors are correlated
// (a smooth spatial bias) instead of independent per-probe noise that
// the runtime blend shows as coverage-sized speckle.
void BakeProbeBrick(const GiTraceScene& scene, const SgGiBakeConfig& cfg,
	BakeBrick& brick, uint64_t seed, const std::vector<glm::vec3>& dirs,
	uint64_t& clampedSamples, uint64_t& nonFiniteSamples) {
	const float clearance = std::max(cfg.probeMinClearance,
		cfg.probeClearanceFactor * brick.size);
	if (!RelocateProbe(scene.Bvh(), brick, clearance, brick.probePos))
		return; // fully embedded: drop the leaf, neighbors cover
	brick.hasProbe = true;

	// Two-pass firefly filter (standard path-tracer GI practice): sun pools
	// make the incident field brutally contrasty (0 vs ~10^4 in one probe's
	// sample set) and the 9-fat-lobe fit turns that contrast into spiky
	// 10^4-scale lobe peaks that read as hard-edged colored dots. Clamp
	// each sample to 8x the probe's own mean (hue-preserving): smooth
	// bounce passes through untouched, only true outliers are tamed.
	struct RaySample { glm::vec3 dir; glm::vec3 L; };
	Pcg32 rng(seed);
	std::vector<RaySample> samples;
	samples.reserve(dirs.size());
	double meanLum = 0.0;
	uint32_t finiteSamples = 0;
	for (uint32_t i = 0; i < dirs.size(); ++i) {
		const glm::vec3 L = scene.TraceIncoming(brick.probePos, dirs[i], rng, cfg.maxBounces);
		samples.push_back({ dirs[i], L });
		// A non-finite sample must never enter the firefly-cap mean: a
		// NaN mean makes cap NaN and silently DISARMS the clamp for the
		// whole probe (every l > cap comparison is false). The sample
		// itself still reaches acc.AddSample, which quarantines it (the
		// fit is safe either way — SgFitAccumulator::AddSample).
		if (!std::isfinite(L.x + L.y + L.z)) {
			++nonFiniteSamples;
			continue;
		}
		meanLum += Luminance(L);
		++finiteSamples;
	}
	meanLum /= double(std::max(finiteSamples, 1u));
	const float cap = std::max(float(meanLum) * 8.0f, 1e-6f);
	SgFitAccumulator acc;
	const float weight = 4.0f * kPi / float(dirs.size());
	for (const RaySample& s : samples) {
		glm::vec3 L = s.L;
		const float l = Luminance(L);
		if (l > cap && l > 0.0f) {
			L *= cap / l;
			++clampedSamples;
		}
		acc.AddSample(s.dir, L, weight);
	}
	acc.Solve(brick.amps);
	// NOTE: ringing on sun-pool edges is handled inside Solve (diagonal
	// fallback); no post-hoc peak squashing here — it would bias honest
	// directional probes toward uniform grey.

	// 6-axis visibility (obstacle distances, capped).
	static const glm::vec3 kAxes[6] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
	};
	for (uint32_t k = 0; k < 6; ++k) {
		TriangleBvh::Hit h;
		// 0 = no known obstacle (unobstructed): both consumers skip
		// vis == 0 axes. NEVER store the trace cutoff as a distance —
		// that reads as a phantom wall at maxDistance and rejects
		// everything beyond it (coarse probes with R >> maxDistance
		// go blind past 2.5 m and force fallback blends).
		float d = 0.0f;
		if (scene.Bvh().Raycast(brick.probePos, kAxes[k], cfg.visibilityMaxDistance, h))
			d = std::max(h.t, 1e-3f);
		brick.vis[k] = d;
	}

	// Error-driven refinement metric: reference irradiance at off-center
	// test points vs the probe's own SG reconstruction. Measures combined
	// spatial + angular error inside the brick.
	static const glm::vec3 kTestOffsets[2] = {
		{ 0.29f, 0.11f, -0.23f },
		{ -0.17f, -0.21f, 0.31f },
	};
	static const glm::vec3 kTestNormals[3] = {
		{ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
	};
	const float step = std::max(brick.size * 0.125f, 1e-3f);
	float errSum = 0.0f;
	float refLumSum = 0.0f;
	int refCount = 0;
	glm::vec3 refs[6];
	for (int t = 0; t < 2; ++t) {
		const glm::vec3 tp = brick.probePos + kTestOffsets[t] * brick.size;
		for (int nn = 0; nn < 3; ++nn) {
			// Nudge test points out of solid contact (they may sit inside a
			// wall the probe escaped): trace from slightly along the normal.
			const glm::vec3 n = kTestNormals[nn];
			glm::vec3 origin = tp;
			if (scene.Occluded(origin, -n, step))
				origin = tp + n * step;
			const glm::vec3 ref = scene.TraceIrradiance(
				origin, n, rng, cfg.errorRaysPerProbe, cfg.maxBounces);
			refs[t * 3 + nn] = ref;
			refLumSum += Luminance(ref);
			++refCount;
		}
	}
	const float floor = cfg.errorFloor * std::max(refLumSum / float(std::max(refCount, 1)), 1e-5f);
	int validPoints = 0;
	for (int i = 0; i < 6; ++i) {
		const glm::vec3& ref = refs[i];
		// Non-finite references carry no information; counting them as
		// error subdivides garbage forever (NaN <= tolerance is false).
		if (!std::isfinite(ref.x + ref.y + ref.z))
			continue;
		const glm::vec3 rec = SgDiffuseIrradiance(brick.amps, kTestNormals[i % 3]);
		if (!std::isfinite(rec.x + rec.y + rec.z))
			continue;
		errSum += Luminance(glm::abs(ref - rec)) / std::max(Luminance(ref), floor);
		++validPoints;
	}
	brick.error = validPoints > 0 ? errSum / float(validPoints) : 0.0f;
}

} // namespace

bool BakeSgGiVolume(const GiTraceScene& scene, const SgGiBakeConfig& configIn,
	SgGiVolume& outVolume, SgGiBakeStats& stats, GiBakeProgress progress) {
	const auto t0 = std::chrono::steady_clock::now();
	SgGiBakeConfig cfg = configIn;
	cfg.raysPerProbe = std::max(cfg.raysPerProbe, 16u);
	cfg.maxBounces = std::min(cfg.maxBounces, 8u);
	cfg.errorRaysPerProbe = std::max(cfg.errorRaysPerProbe, 8u);
	cfg.rootBrickSize = std::max(cfg.rootBrickSize, cfg.minBrickSize);
	cfg.maxProbes = std::max(cfg.maxProbes, 64u);
	cfg.visibilityMaxDistance = std::max(cfg.visibilityMaxDistance, 0.05f);

	auto say = [&](const char* line) {
		if (progress)
			progress(line);
	};

	// ------------------------------------------------------------------
	// Root bricks over the scene bounds
	// ------------------------------------------------------------------
	glm::vec3 bmin(1e30f), bmax(-1e30f);
	{
		glm::vec3 v0, v1, v2;
		for (uint32_t k = 0; k < scene.Bvh().TriangleCount(); ++k) {
			scene.Bvh().TriangleVertices(k, v0, v1, v2);
			bmin = glm::min(bmin, glm::min(v0, glm::min(v1, v2)));
			bmax = glm::max(bmax, glm::max(v0, glm::max(v1, v2)));
		}
		if (scene.Bvh().TriangleCount() == 0) {
			if (progress)
				progress("bake: empty scene\n");
			return false;
		}
		bmin -= cfg.rootBrickSize * 0.25f;
		bmax += cfg.rootBrickSize * 0.25f;
	}

	std::vector<BakeBrick> bricks;
	// Geometry refinement (distance-to-surface heuristic): BREADTH-FIRST,
	// one octree level at a time, so every region reaches the same depth
	// before any region gets finer. The depth-first predecessor granted
	// fine probes to whichever region the recursion visited FIRST — with
	// a probe budget it starved the late-processed regions (whole streets
	// stranded on coarse L0-only probes). The geometry stage spends at
	// most kGeometryBudgetShare of the budget; the error-driven waves own
	// the rest and push deeper where lighting actually varies.
	{
		const float rs = cfg.rootBrickSize;
		glm::vec3 lo(std::floor(bmin.x / rs), std::floor(bmin.y / rs), std::floor(bmin.z / rs));
		glm::vec3 hi(std::floor(bmax.x / rs), std::floor(bmax.y / rs), std::floor(bmax.z / rs));
		for (float x = lo.x; x <= hi.x; ++x)
			for (float y = lo.y; y <= hi.y; ++y)
				for (float z = lo.z; z <= hi.z; ++z) {
					BakeBrick b{};
					b.center = glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f) * rs;
					b.size = rs;
					b.level = 0;
					bricks.push_back(b);
				}
		uint32_t leafCount = uint32_t(bricks.size());
		const uint32_t geometryBudget =
			std::max<uint32_t>(64u, uint32_t(float(cfg.maxProbes) * 0.6f));
		for (uint32_t level = 0; level + 1 < kSgGiMaxLevels; ++level) {
			const float size = cfg.rootBrickSize / float(1u << level);
			if (size * 0.5f + 1e-6f < cfg.minBrickSize)
				break; // finest level reached everywhere
			// Near-surface classification for this level's leaves
			// (conservative: center distance minus the half diagonal).
			const float halfDiag = size * 0.8660254f;
			uint32_t candidates = 0;
			for (BakeBrick& b : bricks) {
				if (b.subdivided || b.level != level || b.nearSurface)
					continue;
				const float dist = scene.Bvh().ClosestDistance(b.center) - halfDiag;
				b.nearSurface = (dist < size * cfg.nearSurfaceFactor);
				if (b.nearSurface)
					++candidates;
			}
			if (candidates == 0)
				break; // nothing near geometry at this depth
			// Budget gate: subdividing all candidates must keep the level's
			// leaf count inside the geometry share — otherwise EVERY region
			// stays at this depth (spatially fair), and only the error
			// waves may go deeper.
			if (leafCount + 7u * candidates > geometryBudget)
				break;
			for (uint32_t i = 0; i < bricks.size(); ++i) {
				BakeBrick& b = bricks[i];
				if (b.subdivided || b.level != level || !b.nearSurface)
					continue;
				// Copy before the push_backs below: bricks reallocates.
				const glm::vec3 center = b.center;
				const uint32_t nextLevel = level + 1;
				b.subdivided = true;
				b.hasProbe = false; // placeholder; children are the leaves now
				const float half = size * 0.5f;
				const float q = size * 0.25f;
				for (int sx = -1; sx <= 1; sx += 2) {
					for (int sy = -1; sy <= 1; sy += 2) {
						for (int sz = -1; sz <= 1; sz += 2) {
							BakeBrick c{};
							c.center = center + glm::vec3(sx, sy, sz) * q;
							c.size = half;
							c.level = nextLevel;
							bricks.push_back(c);
						}
					}
				}
			}
			leafCount += 7u * candidates;
		}
	}
	stats.bricksBuilt = uint32_t(bricks.size());
	{
		char line[160];
		std::snprintf(line, sizeof(line),
			"bake: %u bricks after geometry refinement\n", stats.bricksBuilt);
		say(line);
	}

	// ------------------------------------------------------------------
	// Waves: bake leaves, subdivide high-error bricks, repeat
	// ------------------------------------------------------------------
	std::vector<uint32_t> pending;
	for (uint32_t i = 0; i < bricks.size(); ++i)
		if (!bricks[i].subdivided)
			pending.push_back(i);

	const uint32_t threadCount = cfg.threadCount ? cfg.threadCount : std::thread::hardware_concurrency();
	ThreadPool pool(threadCount ? threadCount : 1);

	// Shared deterministic direction set (Fibonacci sphere): every probe
	// samples the SAME directions, so estimation error is spatially
	// correlated (smooth bias) instead of independent per-probe noise.
	// Independent random spheres measured 24% median / 83% p90 neighbor
	// disagreement — coverage-sized speckle at runtime.
	std::vector<glm::vec3> fibDirs;
	fibDirs.reserve(cfg.raysPerProbe);
	for (uint32_t i = 0; i < cfg.raysPerProbe; ++i) {
		const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(cfg.raysPerProbe);
		const float r = SafeSqrt(std::max(0.0f, 1.0f - y * y));
		const float phi = float(i) * 2.39996323f;
		fibDirs.push_back(glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r));
	}

	// Bakes a set of leaves in parallel (deterministic per-brick seeds keep
	// results independent of task scheduling). Returns the live-probe count.
	auto bakeLeaves = [&](std::vector<uint32_t>& leaves) -> uint64_t {
		std::atomic<uint64_t> baked{ 0 };
		std::atomic<uint64_t> clamped{ 0 };
		std::atomic<uint64_t> nonfinite{ 0 };
		constexpr size_t kBatch = 512;
		for (size_t base = 0; base < leaves.size(); base += kBatch) {
			const size_t end = std::min(base + kBatch, leaves.size());
			std::vector<std::future<void>> futures;
			futures.reserve(end - base);
			for (size_t k = base; k < end; ++k) {
				const uint32_t idx = leaves[k];
				futures.push_back(pool.enqueue([&, idx] {
					const uint64_t seed = (uint64_t(cfg.seed) << 32)
						^ (uint64_t(idx) * 0x9e3779b97f4a7c15ull);
					uint64_t localClamped = 0;
					uint64_t localNonFinite = 0;
					BakeProbeBrick(scene, cfg, bricks[idx], seed, fibDirs,
						localClamped, localNonFinite);
					clamped += localClamped;
					nonfinite += localNonFinite;
					if (bricks[idx].hasProbe)
						++baked;
				}));
			}
			for (auto& f : futures)
				f.get();
		}
		stats.pathsTraced += uint64_t(leaves.size())
			* (cfg.raysPerProbe + uint64_t(6) * cfg.errorRaysPerProbe);
		stats.samplesClamped += clamped.load();
		stats.samplesNonFinite += nonfinite.load();
		return baked.load();
	};

	uint32_t wave = 0;
	while (!pending.empty()) {
		const uint64_t baked = bakeLeaves(pending);
		{
			char line[160];
			std::snprintf(line, sizeof(line),
				"bake: wave %u: %zu leaves, %llu probes\n",
				wave, pending.size(), (unsigned long long)baked);
			say(line);
		}

		// Subdivide high-error leaves: the parent probe is replaced by
		// (up to) 8 children, which the next wave bakes.
		std::vector<uint32_t> next;
		uint32_t liveProbes = 0;
		for (const auto& b : bricks)
			if (b.hasProbe)
				++liveProbes;
		for (uint32_t idx : pending) {
			BakeBrick& b = bricks[idx];
			if (!b.hasProbe)
				continue;
			if (b.error <= cfg.errorTolerance)
				continue;
			if (b.level + 1 >= kSgGiMaxLevels || b.size * 0.5f + 1e-6f < cfg.minBrickSize)
				continue;
			if (liveProbes + 8 > cfg.maxProbes) {
				++stats.probeBudgetHits;
				continue;
			}
			// Copy before the push_backs below: bricks reallocates and
			// would dangle the reference.
			const glm::vec3 center = b.center;
			const float size = b.size;
			const uint32_t level = b.level;
			b.subdivided = true;
			b.hasProbe = false;
			liveProbes -= 1;
			const float half = size * 0.5f;
			const float q = size * 0.25f;
			for (int sx = -1; sx <= 1; sx += 2) {
				for (int sy = -1; sy <= 1; sy += 2) {
					for (int sz = -1; sz <= 1; sz += 2) {
						BakeBrick c{};
						c.center = center + glm::vec3(sx, sy, sz) * q;
						c.size = half;
						c.level = level + 1;
						next.push_back(uint32_t(bricks.size()));
						bricks.push_back(c);
						++liveProbes;
					}
				}
			}
		}
		if (next.empty())
			break;
		// Wave budget spent: bake the children once so the freshly created
		// leaves still have probes, then stop refining.
		if (wave + 1 >= std::min(cfg.maxRefineWaves, kSgGiMaxLevels)) {
			const uint64_t finalBaked = bakeLeaves(next);
			(void)finalBaked;
			{
				char line[160];
				std::snprintf(line, sizeof(line),
					"bake: final wave: %zu leaves, %llu probes\n",
					next.size(), (unsigned long long)finalBaked);
				say(line);
			}
			break;
		}
		pending = std::move(next);
		++wave;
	}
	stats.refineWaves = wave + 1;

	// ------------------------------------------------------------------
	// Diffusion procedure: the brick lattice is a relaxation FIXED POINT
	// (grid-aligned interpolation reads as polka dots) and per-probe MC
	// noise makes every probe its own colored patch (cyan sky probes,
	// warm bounce probes, black occluded probes). Two diffusive passes:
	//
	//  1. PLACEMENT diffusion: jitter each probe inside its brick to
	//     break the lattice symmetry, then relax with same-level
	//     repulsion for a few iterations — probes spread like particles
	//     diffusing to an even (blue-noise-ish) spacing, and the free
	//     space constraint keeps them out of geometry. The rigid grid
	//     pattern dissolves into an organic distribution.
	//  2. RADIANCE diffusion: a few Laplacian (heat equation) passes over
	//     the same-level neighbor graph — neighboring probes exchange
	//     amplitude mass until the field is spatially smooth, wiping the
	//     independent per-probe sampling noise the interpolation would
	//     otherwise show as dots.
	//
	//     NOTE: both stages are OFF by default now (diffusionIterations =
	//     smoothingPasses = 0). Moving probes after baking invalidates the
	//     baked radiance (fatal near arches/openings), and smoothing
	//     without a visibility criterion leaks bright across walls. Both
	//     stay available as explicit opt-in bake flags for experiments.
	// ------------------------------------------------------------------
	if (cfg.diffusionIterations > 0 || cfg.smoothingPasses > 0) {
		// Collect live probes per level with a uniform neighbor grid
		// (cell = brick size of that level).
		struct LevelCloud {
			std::vector<uint32_t> probeBricks; // indices into `bricks`
			glm::vec3 gmin{ 0.0f };
			uint32_t dims[3] = { 1, 1, 1 };
			float cell = 1.0f;
			std::vector<uint32_t> cellStart; // dims^3 + 1
		};
		uint32_t maxLvl = 0;
		for (const BakeBrick& b : bricks)
			if (b.hasProbe)
				maxLvl = std::max(maxLvl, b.level);
		std::vector<LevelCloud> clouds(maxLvl + 1);
		for (uint32_t lvl = 0; lvl <= maxLvl; ++lvl) {
			LevelCloud& cloud = clouds[lvl];
			cloud.cell = cfg.rootBrickSize / float(1u << lvl);
			glm::vec3 gmin(1e30f), gmax(-1e30f);
			for (uint32_t i = 0; i < bricks.size(); ++i) {
				if (!bricks[i].hasProbe || bricks[i].level != lvl)
					continue;
				cloud.probeBricks.push_back(i);
				gmin = glm::min(gmin, bricks[i].probePos);
				gmax = glm::max(gmax, bricks[i].probePos);
			}
			if (cloud.probeBricks.empty())
				continue;
			cloud.gmin = gmin - 0.001f;
			const glm::vec3 ext = gmax - cloud.gmin + 0.002f;
			for (uint32_t a = 0; a < 3; ++a)
				cloud.dims[a] = std::max(1u, uint32_t(ext[a] / cloud.cell) + 1u);
			const uint32_t cells = cloud.dims[0] * cloud.dims[1] * cloud.dims[2];
			cloud.cellStart.assign(cells + 1, 0);
			auto cellOf = [&](const glm::vec3& p, uint32_t (&out)[3]) {
				for (uint32_t a = 0; a < 3; ++a) {
					int32_t c = int32_t(std::floor((p[a] - cloud.gmin[a]) / cloud.cell));
					out[a] = uint32_t(std::clamp(c, 0, int32_t(cloud.dims[a]) - 1));
				}
			};
			for (uint32_t k = 0; k < cloud.probeBricks.size(); ++k) {
				uint32_t c[3];
				cellOf(bricks[cloud.probeBricks[k]].probePos, c);
				++cloud.cellStart[c[0] + cloud.dims[0] * (c[1] + cloud.dims[1] * c[2])];
			}
			uint32_t sum = 0;
			for (uint32_t c = 0; c < cells; ++c) {
				const uint32_t n = cloud.cellStart[c];
				cloud.cellStart[c] = sum;
				sum += n;
			}
			cloud.cellStart[cells] = sum;
			std::vector<uint32_t> fill(cells, 0);
			std::vector<uint32_t> ordered(cloud.probeBricks.size());
			for (uint32_t k = 0; k < cloud.probeBricks.size(); ++k) {
				uint32_t c[3];
				cellOf(bricks[cloud.probeBricks[k]].probePos, c);
				const uint32_t ci = c[0] + cloud.dims[0] * (c[1] + cloud.dims[1] * c[2]);
				ordered[cloud.cellStart[ci] + fill[ci]++] = cloud.probeBricks[k];
			}
			cloud.probeBricks = std::move(ordered);
		}

		// ---- (1) Placement diffusion: jitter + repulsion relaxation ----
		// NOTE: everything here moves probes AFTER their radiance was
		// baked, so baked amps no longer belong to the serialized
		// positions (fatal near arches/openings where indirect changes
		// fast). iterations == 0 disables the whole stage: probes keep
		// exactly the relocated positions they were baked at. The jitter
		// is gated too (it used to run even with 0 iterations).
		const uint32_t iterations = std::min(cfg.diffusionIterations, 32u);
		for (uint32_t lvl = 0; lvl < clouds.size() && iterations > 0; ++lvl) {
			LevelCloud& cloud = clouds[lvl];
			if (cloud.probeBricks.empty())
				continue;
			const float size = cloud.cell;
			// Deterministic jitter (seeded by level) breaks the lattice.
			Pcg32 jitter(0x9e3779b9u + uint64_t(lvl) * 126271u);
			for (uint32_t k = 0; k < cloud.probeBricks.size(); ++k) {
				BakeBrick& b = bricks[cloud.probeBricks[k]];
				const glm::vec3 offset(
					(jitter.Float() - 0.5f), (jitter.Float() - 0.5f),
					(jitter.Float() - 0.5f));
				const glm::vec3 cand = glm::clamp(
					b.probePos + offset * size * 0.4f,
					b.center - size * 0.5f, b.center + size * 0.5f);
				if (scene.Bvh().ClosestDistance(cand) >= std::max(0.03f, size * 0.1f))
					b.probePos = cand;
			}
			// Repulsion relaxation: probes push apart to an even spacing.
			const float targetSpacing = size * 0.85f;
			for (uint32_t iter = 0; iter < iterations; ++iter) {
				for (uint32_t k = 0; k < cloud.probeBricks.size(); ++k) {
					BakeBrick& b = bricks[cloud.probeBricks[k]];
					glm::vec3 disp(0.0f);
					uint32_t c0[3];
					for (uint32_t a = 0; a < 3; ++a)
						c0[a] = uint32_t(std::clamp(
							int32_t(std::floor((b.probePos[a] - cloud.gmin[a]) / cloud.cell)),
							0, int32_t(cloud.dims[a]) - 1));
					for (int dx = -1; dx <= 1; ++dx) {
						for (int dy = -1; dy <= 1; ++dy) {
							for (int dz = -1; dz <= 1; ++dz) {
								const int cx = int(c0[0]) + dx;
								const int cy = int(c0[1]) + dy;
								const int cz = int(c0[2]) + dz;
								if (cx < 0 || cy < 0 || cz < 0
									|| cx >= int(cloud.dims[0]) || cy >= int(cloud.dims[1])
									|| cz >= int(cloud.dims[2]))
									continue;
								const uint32_t ci = uint32_t(cx)
									+ cloud.dims[0] * (uint32_t(cy)
										+ cloud.dims[1] * uint32_t(cz));
								for (uint32_t s = cloud.cellStart[ci];
									s < cloud.cellStart[ci + 1]; ++s) {
									const uint32_t j = cloud.probeBricks[s];
									if (j == cloud.probeBricks[k])
										continue;
									const glm::vec3 d = b.probePos - bricks[j].probePos;
									const float dist = glm::length(d);
									if (dist >= targetSpacing || dist < 1e-5f)
										continue;
									disp += (d / dist)
										* ((targetSpacing - dist) / targetSpacing);
								}
							}
						}
					}
					if (glm::dot(disp, disp) < 1e-8f)
						continue;
					glm::vec3 cand = b.probePos + disp * (size * 0.2f);
					cand = glm::clamp(cand,
						b.center - size * 0.5f, b.center + size * 0.5f);
					if (scene.Bvh().ClosestDistance(cand) >= std::max(0.03f, size * 0.1f))
						b.probePos = cand;
				}
			}
		}

		// Re-trace the 6-axis visibility from the relaxed positions (the
		// baked distances belong to the final placement).
		{
			static const glm::vec3 kAxes[6] = {
				{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
				{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
			};
			for (BakeBrick& b : bricks) {
				if (!b.hasProbe)
					continue;
				for (uint32_t k = 0; k < 6; ++k) {
					TriangleBvh::Hit h;
					// Same sentinel convention as BakeProbeBrick: 0 means
					// unobstructed, never the trace cutoff.
					float d = 0.0f;
					if (scene.Bvh().Raycast(b.probePos, kAxes[k],
							cfg.visibilityMaxDistance, h))
						d = std::max(h.t, 1e-3f);
					b.vis[k] = d;
				}
			}
		}

		// ---- (2) Radiance diffusion: Laplacian smoothing over the probe
		// neighbor graph (heat equation on the probe field). Neighboring
		// probes exchange amplitude mass; per-probe Monte Carlo noise
		// equalizes instead of reading as colored dots. The gather spans
		// ADJACENT LEVELS too (halved weight): where error-driven
		// subdivision created mixed L(n)/L(n+1) regions, parent-scale
		// and child-scale probes systematically disagree — at runtime the
		// partition-of-unity blends across that seam and the disagreement
		// shows as per-probe patches.
		for (uint32_t pass = 0; pass < cfg.smoothingPasses; ++pass) {
			// Neighbor gather over one level cloud, within that cloud's
			// support radius; skips the probe itself.
			auto gather = [&](const LevelCloud& cloud, const glm::vec3& pos,
					uint32_t selfIdx, float weightScale,
					glm::vec3 (&avg)[kSgLobeCount], float& wsum) {
				const float r = cfg.probeSupportFactor * cloud.cell;
				const float r2 = r * r;
				uint32_t c0[3];
				for (uint32_t a = 0; a < 3; ++a)
					c0[a] = uint32_t(std::clamp(
						int32_t(std::floor((pos[a] - cloud.gmin[a]) / cloud.cell)),
						0, int32_t(cloud.dims[a]) - 1));
				for (int dx = -1; dx <= 1; ++dx) {
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dz = -1; dz <= 1; ++dz) {
							const int cx = int(c0[0]) + dx;
							const int cy = int(c0[1]) + dy;
							const int cz = int(c0[2]) + dz;
							if (cx < 0 || cy < 0 || cz < 0
								|| cx >= int(cloud.dims[0]) || cy >= int(cloud.dims[1])
								|| cz >= int(cloud.dims[2]))
								continue;
							const uint32_t ci = uint32_t(cx)
								+ cloud.dims[0] * (uint32_t(cy)
									+ cloud.dims[1] * uint32_t(cz));
							for (uint32_t s = cloud.cellStart[ci];
								s < cloud.cellStart[ci + 1]; ++s) {
								const uint32_t j = cloud.probeBricks[s];
								if (j == selfIdx)
									continue;
								const BakeBrick& nb = bricks[j];
								// Skip non-finite neighbors: their mass must
								// not leak into healthy probes (with the fit
								// quarantine above this never triggers, but
								// the check is nearly free).
								bool nbOk = true;
								for (uint32_t i = 0; i < kSgLobeCount && nbOk; ++i)
									nbOk = std::isfinite(nb.amps[i].x + nb.amps[i].y + nb.amps[i].z);
								if (!nbOk)
									continue;
								const glm::vec3 d = pos - nb.probePos;
								const float dist2 = glm::dot(d, d);
								if (dist2 >= r2)
									continue;
								const float x2 = dist2 / r2;
								const float x2m = 1.0f - x2;
								const float w = x2m * x2m * x2m * weightScale;
								wsum += w;
								for (uint32_t i = 0; i < kSgLobeCount; ++i)
									avg[i] += nb.amps[i] * w;
							}
						}
					}
				}
			};
			for (uint32_t lvl = 0; lvl < clouds.size(); ++lvl) {
				LevelCloud& cloud = clouds[lvl];
				if (cloud.probeBricks.empty())
					continue;
				// Jacobi update: snapshot of deltas, applied after gather.
				std::vector<glm::vec3> delta(cloud.probeBricks.size() * kSgLobeCount);
				for (uint32_t k = 0; k < cloud.probeBricks.size(); ++k) {
					const uint32_t selfIdx = cloud.probeBricks[k];
					const BakeBrick& b = bricks[selfIdx];
					glm::vec3 avg[kSgLobeCount] = {};
					float wsum = 0.0f;
					gather(cloud, b.probePos, selfIdx, 1.0f, avg, wsum);
					if (lvl > 0 && !clouds[lvl - 1].probeBricks.empty())
						gather(clouds[lvl - 1], b.probePos, selfIdx, 0.5f, avg, wsum);
					if (lvl + 1 < clouds.size() && !clouds[lvl + 1].probeBricks.empty())
						gather(clouds[lvl + 1], b.probePos, selfIdx, 0.5f, avg, wsum);
					if (wsum <= 1e-4f)
						continue;
					for (uint32_t i = 0; i < kSgLobeCount; ++i)
						delta[k * kSgLobeCount + i] =
							(avg[i] / wsum - b.amps[i]) * cfg.smoothingLambda;
				}
				for (uint32_t k = 0; k < cloud.probeBricks.size(); ++k) {
					BakeBrick& b = bricks[cloud.probeBricks[k]];
					for (uint32_t i = 0; i < kSgLobeCount; ++i) {
						const glm::vec3 v = b.amps[i] + delta[k * kSgLobeCount + i];
						// Never let a non-finite smoothing step poison a
						// healthy probe (or spread further next pass).
						if (std::isfinite(v.x + v.y + v.z))
							b.amps[i] = v;
					}
				}
			}
		}
	} // end diffusion opt-in (skipped entirely when both stages are 0:
	  // BakeProbeBrick already relocated + traced visibility in place,
	  // so there is nothing to redo)

	// ------------------------------------------------------------------
	// Assemble the volume
	// ------------------------------------------------------------------
	uint32_t maxLevel = 0;
	uint32_t probeCount = 0;
	for (const BakeBrick& b : bricks) {
		if (b.hasProbe) {
			++probeCount;
			maxLevel = std::max(maxLevel, b.level);
		}
	}
	stats.probeCount = probeCount;

	outVolume = SgGiVolume{};
	outVolume.boundsMin = bmin;
	outVolume.boundsMax = bmax;
	outVolume.levelCount = std::min(maxLevel + 1, kSgGiMaxLevels);
	for (uint32_t lvl = 0; lvl < outVolume.levelCount; ++lvl) {
		const float brickSize = cfg.rootBrickSize / float(1u << lvl);
		// Cell = 1 brick: per-pixel list length is scale-free —
		// ((cellBricks + sphereDiameterBricks)/spacing)^3 — so a 2x cell
		// with a 2.5-brick sphere stuffs every cell with ~91 probes (capped
		// at 64, silently DROPPING probes: holes + a ~9 ms/frame gather
		// walk). Cell = brick + support = spacing (sphere d = 2 bricks)
		// gives ~27 probes/cell — console-class Enlighten-style geometry.
		outVolume.cellSize[lvl] = brickSize;
	}
	outVolume.probes.clear();
	outVolume.probes.reserve(probeCount);
	for (const BakeBrick& b : bricks) {
		if (!b.hasProbe)
			continue;
		SgProbe p{};
		p.position = b.probePos;
		p.radius = b.size * cfg.probeSupportFactor;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			p.amps[i] = b.amps[i];
		for (uint32_t k = 0; k < 6; ++k)
			p.visibility[k] = b.vis[k];
		p.level = b.level;
		p.valid = true;
		if (p.level < kSgGiMaxLevels) {
			outVolume.probes.push_back(p);
			++stats.probesPerLevel[p.level];
		}
	}
	outVolume.BuildGrids();

	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>(t1 - t0).count();
	{
		char line[192];
		std::snprintf(line, sizeof(line),
			"bake: done: %u probes in %.1fs (%.1f MiB GPU, %llu clamped, %llu nonfinite)\n",
			stats.probeCount, stats.seconds,
			double(outVolume.GpuBytes()) / (1024.0 * 1024.0),
			(unsigned long long)stats.samplesClamped,
			(unsigned long long)stats.samplesNonFinite);
		say(line);
	}
	return true;
}

} // namespace smvk::gi
