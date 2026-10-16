// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Tiled-terrain height packing (RG16 in RGBA8_UNORM).
//
// Contract (locked):
//   H-tile texture: RGBA8_UNORM, 1024x1024, R = height MSB, G = height LSB,
//   B/A free (preserved through cook, ignored by decode). No sRGB, no mips
//   in source.
//   h01 = (R*256 + G) / 65535,  worldY = kTerrainBaseY + h01 * kTerrainRange.
//   kTerrainBaseY = -40, kTerrainRange = 420  ->  6.4mm steps, no terracing.
//   Normals are COMPUTED from height (central differences); the import path
//   is stubbed (same signature, uncalled) so a future WM-normal texture can
//   slot in without touching callers.

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

namespace smvk::terrain {

// ---- mapping -----------------------------------------------------------
inline constexpr float kTerrainBaseY = -40.0f;
inline constexpr float kTerrainRange = 420.0f;
inline constexpr int kHeightTileRes = 1024;
inline constexpr int kMaskTileRes = 256;
inline constexpr float kHeightTileSize = 1024.0f; // meters per H-tile
inline constexpr float kGeoBlockSize = 256.0f;    // meters per G-block

[[nodiscard]] inline float DecodeH01(uint8_t r, uint8_t g) noexcept {
	// 16-bit value R*256+G over full 0..65535 range.
	const uint32_t v = static_cast<uint32_t>(r) * 256u + static_cast<uint32_t>(g);
	return static_cast<float>(v) * (1.0f / 65535.0f);
}

inline void EncodeRG16(float h01, uint8_t& rOut, uint8_t& gOut) noexcept {
	const float c = std::clamp(h01, 0.0f, 1.0f);
	const uint32_t v = static_cast<uint32_t>(c * 65535.0f + 0.5f);
	rOut = static_cast<uint8_t>((v >> 8) & 0xFFu);
	gOut = static_cast<uint8_t>(v & 0xFFu);
}

[[nodiscard]] inline float HeightToWorld(float h01) noexcept {
	return kTerrainBaseY + h01 * kTerrainRange;
}

[[nodiscard]] inline float WorldToH01(float worldY) noexcept {
	return (worldY - kTerrainBaseY) * (1.0f / kTerrainRange);
}

// Atlas texel: RG16 bytes -> true 16-bit height (R=MSB). The R16 atlas
// copies these bits verbatim, so the .r sample IS h01 and hardware
// linear filtering interpolates true heights.
[[nodiscard]] inline uint16_t PackHeightR16(uint8_t r, uint8_t g) noexcept {
	return static_cast<uint16_t>(static_cast<uint32_t>(r) * 256u + g);
}

// ---- normals ------------------------------------------------------------
// Computed base normal from 4 height neighbors (world-space heights, meters).
// step = texel spacing in meters (1.0 for 1024px/km H-tiles). Y-up, +Y sky.
[[nodiscard]] inline glm::vec3 ComputeNormalH(
	float hL, float hR, float hD, float hU, float step) noexcept {
	const float dx = (hR - hL) / (2.0f * step);
	// U is the +Z side, D the -Z side: dh/dz = (hU - hD) / 2s.
	const float dz = (hU - hD) / (2.0f * step);
	// Surface: y = h(x,z); normal = normalize(-dh/dx, 1, -dh/dz).
	const glm::vec3 n(-dx, 1.0f, -dz);
	const float len = glm::length(n);
	return len > 0.0f ? n * (1.0f / len) : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Import-path stub: identical signature, reserved for a future WM/Houdini
// normal texture (world-space, +Y up). Currently returns the computed normal
// so callers can wire the branch now and flip the source later. NOT called
// by the v1 build path.
[[nodiscard]] inline glm::vec3 ComputeNormalImportStub(
	float hL, float hR, float hD, float hU, float step,
	const glm::vec3& importedNormal) noexcept {
	(void)hL; (void)hR; (void)hD; (void)hU; (void)step;
	const float len = glm::length(importedNormal);
	return len > 0.0f ? importedNormal * (1.0f / len) : glm::vec3(0.0f, 1.0f, 0.0f);
}

} // namespace smvk::terrain
