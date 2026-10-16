// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GGX specular prefilter for baked cubemaps (high-frequency specular IBL).
//
// Scope: probes and lightmaps carry ALL diffuse irradiance plus low-frequency
// specular; this module preconvolves environment captures ONLY for the
// high-frequency specular path (split-sum style: mip level ~= roughness).
//
// Convention (matches the runtime TextureCube sampling):
//   - chains[face][level] holds RGBA32F texels, face order = CubemapFace.
//   - Level 0 is the sharp capture (roughness 0, exact copy of the base).
//   - Level m (of L) preconvolves with roughness = m / (L - 1).
//   - The prefilter integral is the standard real-time split-sum
//     environment term:  Lo(R, r) = sum( L(Li) * (N.Li) ) / sum(N.Li)
//     with Li importance-sampled around R = N = V from the GGX normal
//     distribution (Hammersley low-discrepancy sequence, deterministic —
//     no RNG, identical bytes for identical input regardless of threading).
//   - Source fetches are bilinear taps of the BASE level only (no chained
//     mip dependency, so faces/levels parallelize trivially).
//
// Users: tools/cubemap_baker (--prefilter ggx) and the PakCooker --cubemap
// stage (which always preconvolves this way before embedding the KTX2 as a
// Texture blob at textures/cubemaps/<name>.ktx2).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Renderer/cubemap.h>

namespace smvk {

// Base-face storage: baseFaces[f] holds size*size*4 floats (RGBA32F).
using CubemapBaseFaces = std::vector<float>[kCubemapFaceCount];
// Prefiltered chains: chains[face][level] holds (w* h *4) floats, w/h halve
// per level down to 1x1.
using CubemapMipChains = std::vector<std::vector<std::vector<float>>>;

// Inverse of FaceUvToDirection: dominant-axis face selection + the matching
// su/sv solve, returned as u/v in [0,1]. False only for a zero vector.
bool DirectionToFaceUv(const glm::vec3& dir, CubemapFace& face, float& u, float& v);

// Bilinear HDR fetch of the base capture along dir (rgb; alpha reads 1).
glm::vec3 SampleCubemapBilinear(const CubemapBaseFaces& base, uint32_t size,
	const glm::vec3& dir);

// Roughness carried by mip level m of an L-level chain (0 = sharp).
inline float CubemapPrefilterRoughness(uint32_t level, uint32_t levels) {
	if (levels <= 1)
		return 0.0f;
	return float(level) / float(levels - 1);
}

// Mip-level count of a full chain for a size x size base (size, size/2, ...,
// 1x1). size must be >= 1.
uint32_t CubemapFullMipCount(uint32_t size);

// Prefilters ONE face of the base capture into its full (or truncated)
// chain. levels == 0 means the full chain; otherwise exactly levels entries
// (level 0 first). Thread-safe: reads base only, writes outLevels only.
void PrefilterCubemapFaceGGX(const CubemapBaseFaces& base, uint32_t size,
	CubemapFace face, uint32_t levels, uint32_t samplesPerTexel,
	std::vector<std::vector<float>>& outLevels);

// All six faces (single-threaded loop over PrefilterCubemapFaceGGX).
// outChains is resized to [6][levels] on return.
void PrefilterCubemapGGX(const CubemapBaseFaces& base, uint32_t size,
	uint32_t levels, uint32_t samplesPerTexel, CubemapMipChains& outChains);

} // namespace smvk
