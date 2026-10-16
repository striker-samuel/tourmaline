// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Packed GPU vertex streams (see VertexShading in mesh.h).
//
// The shading stream is 24 bytes: color (RGBA8_UNORM), uv (RG16_FLOAT =
// packHalf2x16), chartUv chart-local lightmap UV (RG16_UNORM =
// packUnorm2x16), normal (oct32), tangent (oct32 + handedness bit),
// chart (R32_UInt chart index). Normal
// and tangent ride as Format::R32_UInt and are unpacked by hand in the
// vertex shader (engine/shaders/vertex_pack.hlsli mirrors this file — keep
// the two in sync; the HLSL side uses the same sign(0)=+1 convention and
// the same rounding).
//
// Octahedral mapping (Meyer et al.): project n onto the octahedron
// (|x|+|y|+|z| = 1), fold the lower hemisphere (z < 0) into the upper
// square, quantize the resulting 2D point in [-1,1]^2 as UNORM.
//
// Layout:
//   normal : bits[15:0]  = oct.x as unorm16, bits[31:16] = oct.y as unorm16.
//   tangent: bits[31:16] = oct.x as unorm16, bits[15:1] = oct.y as unorm15,
//            bit[0] = handedness (1 = +1, 0 = -1; a zero w packs as +1).
// The tangent drops one bit of oct.y to make room for handedness; the
// bitangent itself is never stored (B = cross(N, T) * w in the fragment
// shader, see default_lit.frag.hlsl).

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

namespace smvk {
namespace vertexpack {

constexpr float kUnorm16Scale = 65535.0f;
constexpr float kUnorm16Inv = 1.0f / 65535.0f;
constexpr float kUnorm15Scale = 32767.0f;
constexpr float kUnorm15Inv = 1.0f / 32767.0f;

// Oct fold sign with sign(0) = +1 (must match vertex_pack.hlsli).
inline float OctSign(float v) noexcept { return v >= 0.0f ? 1.0f : -1.0f; }

inline glm::vec2 OctEncode01(const glm::vec3& n) noexcept {
	glm::vec2 p{ 0.0f, 0.0f };
	const float invL1 = 1.0f / (fabsf(n.x) + fabsf(n.y) + fabsf(n.z));
	p.x = n.x * invL1;
	p.y = n.y * invL1;
	if (n.z < 0.0f) {
		const float tx = (1.0f - fabsf(p.y)) * OctSign(p.x);
		const float ty = (1.0f - fabsf(p.x)) * OctSign(p.y);
		p.x = tx;
		p.y = ty;
	}
	return p;
}

inline uint32_t OctQuantize16(float v) noexcept {
	const float c = v * 0.5f + 0.5f;
	const float clamped = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	return static_cast<uint32_t>(floorf(clamped * kUnorm16Scale + 0.5f));
}

inline uint32_t OctQuantize15(float v) noexcept {
	const float c = v * 0.5f + 0.5f;
	const float clamped = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	return static_cast<uint32_t>(floorf(clamped * kUnorm15Scale + 0.5f));
}

// Normal -> 32-bit octahedral (16+16). Degenerate input maps to +Z.
inline uint32_t PackNormalOct(glm::vec3 n) noexcept {
	const float l2 = glm::dot(n, n);
	if (!(l2 > 1e-24f))
		n = glm::vec3(0.0f, 0.0f, 1.0f);
	else
		n *= 1.0f / sqrtf(l2);
	const glm::vec2 p = OctEncode01(n);
	const uint32_t ux = OctQuantize16(p.x);
	const uint32_t uy = OctQuantize16(p.y);
	return (uy << 16) | ux;
}

inline glm::vec3 UnpackNormalOct(uint32_t packed) noexcept {
	const uint32_t ux = packed & 0xFFFFu;
	const uint32_t uy = (packed >> 16) & 0xFFFFu;
	float x = static_cast<float>(ux) * (2.0f * kUnorm16Inv) - 1.0f;
	float y = static_cast<float>(uy) * (2.0f * kUnorm16Inv) - 1.0f;
	// z keeps its (negative) pre-fold value: the fold only undoes the
	// hemisphere flip of xy (recomputing z from the folded xy would flip
	// the hemisphere back and break the -Z pole).
	float z = 1.0f - fabsf(x) - fabsf(y);
	if (z < 0.0f) {
		const float tx = (1.0f - fabsf(y)) * OctSign(x);
		const float ty = (1.0f - fabsf(x)) * OctSign(y);
		x = tx;
		y = ty;
	}
	const float l2 = x * x + y * y + z * z;
	if (!(l2 > 1e-24f))
		return glm::vec3(0.0f, 0.0f, 1.0f);
	const float inv = 1.0f / sqrtf(l2);
	return glm::vec3(x * inv, y * inv, z * inv);
}

// Tangent xyz -> 32-bit octahedral (16+15) + handedness in bit 0.
// w == 0 (placeholder) packs as +1 to match the fragment fallback.
inline uint32_t PackTangentOct(glm::vec3 t, float w) noexcept {
	const float l2 = glm::dot(t, t);
	if (!(l2 > 1e-24f))
		t = glm::vec3(1.0f, 0.0f, 0.0f);
	else
		t *= 1.0f / sqrtf(l2);
	const glm::vec2 p = OctEncode01(t);
	const uint32_t ux = OctQuantize16(p.x);
	const uint32_t uy = OctQuantize15(p.y);
	const uint32_t wbit = (w < 0.0f) ? 0u : 1u;
	return (ux << 16) | (uy << 1) | wbit;
}

inline glm::vec4 UnpackTangentOct(uint32_t packed) noexcept {
	const uint32_t ux = (packed >> 16) & 0xFFFFu;
	const uint32_t uy = (packed >> 1) & 0x7FFFu;
	const float w = (packed & 1u) ? 1.0f : -1.0f;
	float x = static_cast<float>(ux) * (2.0f * kUnorm16Inv) - 1.0f;
	float y = static_cast<float>(uy) * (2.0f * kUnorm15Inv) - 1.0f;
	// z keeps its (negative) pre-fold value (see UnpackNormalOct).
	float z = 1.0f - fabsf(x) - fabsf(y);
	if (z < 0.0f) {
		const float tx = (1.0f - fabsf(y)) * OctSign(x);
		const float ty = (1.0f - fabsf(x)) * OctSign(y);
		x = tx;
		y = ty;
	}
	const float l2 = x * x + y * y + z * z;
	if (!(l2 > 1e-24f))
		return glm::vec4(1.0f, 0.0f, 0.0f, w);
	const float inv = 1.0f / sqrtf(l2);
	return glm::vec4(x * inv, y * inv, z * inv, w);
}

inline uint32_t PackUvHalf(const glm::vec2& uv) noexcept {
	return glm::packHalf2x16(uv);
}

// Chart-local lightmap UV: normalized [0,1] in the chart's inner rect,
// UNORM16 for texel precision at 8192-wide atlases (half-float quantizes
// to whole texels there). The atlas-space transform lives per chart in
// the GPU transform buffer and is applied in the vertex shader, so the
// vertex stream no longer carries atlas-space uv2.
inline uint32_t PackChartUv(const glm::vec2& uv) noexcept {
	return glm::packUnorm2x16(glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f)));
}
// Legacy alias (atlas-space uv2 encoding was identical).
inline uint32_t PackUv2Unorm(const glm::vec2& uv) noexcept {
	return PackChartUv(uv);
}

inline uint32_t PackColorUnorm8(const glm::vec4& color) noexcept {
	return glm::packUnorm4x8(color);
}

// ---- Terrain stream packing (see TerrainVertex in mesh.h) --------------
// The terrain format stores normal/tangent as RGBA16_FLOAT halves (not the
// static stream's oct32): generated geometry with no chart data, and the
// tangent needs its w channel for the bitangent-flip handedness anyway.
// out[4] is the raw 4x half-word attribute (w unused for the normal).

// Normal -> 4x half (xyz normalized, w = 0). Degenerate input maps to +Z.
inline void PackTerrainNormalHalf(const glm::vec3& n, uint16_t out[4]) noexcept {
	glm::vec3 v = n;
	const float l2 = glm::dot(v, v);
	if (!(l2 > 1e-24f))
		v = glm::vec3(0.0f, 0.0f, 1.0f);
	else
		v *= 1.0f / sqrtf(l2);
	out[0] = glm::packHalf1x16(v.x);
	out[1] = glm::packHalf1x16(v.y);
	out[2] = glm::packHalf1x16(v.z);
	out[3] = 0;
}

// Tangent xyz + bitangent-flip handedness -> 4x half (xyz normalized
// tangent, w = +1/-1 flip; B = cross(N, T) * flip in the shader — the
// bitangent itself is never stored). Degenerate tangent maps to +X.
inline void PackTerrainTangentHalf(const glm::vec3& t, float flip,
	uint16_t out[4]) noexcept {
	glm::vec3 v = t;
	const float l2 = glm::dot(v, v);
	if (!(l2 > 1e-24f))
		v = glm::vec3(1.0f, 0.0f, 0.0f);
	else
		v *= 1.0f / sqrtf(l2);
	out[0] = glm::packHalf1x16(v.x);
	out[1] = glm::packHalf1x16(v.y);
	out[2] = glm::packHalf1x16(v.z);
	out[3] = glm::packHalf1x16(flip < 0.0f ? -1.0f : 1.0f);
}

} // namespace vertexpack
} // namespace smvk
