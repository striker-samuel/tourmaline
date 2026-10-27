// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Analytic environment zones (see Rendergraph/envzones.h): the CPU packer
// (authored OBB + look -> GPU record) and the CPU mirror of the shader
// resolver (falloff + first-zone selection) for tooling/tests.

#include <Engine/Rendergraph/envzones.h>
#include <Engine/Scene/scene.h>

#include <algorithm>
#include <cmath>

namespace smvk::envzones {

namespace {

inline bool Mat4Finite(const glm::mat4& m) {
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (!std::isfinite(m[c][r]))
				return false;
	return true;
}

inline uint32_t NegBit(int axis) { return 1u << (2 * axis); }
inline uint32_t PosBit(int axis) { return 2u << (2 * axis); }

} // namespace

bool MakeEnvZoneGpu(const glm::mat4& transform, uint32_t infMask,
	const EnvZone& look, float priority, float blendWeight,
	float blendRadius, EnvZoneGpu& out) {
	const glm::mat4& g = transform;
	float half[3] = {
		glm::length(glm::vec3(g[0])),
		glm::length(glm::vec3(g[1])),
		glm::length(glm::vec3(g[2]))
	};
	infMask &= 63u;

	// Empty zone: a flat finite axis without any inf face (ZoneCovers).
	for (int a = 0; a < 3; ++a) {
		const bool flat = std::isfinite(half[a]) && !(half[a] > 0.0f);
		const bool faceInf = (infMask & (NegBit(a) | PosBit(a))) != 0;
		if (flat && !faceInf)
			return false;
	}

	// Sanitize the frame before inverting: fully unbounded (or flat with
	// an inf face) axes get unit columns - those axes are skipped by the
	// coverage test anyway.
	glm::mat4 frame = g;
	for (int a = 0; a < 3; ++a) {
		const bool flat = std::isfinite(half[a]) && !(half[a] > 0.0f);
		const bool faceInf = (infMask & (NegBit(a) | PosBit(a))) != 0;
		if (!std::isfinite(half[a]) || (flat && faceInf))
			frame[a] = glm::vec4(a == 0, a == 1, a == 2, 0.0f);
	}
	const float det = glm::determinant(frame);
	if (!std::isfinite(det) || std::abs(det) < 1e-12f)
		return false;
	const glm::mat4 inv = glm::inverse(frame);
	if (!Mat4Finite(inv))
		return false;

	// Store ROWS (not GLM's columns): the shader uses dot(row, p4) so a
	// DX/GL matrix-convention mismatch can never corrupt the test.
	for (int r = 0; r < 4; ++r)
		out.worldToLocal[r] = glm::vec4(
			inv[0][r], inv[1][r], inv[2][r], inv[3][r]);
	out.halfAndMask = glm::uvec4(
		glm::floatBitsToUint(half[0]),
		glm::floatBitsToUint(half[1]),
		glm::floatBitsToUint(half[2]),
		infMask);
	out.shape = glm::vec4(priority, std::clamp(blendWeight, 0.0f, 1.0f),
		std::max(blendRadius, 0.0f), 0.0f);
	out.sky = glm::vec4(look.skyTint, look.ambientI);
	out.fog = glm::vec4(look.fogColor, look.fogDensity);
	out.vol = glm::vec4(look.volumetrics, look.volumetricDensity, 0.0f, 0.0f);
	return true;
}

float EnvZoneFalloff(const EnvZoneGpu& zone, const glm::vec3& p) {
	const glm::vec3 half(
		glm::uintBitsToFloat(zone.halfAndMask.x),
		glm::uintBitsToFloat(zone.halfAndMask.y),
		glm::uintBitsToFloat(zone.halfAndMask.z));
	const uint32_t infMask = zone.halfAndMask.w & 63u;
	const glm::vec4 p4(p, 1.0f);
	const glm::vec3 local(
		glm::dot(zone.worldToLocal[0], p4),
		glm::dot(zone.worldToLocal[1], p4),
		glm::dot(zone.worldToLocal[2], p4));

	float dist2 = 0.0f;
	for (int a = 0; a < 3; ++a) {
		const float h = half[a];
		const bool flat = std::isfinite(h) && !(h > 0.0f);
		const bool faceInf = (infMask & (NegBit(a) | PosBit(a))) != 0;
		if (flat && !faceInf)
			return 0.0f; // empty zone
		const bool negInf = !std::isfinite(h)
			|| (infMask & NegBit(a)) != 0 || (flat && faceInf);
		const bool posInf = !std::isfinite(h)
			|| (infMask & PosBit(a)) != 0 || (flat && faceInf);
		const float scale = (std::isfinite(h) && h > 0.0f) ? h : 1.0f;
		if (!negInf && local[a] < -1.0f) {
			const float d = (-1.0f - local[a]) * scale;
			dist2 += d * d;
		}
		if (!posInf && local[a] > 1.0f) {
			const float d = (local[a] - 1.0f) * scale;
			dist2 += d * d;
		}
	}
	if (!(dist2 > 0.0f))
		return 1.0f;
	const float radius = std::max(zone.shape.z, 0.0f);
	if (!(radius > 0.0f))
		return 0.0f;
	const float t = std::clamp(std::sqrt(dist2) / radius, 0.0f, 1.0f);
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

bool EnvZoneAt(const std::vector<EnvZoneGpu>& zones, const glm::vec3& p,
	float& weight, EnvZone& look) {
	weight = 0.0f;
	look = EnvZone{};
	const size_t n = std::min<size_t>(zones.size(), kEnvZoneMaxZones);
	for (size_t i = 0; i < n; ++i) {
		const EnvZoneGpu& z = zones[i];
		const float w = std::clamp(z.shape.y, 0.0f, 1.0f)
			* EnvZoneFalloff(z, p);
		if (!(w > 0.0f))
			continue;
		weight = w;
		look.skyTint = glm::vec3(z.sky);
		look.ambientI = z.sky.w;
		look.fogColor = glm::vec3(z.fog);
		look.fogDensity = z.fog.w;
		look.volumetrics = z.vol.x;
		look.volumetricDensity = z.vol.y;
		return true;
	}
	return false;
}

EnvZone DeriveZoneLook(const SceneEnv& e, const SceneEnv& base)
{
	EnvZone z{};
	for (int c = 0; c < 3; ++c) {
		const float d = base.skyColor[c];
		z.skyTint[c] = d > 1e-5f ? e.skyColor[c] / d : 1.0f;
	}
	// MapSceneEnv feeds skyIntensity straight into iblIntensity, which is
	// the plain path's ambient scale - so the authored sky intensity IS
	// the zone's ambient intensity.
	z.ambientI = e.skyIntensity;
	z.fogColor = e.fogColor;
	z.fogDensity = e.fogDensity;
	z.volumetrics = base.volumetrics > 1e-5f
		? std::max(e.volumetrics, 0.0f) / base.volumetrics
		: 1.0f;
	z.volumetricDensity = e.volumetricDensity;
	return z;
}

} // namespace smvk::envzones
