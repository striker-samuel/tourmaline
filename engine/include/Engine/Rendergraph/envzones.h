// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Analytic environment zones: the world/env look (sky tint, ambient
// intensity, fog, volumetrics) varies per world region and blends across
// region borders.
//
// A zone is an env ENTITY (scene.envForNode) whose transform defines an
// OBB: translation + rotation = placement, column lengths = half extents,
// an INFINITE column or an EnvVolume inf-mask face = an unbounded slab /
// half-space (the exact contract lives in ZoneCovers). The look rides the
// node's SceneEnv block. There is NO baked map anymore: the renderer
// packs the zones into a small GPU buffer every frame and the shaders
// resolve the covering zone per pixel (EnvZoneAt - the exact CPU
// ZoneCovers test, with EnvVolume's blendWeight/blendRadius applied).
//
// Why: the old 2D XZ RGBA8 map clamped the world to 4 km / 10 m texels,
// capped the scene at 3 zones (one alpha channel was the fallback), could
// not represent rotated boxes or Y-varying coverage, and ignored
// EnvVolume's blend radius. The buffer is cheap (64 x 144 B per frame),
// has no world-size limit, no texel quantization and no hard zone cap
// beyond the GPU loop bound.
//
// Consumers: the terrain field (its ambient + fog blend per zone) and the
// volumetric fog (its march density/scatter blend per zone at the ray
// midpoint). The sun stays global (one sun everywhere). CPU helpers
// (EnvZoneFalloff/EnvZoneAt) mirror the shader resolver for tests/tooling.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace smvk::envzones {

// GPU loop bound (the buffer is sized for this many records). Zones past
// it are dropped after the priority sort (the renderer logs once).
inline constexpr uint32_t kEnvZoneMaxZones = 64;

// One zone's blendable look (the env subset zones own; the sun stays
// global - one sun everywhere). Matches the shader-side EnvZoneLook.
struct EnvZone {
	// Sky tint (linear 0..1, multiplies the env zenith/horizon/ground
	// colors) + the zone's ambient intensity (the iblIntensity analog -
	// pre-exposure HDR radiance scale for the analytic ambient).
	glm::vec3 skyTint{ 1.0f, 1.0f, 1.0f };
	float ambientI = 20.0f;
	// Fog (blended into the analytic fog color/density).
	glm::vec3 fogColor{ 0.6f, 0.7f, 0.8f };
	float fogDensity = 0.01f;
	// Volumetric fog (the raymarched pass: scattering scale over the
	// pass base (0 = off, 1 = the global intensity) + extinction
	// density, blended at the ray midpoint).
	float volumetrics = 0.0f;
	float volumetricDensity = 0.015f;
	float pad[2]{};
};

// GPU zone record (BDA storage buffer, 144 B; HLSL mirror in
// common.hlsli). worldToLocal holds the ROWS of the sanitized inverse
// frame (four float4, so the shader uses dot products and never depends
// on HLSL/GLM matrix conventions). halfAndMask.xyz = asuint(half
// extents), .w = the EnvVolume inf-mask bits. shape = (priority,
// blendWeight, blendRadius, reserved). sky/fog/vol mirror EnvZone.
struct EnvZoneGpu {
	glm::vec4 worldToLocal[4]{};
	glm::uvec4 halfAndMask{};
	glm::vec4 shape{};
	glm::vec4 sky{};
	glm::vec4 fog{};
	glm::vec4 vol{};
};
static_assert(sizeof(EnvZoneGpu) == 144, "EnvZoneGpu: HLSL mirror");

// Packs one authored zone. Returns false for an EMPTY zone (a flat finite
// axis without an inf face - the same rule as ZoneCovers). The frame is
// sanitized exactly like ZoneCovers: fully unbounded axes get unit columns
// before the inverse (INF entries would poison it). Called by the
// renderer's per-frame zone upload; also the unit-testable core.
bool MakeEnvZoneGpu(const glm::mat4& transform, uint32_t infMask,
	const EnvZone& look, float priority, float blendWeight,
	float blendRadius, EnvZoneGpu& out);

// Soft falloff in 0..1 at world position p: 1 inside the OBB, fading to
// 0 at blendRadius metres outside the bounded faces (Euclidean distance;
// infinite faces contribute nothing). Mirrors the shader helper.
float EnvZoneFalloff(const EnvZoneGpu& zone, const glm::vec3& p);

// Resolves the FIRST influencing zone in `zones` (the renderer uploads
// them sorted priority desc, then innermost, then node id - so the first
// hit is the winner) and blends its look with the weight
// blendWeight * falloff. Returns false when no zone influences p (the
// caller keeps the plain resolved env look). Mirrors the shader
// EnvZoneAt; used by tooling/tests.
bool EnvZoneAt(const std::vector<EnvZoneGpu>& zones, const glm::vec3& p,
	float& weight, EnvZone& look);

} // namespace smvk::envzones
