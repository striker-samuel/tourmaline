// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Tiled environment-zone separation: the world/env look (sky tint, ambient
// intensity, fog, volumetrics) varies per world region and blends across
// region borders.
//
// The map: ONE RGBA8_UNORM texture over the world extent, each texel
// covering `texelSize` meters (default 10 m -> 400x400 over the default
// 4 km terrain). The R/G/B/A channels are the WEIGHTS of zone definitions
// 0..3 at that texel — normalized at sample time, so a texel can blend up
// to four zones; bilinear filtering blends between texels at runtime.
//
// The zones are ENTITIES (scene.envForNode nodes — see WorldRenderer::
// RebuildEnvZoneMap): coverage from the node TRANSFORM (translation +
// rotation = placement, scale = half extents per local axis, an INFINITE
// component = an unbounded slab), the look from the node's SceneEnv
// block. Channel 0 = the resolved environment (weight 1 outside every
// zone); channels 1..3 = the top-3 zones (priority descending, then
// innermost — the same order ZoneAtPosition resolves by). The sun stays
// global (one sun). Zone definitions ride FrameUniforms (the envZone*
// tail; see WorldRenderer.h), the map rides the bindless Texture2D
// array — every shader that includes common.hlsli can blend them via
// EnvZoneWeights (environment.hlsli).
//
// Consumers today: the terrain field (its ambient + fog blend per zone)
// and the volumetric fog (its march density/scatter blend per zone at
// the ray start). CPU-side SampleAt mirrors the GPU blend for
// tooling/tests.

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace smvk::envzones {

inline constexpr uint32_t kEnvZoneMax = 4; // R/G/B/A weight channels

// One zone's blendable look (the env subset zones own; the sun stays
// global — one sun everywhere).
struct EnvZone {
	// Sky tint (linear 0..1, multiplies the env zenith/horizon/ground
	// colors) + the zone's ambient intensity (the iblIntensity analog —
	// pre-exposure HDR radiance scale for the analytic ambient).
	glm::vec3 skyTint{ 1.0f, 1.0f, 1.0f };
	float ambientI = 20.0f;
	// Fog (blended into the analytic fog color/density).
	glm::vec3 fogColor{ 0.6f, 0.7f, 0.8f };
	float fogDensity = 0.01f;
	// Volumetric fog (the raymarched pass: scattering scale over the
	// pass base (0 = off, 1 = the global intensity) + extinction
	// density, blended at the ray start).
	float volumetrics = 0.0f;
	float volumetricDensity = 0.015f;
	float pad[2]{};
};

struct EnvZoneDesc {
	// Map extent (meters, square, centered at the origin — matches the
	// default terrain field).
	float worldSize = 4000.0f;
	// Meters per texel (the paint/blend granularity).
	float texelSize = 10.0f;
	// The up-to-4 zone definitions (channel i's weights blend zones[i]).
	EnvZone zones[kEnvZoneMax];
};

struct EnvZoneMap {
	EnvZoneDesc desc{};

	// CPU weight grid (RGBA8 packed as u32 per texel, row-major, x-major:
	// texel (x, z) at z * dim + x). 0xFFFFFFFF default = zone 0 fill.
	std::vector<uint32_t> weights;
	uint32_t dim = 0; // texels per side (worldSize / texelSize)

	// GPU map (bindless Texture2D; UINT32_MAX index = not registered).
	Holder<TextureHandle> texture;
	uint32_t bindlessIndex = UINT32_MAX;
	bool valid = false;

	[[nodiscard]] bool Valid() const {
		return valid && dim > 0 && weights.size() == size_t(dim) * dim
			&& !texture.empty() && bindlessIndex != UINT32_MAX;
	}

	// Builds the map (default zone-0 fill), the GPU texture + the
	// bindless registration. Returns false on a degenerate desc or a GPU
	// failure (the map stays invalid; consumers take the plain env path).
	bool Build(ResourceManager& ctx, const EnvZoneDesc& d);
	void Destroy();

	// Re-uploads the CPU weight grid after Paint (staging flush by the
	// caller's cadence). No-op when invalid.
	void Upload(ResourceManager& ctx);

	// World XZ -> texel center coordinates (continuous; bilinear taps).
	void WorldToTexel(float wx, float wz, float& outX, float& outZ) const;
	// Texel-space weight fetch (clamped, nearest).
	glm::vec4 WeightsAtTexel(float x, float z) const;

	// CPU blend at world XZ (bilinear over the grid, then the weighted
	// zone mix) — mirrors the GPU blend (EnvZoneWeights + the envZone*
	// FrameUniforms block) for tooling/tests.
	EnvZone SampleAt(float wx, float wz) const;
};

} // namespace smvk::envzones
