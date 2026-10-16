// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Shared stencil-VALUE layout of the scene depth target's stencil aspect.
//
// The depth image (D32S8 / D24S8 on every supported device) carries an
// 8-bit stencil this engine never used otherwise. It now carries the
// SSR receiver ROUGHNESS as a full byte — the continuous successor of
// the old binary gloss gate (STENCIL_SSR bit) and of the short-lived
// full-res R8_UNorm MRT roughness target (killed: the MRT write made
// Main measurably more expensive; the stencil REPLACE already happened
// per opaque fragment, so encoding roughness in its reference value
// costs Main nothing extra):
//
//   bits 0..6  receiver roughness, quantized: round(r * 127)
//              (127 levels — the authored Bistro range 0.05..1.0
//              survives far below perceptual granularity)
//   bit  7     metallic refinement: F0 rises to ~1 (flat colored-F0
//              approximation, scalars only)
//
// Writer: the MAIN forward pass (alpha-tested first, then opaque, per
// material — see WorldRenderer's bindForMaterial): every bind of an
// OPAQUE material's pipeline carries the stencil REPLACE write state +
// the material's quantized roughness|metal byte as the dynamic
// reference (see CommandBuffer::SetStencilReference — the reference
// never enters the pipeline cache key, so per-material bytes mint zero
// pipeline variants). Alpha-tested (Masked) materials write it too:
// the FS discard reverts the stencil write, so cutout holes stay
// unwritten for free (holes carry no depth either — the trace's
// background early-out never reads them). Transparent (Blend)
// materials never write it (the receiver behind a pane reads as the
// seen-through opaque surface — the PaneMask redraw is gone by design).
// Occluded fragments fail Main's GreaterEqual depth test and KEEP the
// visible surface's byte (depthFailOp KEEP). Writing in Main makes the
// layer complete on every path that shades the frame — including the
// Northlight temporal depth split (the filter's seed/remainder passes
// own depth, but Main still draws every visible surface through its
// filtered + direct legs), so no extra redraw exists. MSAA frames skip
// the layer (Main writes the transient MSAA depth whose stencil is
// DontCare-stored) — the trace falls back to the legacy mirror.
//
// Reader: the SSR trace (bindless S8_UINT stencil view + uintTextures[]
// alias, see ssr.frag.hlsl) decodes continuous roughness for the
// (1-r)^2 gloss fade + the r^2 cone jitter, and the metal bit for the
// Fresnel F0 rise. Clear value 0 = unwritten = roughness 0 = perfect
// mirror — safe ONLY because every pixel that survives the trace's
// receiver selection has valid opaque depth, i.e. Main drew it, i.e.
// the stencil was REPLACEd there.
//
// Layered materials combine at the call site — MIN roughness wins
// ("strongest reflector wins", the per-draw quantization of the old
// per-pixel Max blend), metal ORs; the sibling-material fetches need
// the world, which the caller (world renderer) owns.
//
// The HLSL twins of the live layout live in ssr.frag.hlsl — keep them
// in sync (single byte, cheap to audit).

#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>

#include <cmath>
#include <cstdint>

namespace smvk::stencil {

// Metal threshold (material factor domain 0..1): metallic >= 0.50 sets
// the F0-rise bit. Scene-calibrated on Bistro (the material census
// found no metallicRoughness textures in the pak — authored factors).
constexpr float kMetalThreshold = 0.50f;

// Roughness quantization: 7 bits of the byte (0..127). The display
// threshold below is CENSUS/UI ONLY (mirror-class naming for the
// material census readout) — the trace itself consumes the continuous
// value and never thresholds.
constexpr uint8_t kRoughnessMaxValue = 127u; // bits 0..6
constexpr uint8_t kMetalBit = 1u << 7;       // bit 7
constexpr float kCensusMirrorRoughness = 0.15f; // census display: r <= 0.15 -> "SSR"

// Receiver byte for ONE material's values (Main's opaque binds REPLACE
// it into the stencil; see the header comment): quantized roughness in
// bits 0..6, metal bit 7. Masked (cutout) materials classify like
// anyone else: Main's alpha-tested FS discards the holes, which reverts
// their stencil writes (the old whole-draw zeroing, per-pixel instead).
inline uint8_t ReceiverRef(const MaterialValues& values)
{
	const float roughness = std::min(std::max(values.params.z, 0.0f), 1.0f);
	uint8_t ref = static_cast<uint8_t>(
		roughness * static_cast<float>(kRoughnessMaxValue) + 0.5f);
	if (values.params.y >= kMetalThreshold)
		ref |= kMetalBit;
	return ref;
}

// Decode helpers (mirror the HLSL twins in ssr.frag.hlsl).
inline float RefRoughness(uint8_t ref)
{
	return static_cast<float>(ref & kRoughnessMaxValue)
		/ static_cast<float>(kRoughnessMaxValue);
}
inline bool RefMetal(uint8_t ref) { return (ref & kMetalBit) != 0; }

} // namespace smvk::stencil
