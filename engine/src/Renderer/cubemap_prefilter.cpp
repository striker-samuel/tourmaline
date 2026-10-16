// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/cubemap_prefilter.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace smvk {
namespace {

// Van der Corput radical inverse, base 2 (Hammersley point set).
float RadicalInverseVdC(uint32_t bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10f; // / 0x100000000
}

glm::vec2 Hammersley(uint32_t i, uint32_t n) {
	return glm::vec2(float(i) / float(n), RadicalInverseVdC(i));
}

// GGX normal-distribution importance sample (tangent space, N = +Z).
glm::vec3 ImportanceSampleGGX(const glm::vec2& xi, float roughness) {
	const float a = roughness * roughness;
	const float phi = 2.0f * 3.14159265358979323846f * xi.x;
	const float cosTheta = std::sqrt(
		(1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
	const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
	return glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
}

} // namespace

bool DirectionToFaceUv(const glm::vec3& dir, CubemapFace& face, float& u, float& v) {
	const float ax = std::abs(dir.x), ay = std::abs(dir.y), az = std::abs(dir.z);
	if (ax + ay + az <= 1e-12f)
		return false;
	float su = 0.0f, sv = 0.0f;
	if (ax >= ay && ax >= az) {
		if (dir.x > 0.0f) {
			face = CubemapFace::Px; // d = (1,-sv,-su)
			su = -dir.z / dir.x;
			sv = -dir.y / dir.x;
		}
		else {
			face = CubemapFace::Nx; // d = (-1,-sv,su)
			su = -dir.z / dir.x;
			sv = dir.y / dir.x;
		}
	}
	else if (ay >= ax && ay >= az) {
		if (dir.y > 0.0f) {
			face = CubemapFace::Py; // d = (su,1,sv)
			su = dir.x / dir.y;
			sv = dir.z / dir.y;
		}
		else {
			face = CubemapFace::Ny; // d = (su,-1,-sv)
			su = -dir.x / dir.y;
			sv = dir.z / dir.y;
		}
	}
	else {
		if (dir.z > 0.0f) {
			face = CubemapFace::Pz; // d = (su,-sv,1)
			su = dir.x / dir.z;
			sv = -dir.y / dir.z;
		}
		else {
			face = CubemapFace::Nz; // d = (-su,-sv,-1)
			su = dir.x / dir.z;
			sv = dir.y / dir.z;
		}
	}
	u = su * 0.5f + 0.5f;
	v = sv * 0.5f + 0.5f;
	return true;
}

glm::vec3 SampleCubemapBilinear(const CubemapBaseFaces& base, uint32_t size,
	const glm::vec3& dir) {
	CubemapFace face = CubemapFace::Px;
	float u = 0.5f, v = 0.5f;
	if (!DirectionToFaceUv(dir, face, u, v))
		return glm::vec3(0.0f);
	const float fx = std::clamp(u * float(size) - 0.5f, 0.0f, float(size) - 1.0f);
	const float fy = std::clamp(v * float(size) - 0.5f, 0.0f, float(size) - 1.0f);
	const uint32_t x0 = std::min(uint32_t(fx), size - 1);
	const uint32_t y0 = std::min(uint32_t(fy), size - 1);
	const uint32_t x1 = std::min(x0 + 1, size - 1);
	const uint32_t y1 = std::min(y0 + 1, size - 1);
	const float tx = std::clamp(fx - float(x0), 0.0f, 1.0f);
	const float ty = std::clamp(fy - float(y0), 0.0f, 1.0f);
	const std::vector<float>& px = base[uint32_t(face)];
	auto tap = [&](uint32_t x, uint32_t y) {
		const float* p = &px[(size_t(y) * size + x) * 4];
		return glm::vec3(p[0], p[1], p[2]);
	};
	const glm::vec3 a = tap(x0, y0), b = tap(x1, y0);
	const glm::vec3 c = tap(x0, y1), d = tap(x1, y1);
	return a * ((1.0f - tx) * (1.0f - ty))
		+ b * (tx * (1.0f - ty))
		+ c * ((1.0f - tx) * ty)
		+ d * (tx * ty);
}

uint32_t CubemapFullMipCount(uint32_t size) {
	size = std::max(1u, size);
	uint32_t levels = 1;
	while (size > 1) {
		size = std::max(1u, size / 2);
		++levels;
	}
	return levels;
}

void PrefilterCubemapFaceGGX(const CubemapBaseFaces& base, uint32_t size,
	CubemapFace face, uint32_t levels, uint32_t samplesPerTexel,
	std::vector<std::vector<float>>& outLevels) {
	size = std::max(1u, size);
	const uint32_t full = CubemapFullMipCount(size);
	if (levels == 0)
		levels = full;
	levels = std::min(levels, full);
	const uint32_t samples = std::max(1u, samplesPerTexel);
	outLevels.clear();
	outLevels.reserve(levels);

	uint32_t w = size, h = size;
	for (uint32_t level = 0; level < levels; ++level) {
		std::vector<float> dst(size_t(w) * h * 4, 0.0f);
		const float roughness = CubemapPrefilterRoughness(level, levels);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const float u = (float(x) + 0.5f) / float(w);
				const float v = (float(y) + 0.5f) / float(h);
				const CubemapDir cd = FaceUvToDirection(face, u, v);
				glm::vec3 R(cd.x, cd.y, cd.z);
				R = glm::normalize(R);

				glm::vec3 Lo(0.0f);
				if (roughness <= 1e-4f) {
					// Sharp level: single center fetch (exact base copy
					// when w == size; bilinear resample otherwise).
					Lo = SampleCubemapBilinear(base, size, R);
				}
				else {
					// Tangent frame around R (= N = V).
					const glm::vec3 up = (std::abs(R.z) < 0.999f)
						? glm::vec3(0.0f, 0.0f, 1.0f)
						: glm::vec3(1.0f, 0.0f, 0.0f);
					const glm::vec3 T = glm::normalize(glm::cross(up, R));
					const glm::vec3 B = glm::cross(R, T);
					float totalWeight = 0.0f;
					for (uint32_t i = 0; i < samples; ++i) {
						const glm::vec2 Xi = Hammersley(i, samples);
						const glm::vec3 Ht = ImportanceSampleGGX(Xi, roughness);
						const glm::vec3 H = glm::normalize(
							T * Ht.x + B * Ht.y + R * Ht.z);
						glm::vec3 L = 2.0f * glm::dot(R, H) * H - R;
						const float len2 = glm::dot(L, L);
						if (len2 <= 1e-12f)
							continue;
						L *= 1.0f / std::sqrt(len2);
						const float ndotl = glm::dot(R, L);
						if (ndotl <= 0.0f)
							continue;
						Lo += SampleCubemapBilinear(base, size, L) * ndotl;
						totalWeight += ndotl;
					}
					Lo = (totalWeight > 1e-9f) ? Lo / totalWeight : glm::vec3(0.0f);
				}

				float* px = &dst[(size_t(y) * w + x) * 4];
				px[0] = Lo.x;
				px[1] = Lo.y;
				px[2] = Lo.z;
				px[3] = 1.0f;
			}
		}
		outLevels.push_back(std::move(dst));
		w = std::max(1u, w / 2);
		h = std::max(1u, h / 2);
	}
}

void PrefilterCubemapGGX(const CubemapBaseFaces& base, uint32_t size,
	uint32_t levels, uint32_t samplesPerTexel, CubemapMipChains& outChains) {
	outChains.clear();
	outChains.reserve(kCubemapFaceCount);
	for (uint32_t f = 0; f < kCubemapFaceCount; ++f) {
		std::vector<std::vector<float>> faceLevels;
		PrefilterCubemapFaceGGX(base, size, CubemapFace(f), levels,
			samplesPerTexel, faceLevels);
		outChains.push_back(std::move(faceLevels));
	}
}

} // namespace smvk
