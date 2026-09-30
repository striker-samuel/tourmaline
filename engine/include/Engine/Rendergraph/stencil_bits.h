#pragma once
// Shared stencil-bit layout of the scene depth target's stencil aspect.
//
// The depth image (D32S8 / D24S8 on every supported device) carries an
// 8-bit stencil this engine never used: cleared 0 everywhere, disabled
// binds on every pass. The SSR receiver-material mask (previously a
// full-res RGBA16F "glass" target filled by a two-leg scene redraw)
// moves here instead — as a general per-material classification layer
// with room for the consumer systems below.
//
// Writer: the MAIN forward pass (alpha-tested first, then opaque, per
// material — see WorldRenderer's bindForMaterial): every bind of an
// OPAQUE material's pipeline carries the stencil REPLACE write state +
// the material's classification as the dynamic reference (see
// CommandBuffer::SetStencilReference — the reference never enters the
// pipeline cache key). Alpha-tested (Masked) materials classify too:
// the FS discard reverts the stencil write, so cutout holes stay
// unwritten for free. Transparent (Blend) materials never write —
// panes are the exception, marked by the PaneMask pass below. Occluded
// fragments fail Main's GreaterEqual depth test and KEEP the visible
// surface's bits (depthFailOp KEEP). Writing in Main makes the layer
// complete by construction on every path that shades the frame —
// including the Northlight temporal depth split (the filter's
// seed/remainder passes own depth, but Main still draws every visible
// surface through its filtered + direct legs), so no refresh redraw
// exists. MSAA frames skip the layer (Main writes the transient MSAA
// depth whose stencil is DontCare-stored).
//
// Pane receivers: transparent panes never write the opaque depth, so
// their receiver plane lives in the SSR builder's full-res R16_UNorm
// paneDepth target (the PaneMask redraw, Max blend = nearest pane) —
// pane identity is paneDepth > 0, no pane bit needed.
//
// Reader today: the SSR trace + composite (bindless S8_UINT stencil
// view + paneDepth). The metal/gloss quantization from the old
// continuous B/A mask channels to STENCIL_SSR (+ STENCIL_SSR_METAL)
// is the accepted tradeoff of moving into the stencil aspect.
//
// Bit budget (clear value 0 = unclassified / background — safe
// default: unmarked surfaces never read as reflective):
//   bit 0   STENCIL_SSR      reflective SSR receiver (metal / glossy /
//                            pane): the trace's material gate
//   bit 1   STENCIL_SSS      reserved: subsurface-scattering receivers
//   bit 2   STENCIL_HAIR      reserved: hair/strand geometry
//   bit 3   STENCIL_WATER     reserved: water surfaces
//   bit 4   STENCIL_DECAL     reserved: decal teardown/reconstruction
//   bit 5   STENCIL_NO_TAA    reserved: TAA exclusion (specular fireflies)
//   bit 6   STENCIL_SSR_METAL refinement of bit 0: metallic receiver —
//                            SSR Fresnel F0 rises to ~1 (flat colored-F0
//                            approximation, scalars only)
//   bit 7   reserved
//
// The HLSL twins of the live bits live in ssr.frag.hlsl — keep them in
// sync (single byte, cheap to audit).

#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>

#include <cstdint>

namespace smvk::stencil {

enum StencilBits : uint8_t {
	STENCIL_SSR = 1 << 0,      // reflective SSR receiver
	STENCIL_SSS = 1 << 1,      // reserved: subsurface
	STENCIL_HAIR = 1 << 2,     // reserved: hair
	STENCIL_WATER = 1 << 3,    // reserved: water
	STENCIL_DECAL = 1 << 4,    // reserved: decals
	STENCIL_NO_TAA = 1 << 5,   // reserved: TAA exclusion
	STENCIL_SSR_METAL = 1 << 6, // metallic receiver (F0 -> ~1)
	// bit 7 reserved.
};

// Classification thresholds (material factor domain 0..1). Scene-
// calibrated on Bistro: the glass/polished class authors roughness
// ~0.05-0.12, semi-gloss paint/wood 0.19-0.45, matte 0.5-1.0 — 0.15
// splits mirror-class from the semi-gloss majority (a 0.40 threshold
// marked 68% of Bistro's materials reflective: "everything looks ssr").
// Metals refine the gloss gate (a brushed metal at roughness 0.8 never
// mirrored in the old continuous (1-r)^2 fade either — ~0.04).
constexpr float kMetalThreshold = 0.50f;  // metallic >= 0.50 -> metal refinement
constexpr float kRoughnessMax = 0.15f;   // roughness <= 0.15 -> reflective

// Classification reference for ONE material's values (Main's opaque
// binds REPLACE it into the stencil; see the header comment). The SSR
// half of the old GlassMask redraw quantized: MIRROR-CLASS receivers
// (roughness <= 0.15 — glass, polished metal; the old continuous
// (1-r)^2 fade kept them at ~0.7-0.9 strength, so the binary gate
// reads near-identically there) carry STENCIL_SSR, mirror-class METALS
// additionally STENCIL_SSR_METAL. Semi-gloss (0.2-0.45) and matte
// surfaces are unclassified — silent, like their old ~0.3-0.6 fade
// never read as chrome. Layered materials OR their layers'
// classification at the call site — "strongest reflector wins", the
// per-draw quantization of the old per-pixel Max blend — which needs
// sibling-material fetches the caller (world renderer) owns.
// Masked (cutout) materials classify like anyone else: Main's
// alpha-tested FS discards the holes, which reverts their stencil
// writes (the old whole-draw zeroing, per-pixel instead).
inline uint8_t ReceiverRef(const MaterialValues& values)
{
	uint8_t ref = 0;
	if (values.params.z <= kRoughnessMax) {
		ref |= STENCIL_SSR;
		if (values.params.y >= kMetalThreshold)
			ref |= STENCIL_SSR_METAL;
	}
	return ref;
}

} // namespace smvk::stencil
