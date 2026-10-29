// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Screen-space reflections: half-res raymarch + depth-aware bilateral
// blur + depth-aware full-res composite.
//
// Ports the SSAO raw/blur/compose discipline into one place:
// resource creation (half-res raw + blurred reflection targets, full-res
// composite) + default program ownership + rendergraph pass setup.
// Sampling is bindless (textures[] indexed by push), barriers come from
// SetColor (write) + SetInputImages (read).
//
// Pipeline (trace + blur at half res, i.e. full extent / 2):
//   trace:     full-res depth 2x2 block -> nearest valid tap is the
//              receiver (reverse-Z: max depth = nearest; never fabricate
//              a center across a silhouette) ->
//              depth-derived normal
//              from CENTRAL differences over a ±3 full-res px baseline
//              (piecewise-planar depth makes one-sided 2px slopes step
//              at every triangle edge — tessellated mirrors read their
//              own wireframe; per-tap background same-depth fallback is
//              sign-safe by construction) -> screen-stride
//              adaptive view-space march (~6 px strides: dense where
//              footprints are small so strides can't straddle occluders,
//              long where the ray drills in depth) with slab acceptance
//              PLUS front-to-behind segment-crossing bisect + binary
//              refine + hit backface rejection (wrong-side hits show
//              occluded geometry through surfaces, so the march rejects
//              and continues instead of sampling them).
//              Output RGBA16F: rgb = reflected HDR x Fresnel x intensity,
//              a = edge x distance confidence (0 on a miss). Rays that
//              hit nothing contribute nothing: the base HDR already
//              carries the analytic sky specular IBL, so isolated mirrors
//              (chrome probe balls, open atriums) keep their chrome read
//              without double-counting the sky.
//   blur:      bilateral blur of (rgb, a) at half res, depth-weighted
//              with exact view distances (flip / finite-far agnostic via
//              invProj, same discipline as dof.hlsli) so edges don't
//              bleed. Kernel radius scales with the receiver roughness
//              (2/3/4 rings = 5x5/7x7/9x9 from the thin G-buffer) for
//              METALS only: the per-material lobe spread the old fixed
//              5x5 could not do (the trace's dielectric (1-r)^2 energy
//              fade substituted for it; metals keep full trace energy
//              now, the blur owns their spread — dielectrics keep fade +
//              5x5, bit-identical, and the blur taps stay off the
//              whole-screen budget: a roughness-only gate measured
//              +2..3.5 ms for zero visible dielectric gain).
//   temporal:  (optional TAA) motion-vector-reprojected accumulation of
//              the blurred layer at half res, scoped to SSR only — the
//              base HDR is never filtered. History lives in two ping-pong
//              targets; rejection (off-screen reproject, disocclusion,
//              invalid history) falls back to the current blurred tap.
//   composite: FULL-res base HDR + depth-guided 2x2 upsample of the
//              blurred (rgb, a) with full-res depth authority. Background
//              pixels keep the base color unconditionally. An NDC
//              vignette on the target pixel fades the added reflection
//              over the outer 15% of the NDC range (screen-edge
//              contributions are the smear band).
// Placement (the pass-order fix): after Main + SSGI/AO, BEFORE
// MotionBlur/DOF, before Exposure/Bloom/Tonemap. The trace reads the
// SHARP post-AO scene color (the old after-MotionBlur+DOF placement
// sampled the blurred/bokeh'd HDR as its radiance source), and the
// composited reflections then pass through MotionBlur and DOF with the
// rest of the scene - reflections blur and defocus consistently with
// their surroundings instead of sitting as sharp stickers over a
// defocused scene. Input is the post-AO scene HDR, output is the
// same-extent HDR with reflections composited additively. Downstream
// passes sample the SSR output when the pass ran, else the input
// directly (no copy disabled).
//
// Receiver-material layer: the SSSR-style THIN G-BUFFER (full-res
// RGBA8: RG = octahedral world normal, B = roughness, A = metal — see
// gbuffer.frag.hlsl), written by the Forward+ DEPTH PREPASS's
// G-buffer FS pair (the prepass already rasterizes every opaque
// surface; the FS is trivial — no textures, no lighting — so the
// G-buffer costs a few ALU ops + 4 bytes/px on a pass that exists,
// and MAIN IS UNTOUCHED: no MRT, no stencil classification. The
// short-lived Main MRT roughness write and the stencil-byte roughness
// encoding are both GONE — the MRT made Main measurably more
// expensive, the stencil carried no normals).
//   - trace: reads REAL interpolated normals from the G-buffer (the
//     depth-derived receiver normal — piecewise-planar per triangle,
//     the wireframe source — is GONE by construction), continuous
//     roughness for the (1-r)^2 gloss fade + the r^2 cone jitter
//     (rough receivers smear the lobe — the screen-space diffuse-GI
//     read), metal raises F0 to ~1. Unbound G-buffer (SSR off /
//     MSAA / legacy) = perfect-mirror uniform-F0 legacy behavior.
//   - composite: NDC vignette fades edge contributions.
//
// Positions still come from the 1x prepass depth (no GBuffer for
// world positions — the thin G-buffer carries normal + material
// only, exactly the SSSR input set):
//   - view positions via FrameUniforms.invProj (exact, flip/finite-far
//     agnostic — same discipline as ssao.h / dof.h),
//   - receiver normals from the G-buffer (oct-decoded, world space),
//   - Fresnel from F0 = lerp(uniform dielectric F0, 1, metal)
//     (glass default 0.04). Every dielectric surface reflects with the
//     same base F0; the per-receiver G-buffer values (continuous
//     roughness + metal) are the material gate — rough receivers fade
//     the lobe (1-r)^2 and jitter the ray cone r^2, mirrors keep it
//     sharp. Bistro windows (large vertical glass planes at grazing
//     street-view angles, double-sided) are the calibration case: aim
//     the default Bistro camera down the road at the shopfront glass
//     and raise intensity until the opposite facades read in the panes.
//     reflect() is invariant under N -> -N, so the viewer-facing
//     normal flip is safe for double-sided glass by construction.
//
// Coordinate discipline (matches ssao.frag.hlsl exactly, so a Y-flip /
// reverse-Z audit only has to happen once):
//   - framebuffer y-down NDC: ndc = px * invDepth * 2 - 1 (top = -1),
//     because the main view bakes proj[1][1] *= -1 for Vulkan.
//   - NDC -> UV: uv = ndc * 0.5 + 0.5 (top-left = (0,0)).
//   - reverse-Z: depth 1 = near, 0 = far/sky (clear 0); background is
//     d <= 1e-6; view distance comes only from invProj unprojection.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

namespace smvk {

// Blur-filter selection for the half-res reflection layer. The 2D
// bilateral (ssr_blur.frag.hlsl) is the quality baseline; the separable
// twin (ssr_blur_separable.frag.hlsl, H then V) is the performance
// candidate — 2*(2r+1) taps/px across two passes vs the 2D kernel's
// (2r+1)^2 (10/14/18 vs 25/49/81 at the radius 2/3/4 buckets). Both
// write the same blurred target, so the temporal/composite chain is
// mode-agnostic. blurTemp (the H-pass intermediate) is allocated in
// BOTH modes — live switching needs no resource rebuild.
enum class SsrBlurMode : uint32_t
{
	Bilateral2D = 0,
	SeparableHV = 1,
};

// SSR composite DIAGNOSTIC modes (the dark-triangle hunt): live A/B
// switches isolating each suspected contribution — selectable from the
// SSR UI panel (live) or TOURMALINE_SSR_COMPOSE_DEBUG headless, all
// capturable from the same frozen camera (TOURMALINE_CAM_POS/FWD +
// TOURMALINE_CAPTURE). Per-recording (the composite is stateless per
// frame — no history reset needed on a flip; the temporal layer
// upstream is unaffected). Interpretation: A killing the artifact =
// SSR composition; B = the envSpec replacement term (conf * (layer -
// envSpec) is NEGATIVE when the traced layer is darker than the IBL it
// replaces — the leading suspect); C = the upsample's normal
// rejection; D = the material-eligibility gate; E shows the add's
// gates (R = the reconstructed confidence the compositor consumes —
// the TEMPORAL alpha when TAA is on — G = eligibility, B = the
// effective add gate); F shows the ± add term (red = darkening, green
// = brightening — the old contrib view clamped negatives away); G
// fills magenta before any read (the fullscreen-draw coverage check).
enum class SsrComposeDebug : uint32_t
{
	Off = 0,
	BypassSsr = 1,      // A: outColor = base (zero SSR contribution)
	AdditiveOnly = 2,   // B: skip the envSpec replacement (add*layer)
	NoNormalReject = 3, // C: skip the upsample's normal agreement
	NoEligibility = 4,  // D: skip the material-eligibility gate
	ConfView = 5,       // E: R=conf G=eligibility B=conf*eligibility
	DeltaView = 6,      // F: red=darkening green=brightening (4x scaled)
	MagentaFill = 7,    // G: constant magenta (raster coverage check)
	// H: the LOAD-OP coverage check — the composite attachment is
	// CLEARED to magenta (a real Clear load op, not DontCare) and the
	// shader fills cyan: all-cyan = the draw covers the image (any
	// artifact is then shader inputs/gates); a MAGENTA region = the
	// draw never touched it (viewport/scissor/extent smaller than the
	// attachment — the straight-cutoff signature); anything else =
	// attachment sizing or a later pass.
	ClearFillCoverage = 8,
};

struct SsrDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	uint32_t steps = 32;                 // raymarch steps, 1..128
	// Hi-Z traversal loop cap, 4..128. 64 since the validHit fix (the
	// intersect's traversal exit): the old (i <= travIters) let budget-
	// exhausted rays validate a MID-FLIGHT position as a hit (the loose
	// reflections: anything within the 0.3 m thickness band of a real
	// surface along the ray). The fix makes budget exhaustion an honest
	// miss, so the cap must be high enough that the rays that USED to
	// loose-hit now refine to the true crossing - 24 cut them off
	// mid-flight (the measured floor-knight patch dropped 10x with the
	// fix at 24); 64 restores the coverage with CORRECT positions.
	uint32_t maxIntersections = 64;
	uint32_t refineSteps = 4;            // binary refine iterations, 0..8
	float maxDistance = 50.0f;           // ray length (m)
	float thickness = 0.3f;              // depth acceptance band (m, view units)
	float intensity = 1.0f;              // reflection strength multiplier
	float f0 = 0.04f;                    // dielectric base reflectance (glass)
	float blurSharpness = 10.0f;         // bilateral depth edge weight (higher = tighter)
	float glossThreshold = 0.94f;        // classify: rays wanted for roughness < this (mirrors < 0.0001 always shoot)
	float vignetteWidth = 0.15f;         // composite: NDC edge fade width (outer % of NDC range)
	float temporalBlend = 0.9f;          // TAA history weight, 0..0.98 (before attenuation)
	// Blur filter (see SsrBlurMode): the 2D baseline or the separable
	// H+V candidate. Per-recording (overrideDesc) — no resource rebuild
	// on a live flip (blurTemp exists in both modes); the app owns the
	// temporal-history invalidation on a mode switch.
	SsrBlurMode blurMode = SsrBlurMode::Bilateral2D;
	// Composite diagnostic mode (see SsrComposeDebug): the dark-triangle
	// hunt's live A/B switches. Per-recording, stateless, no history
	// reset on a flip.
	SsrComposeDebug composeDebug = SsrComposeDebug::Off;
	// SSR contribution debug attachment (the compose .dbg variant +
	// ssrContrib target): the composite writes its EXACT add term to a
	// second attachment so the contribution is directly visible (the
	// ssrdump ssr_contrib stage exports it) instead of differenced
	// against the base HDR where sub-0.1% coverage vanishes in the
	// tone map. Wasteful by design (a second full-res RGBA16F target +
	// write), debug-only. The Ensure recreates the chain on flips of
	// this flag (the target exists only while on).
	bool debugContrib = false;
	const char* name = "ssr";
	const char* viewName = "ssrView";
	const char* logicalName = "ssrLogical";
};

// Must match ssr_classify.cs.hlsl SsrClassifyPush (72 bytes) — the
// SSSR tile-classify pass: full half-res dispatch, wave-compacts the
// wanted rays into the ray list. All uint64 BDAs lead (C++ pads
// mid-struct uint64s to 8 while the HLSL/D3D layout packs rows, so
// scalars never precede a BDA — same discipline as every other push
// in this file).
struct SsrClassifyPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint64_t rayListAddr = 0;   // BDA of the packed ray list (halfW*halfH uints)
	uint64_t rayCounterAddr = 0; // BDA of the count ([0] live, [1] frozen)
	uint64_t radianceAddr = 0;  // BDA of the per-pixel radiance (halfW*halfH float4)
	uint64_t receiverDataAddr = 0; // BDA of the packed receiver metadata (halfW*halfH uints)
	uint32_t gbufferTextureIndex = UINT32_MAX; // thin G-buffer (RGBA8: oct normal + roughness + metal)
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (validity + receiver selection)
	float glossThreshold = 0.94f; // rays wanted for roughness < this (mirrors always shoot)
	uint32_t halfW = 0; // half-res reflection extent (the ray coords domain)
	uint32_t halfH = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(SsrClassifyPush) == 72);

// Must match ssr_prepare_indirect.cs.hlsl SsrIndirectPush (16 bytes).
struct SsrIndirectPush {
	uint64_t rayCounterAddr = 0;  // reads [0], freezes [1], resets [0]
	uint64_t indirectArgsAddr = 0; // the VkDispatchIndirectCommand (3 uints)
};
static_assert(sizeof(SsrIndirectPush) == 16);

// Must match ssr_intersect.cs.hlsl SsrIntersectPush (120 bytes) — the
// SSSR intersect (GGX VNDF + the classic Hi-Z traversal), indirect
// compute dispatch driven by the classify's ray count. uint64s first
// (see SsrClassifyPush).
struct SsrIntersectPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint64_t rayListAddr = 0;
	uint64_t rayCounterAddr = 0; // [1] = the frozen count (early-out bound)
	uint64_t radianceAddr = 0;
	uint64_t ssrHzbAddr = 0; // BDA of the MAX-reduce trace Hi-Z; 0 = no skip
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t gbufferTextureIndex = UINT32_MAX;
	uint32_t halfW = 0;
	uint32_t halfH = 0;
	float glossThreshold = 0.94f;
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f;
	float maxDistance = 50.0f;
	float thickness = 0.3f;
	float intensity = 1.0f;
	float f0 = 0.04f;
	uint32_t frameIndex = 0; // IGN dither epoch
	uint32_t mostDetailedMip = 1; // rough rays start here (mirrors start at 0)
	uint32_t maxIntersections = 24; // traversal loop cap
	uint32_t minOccupancy = 4; // wave early-out (non-mirror only)
	uint32_t ssrHzbMipCount = 0;
	// The F0-chroma layer 2 (gbuffer2, the RGBA8 route): the trace's
	// tinted-Fresnel source. UINT32_MAX = the flat-F0 fallback (the
	// layer absent / the G-buffer leg off) — today's behavior.
	uint32_t gbuffer2TextureIndex = UINT32_MAX;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsrIntersectPush) == 120);

// Must match ssr_copy.frag.hlsl SsrCopyPush (24 bytes) — the
// radiance-buffer -> raw blit.
struct SsrCopyPush {
	uint64_t radianceAddr = 0;
	uint32_t halfW = 0;
	uint32_t halfH = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsrCopyPush) == 24);

// Must match ssr.frag.hlsl SsrTracePush (80 bytes) — the legacy
// fullscreen trace pass (the new compute chain uses
// SsrClassifyPush/SsrIndirectPush/SsrIntersectPush). Two uint64
// BDAs (frame uniforms + trace Hi-Z) keep every 8-byte member
// aligned; pad0/pad1 land the block on a multiple of 8.
struct SsrTracePush {
	uint64_t frameUniforms = 0;
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t steps = 32;
	float invDepthW = 0.0f;
	float invDepthH = 0.0f;
	float maxDistance = 50.0f;
	float thickness = 0.3f;
	float intensity = 1.0f;
	float f0 = 0.04f;
	uint32_t refineSteps = 4;
	uint32_t frameIndex = 0;
	uint32_t gbufferTextureIndex = UINT32_MAX;
	// The F0-chroma layer 2 (see SsrIntersectPush's twin): the legacy
	// FS trace's tinted-Fresnel source. UINT32_MAX = the flat-F0
	// fallback. Unfilled — the legacy FS pass is unrecorded (the
	// compute chain owns the trace); the field keeps the mirrors
	// layout-identical.
	uint32_t gbuffer2TextureIndex = UINT32_MAX;
	uint64_t ssrHzb = 0;
	uint32_t ssrHzbMipCount = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsrTracePush) == 80);

// Must match ssr_blur.frag.hlsl SsrBlurPush (64 bytes).
// SSGI shared-HZB rework: every depth read (center block quad max + one
// rep per tap) comes from the final-depth MAX-HZB mip1 via BDA scalars —
// no depthTextureIndex, no depth image input. hzb* fields are REQUIRED
// (the shader dereferences hzb unconditionally); the app gates the chain
// on the HZB build instead. receiverData: the classify's packed receiver
// metadata (the SELECTED receiver's material + normal) — REQUIRED
// whenever gbufferTextureIndex is bound (the blur dereferences it for
// the radius bucket); the no-G-buffer path never touches it.
struct SsrBlurPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (nearPlane)
	uint64_t hzb = 0; // BDA of the MAX-reduce trace Hi-Z (mip1 reads)
	uint64_t receiverData = 0; // BDA of the packed receiver metadata (classify-selected)
	uint32_t reflTextureIndex = UINT32_MAX; // half-res raw reflection
	uint32_t samplerIndex = 0; // linear clamp (reflection taps only)
	float sharpness = 10.0f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	uint32_t hzbMip1Offset = 0; // float offset of mip1 in the HZB buffer
	uint32_t hzbMip1Stride = 0; // mip1 row stride (floats)
	uint32_t halfW = 0; // half-res extent (BDA tap clamp domain)
	uint32_t halfH = 0;
	// Thin G-buffer gate (the radius-bucket source moved to the receiver
	// buffer — this is the availability flag, not a texture read).
	// UINT32_MAX = no G-buffer: the receiver buffer holds the no-data
	// sentinel -> radius 2, the old 5x5 exactly.
	uint32_t gbufferTextureIndex = UINT32_MAX;
};
static_assert(sizeof(SsrBlurPush) == 64);

// Must match ssr_blur_separable.frag.hlsl SsrSeparableBlurPush (72
// bytes) — the H+V twin of SsrBlurPush: identical field order through
// gbufferTextureIndex, then the axis selector + pad. One compiled
// program serves both passes (axis is a push constant, not a shader
// variant). All-scalar layout on purpose (same straddling-vector
// hazard as SsrBlurPush). receiverData: the classify's packed receiver
// metadata (same contract as SsrBlurPush's).
struct SsrSeparableBlurPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (nearPlane)
	uint64_t hzb = 0; // BDA of the MAX-reduce trace Hi-Z (mip1 reads)
	uint64_t receiverData = 0; // BDA of the packed receiver metadata (classify-selected)
	uint32_t reflTextureIndex = UINT32_MAX; // half-res source (raw for H, blurTemp for V)
	uint32_t samplerIndex = 0; // linear clamp (reflection taps only)
	float sharpness = 10.0f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	uint32_t hzbMip1Offset = 0; // float offset of mip1 in the HZB buffer
	uint32_t hzbMip1Stride = 0; // mip1 row stride (floats)
	uint32_t halfW = 0; // half-res extent (BDA tap clamp domain)
	uint32_t halfH = 0;
	// Thin G-buffer gate (see SsrBlurPush's twin — the availability
	// flag; the material itself comes from the receiver buffer).
	uint32_t gbufferTextureIndex = UINT32_MAX;
	// 0 = horizontal (raw -> blurTemp), 1 = vertical (blurTemp -> blurred)
	uint32_t axis = 0;
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsrSeparableBlurPush) == 72);

// Must match ssr_compose.frag.hlsl SsrComposePush (88 bytes).
// SSGI shared-HZB rework: center depth from HZB mip0 (the exact 1:1
// depth copy), each tap block's representative from mip1
// (nearest-of-2x2 = the exact value the old per-tap 4-load quad max
// derived). No depthTextureIndex, no depth image input; hzb* REQUIRED.
// receiverData: the classify's packed receiver metadata (the upsample's
// tap normals) — REQUIRED while gbufferTextureIndex is bound.
// gbufferTextureIndex: the FULL-res thin G-buffer (the center normal +
// the output pixel's own material for the eligibility mask).
// glossThreshold: the classify's ray gate, reused as the composite's
// material-eligibility threshold (a matte dielectric output pixel must
// not receive its reflective neighbor's layer across a shared plane).
// UINT32_MAX on gbufferTextureIndex = depth-only (the MSAA/no-G-buffer
// fallback; eligibility stays 1).
struct SsrComposePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (nearPlane)
	uint64_t hzb = 0; // BDA of the MAX-reduce trace Hi-Z (mip0/mip1 reads)
	uint64_t receiverData = 0; // BDA of the packed receiver metadata (tap normals)
	uint32_t blurTextureIndex = UINT32_MAX; // half-res blurred reflection
	uint32_t hdrTextureIndex = UINT32_MAX; // full-res base HDR
	uint32_t samplerIndex = 0; // linear clamp (HDR + reflection taps)
	// The env-specular export (Main's third MRT, the AO-compose-scaled
	// version — ssgi's envSpec/envSpecComposed family): the EXACT IBL
	// slice the base holds that a confident hit REPLACES (the composite
	// formula base + conf*(layer - envSpec), the energy-conserving swap).
	// UINT32_MAX = the additive fallback (the export absent — the old
	// stacking composite).
	uint32_t envSpecTextureIndex = UINT32_MAX;
	float invDepthW = 0.0f; // 1 / full-res depth extent (NDC + vignette)
	float invDepthH = 0.0f;
	float sharpness = 10.0f; // upsample edge weight
	uint32_t fullW = 0; // full-res width (HZB mip0 row stride)
	uint32_t fullH = 0;
	uint32_t hzbMip1Offset = 0; // float offset of mip1 in the HZB buffer
	uint32_t hzbMip1Stride = 0; // mip1 row stride (floats)
	float vignetteWidth = 0.15f; // NDC edge fade width (outer % of NDC range)
	float glossThreshold = 0.94f; // the classify's ray gate (eligibility mask)
	uint32_t gbufferTextureIndex = UINT32_MAX; // full-res thin G-buffer (normal + material)
	// The composite diagnostic mode (SsrComposeDebug) — consumes pad0's
	// slot, keeping the block at 88 bytes.
	uint32_t debugMode = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsrComposePush) == 88);

// Temporal (TAA) state for the SSR reflection layer: one per renderer,
// advanced once per recorded frame (record cadence, never per-image —
// same discipline as SmaaT2xState). parity selects the history slot the
// next record writes; historyValid gates the blend (false = warmup
// passthrough of the current blurred tap, which still fills the slot).
struct SsrTaaState {
	uint32_t parity = 0;
	bool historyValid = false;
};

struct SsrTaaFrameParams {
	uint32_t parity = 0;
	bool historyValid = false;
};

// Record-time snapshot (runs before Execute). Zeros when TAA is off.
inline SsrTaaFrameParams SsrTaaBeginFrame(const SsrTaaState& st, bool taaActive) {
	SsrTaaFrameParams p{};
	if (!taaActive)
		return p;
	p.parity = st.parity & 1u;
	p.historyValid = st.historyValid;
	return p;
}

// End-of-record step (runs after Execute). ranTemporal mirrors the
// temporal-emission gate: a non-temporal record freezes the phase and
// invalidates, so the next temporal record warms up instead of blending
// against stale history.
inline void SsrTaaEndFrame(SsrTaaState& st, bool ranTemporal) {
	if (!ranTemporal) {
		st.historyValid = false;
		return;
	}
	st.historyValid = true;
	st.parity ^= 1u;
}

// Explicit invalidation for history-content loss the toggle cannot see:
// extent/format drift (targets recreated) and motion-vector loss. Resets
// parity so the ping-pong restarts at slot 0.
inline void SsrTaaInvalidateHistory(SsrTaaState& st) {
	st.historyValid = false;
	st.parity = 0;
}

// Must match ssr_temporal.frag.hlsl SsrTemporalPush (72 bytes).
// All-scalar layout on purpose: float2 members would land at offsets
// that are not 8-byte aligned — spirv-val rejects the module
// ("improperly straddling vector"), same hazard as SsrBlurPush.
// prevDepthTextureIndex: the previous record's depth mirror (the
// ssr_depth_history.frag.hlsl copy this chain emits per temporal
// record) — the disocclusion test's history side. UINT32_MAX = the
// current-depth approximation (the old behavior, hosts without the
// mirror).
struct SsrTemporalPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (exact view distances)
	uint32_t currTextureIndex = UINT32_MAX; // half-res blurred reflection (this frame)
	uint32_t historyTextureIndex = UINT32_MAX; // half-res temporal history (previous record)
	uint32_t motionTextureIndex = UINT32_MAX; // render-extent RG16F motion (prevUV - currUV)
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (authority + disocclusion)
	uint32_t prevDepthTextureIndex = UINT32_MAX; // previous record's depth mirror (history side)
	uint32_t samplerIndex = 0; // linear clamp
	uint32_t historyValid = 0; // 0 = warmup passthrough of curr
	float jitterDeltaX = 0.0f; // (prevJitter - currJitter) UV (attenuation only)
	float jitterDeltaY = 0.0f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	float blend = 0.9f; // history weight 0..0.98 (before attenuation)
	float sharpness = 10.0f; // depth edge weight (higher = tighter)
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsrTemporalPush) == 72);

// Must match ssr_depth_history.frag.hlsl SsrDepthHistoryPush (8 bytes)
// — the renderDepth -> prevDepth R32F mirror copy (one fullscreen
// pass per temporal record; see the shader header).
struct SsrDepthHistoryPush {
	uint32_t depthTextureIndex = UINT32_MAX; // full-res final depth
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsrDepthHistoryPush) == 8);

// Default SPVs (fullscreen-triangle VS shared with tonemap + SSR PS
// trio; the trace FS is GONE — replaced by the compute chain below).
inline constexpr const char* kSsrVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsrBlurPsPath = "content/shaders/ssr_blur.frag.hlsl.spv";
// The separable H+V twin (SsrBlurMode::SeparableHV): one program, the
// axis via push constant (see SsrSeparableBlurPush).
inline constexpr const char* kSsrSeparableBlurPsPath =
	"content/shaders/ssr_blur_separable.frag.hlsl.spv";
inline constexpr const char* kSsrTemporalPsPath = "content/shaders/ssr_temporal.frag.hlsl.spv";
inline constexpr const char* kSsrComposePsPath = "content/shaders/ssr_compose.frag.hlsl.spv";
// Debug twin of the compose PS (SSR_DEBUG_CONTRIB): writes the exact add
// term to SV_Target1. Loaded tolerantly (a missing .dbg.spv leaves the
// debug output unavailable - the chain keeps the normal program; the
// composite falls back to the single attachment when unarmed).
inline constexpr const char* kSsrComposeDbgPsPath =
	"content/shaders/ssr_compose.frag.hlsl.dbg.spv";
inline constexpr const char* kSsrClassifyCsPath = "content/shaders/ssr_classify.cs.hlsl.spv";
inline constexpr const char* kSsrIndirectCsPath = "content/shaders/ssr_prepare_indirect.cs.hlsl.spv";
inline constexpr const char* kSsrIntersectCsPath = "content/shaders/ssr_intersect.cs.hlsl.spv";
inline constexpr const char* kSsrCopyPsPath = "content/shaders/ssr_copy.frag.hlsl.spv";
// The temporal depth-history copy (renderDepth -> the prevDepth R32F
// mirror, one pass per temporal record; see ssr_depth_history.frag.hlsl).
inline constexpr const char* kSsrDepthHistoryPsPath =
	"content/shaders/ssr_depth_history.frag.hlsl.spv";

struct ScreenSpaceReflections {
	// Half-res target helper (raw + blurred reflection share the layout).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	// Compute-chain buffer helper (the classify->args->intersect
	// handoff: storage, BDA-addressed, barriered via logical hints).
	struct CBuf {
		Holder<BufferHandle> buffer;
		LogicalBufferHandle logical{};
		[[nodiscard]] bool Valid() const { return !buffer.empty() && !logical.empty(); }
	};

	Target raw; // half-res trace output (rgb = refl x fresnel x intensity, a = confidence)
	Target blurred; // half-res bilateral blur of raw
	// The separable-blur intermediate (SsrBlurMode::SeparableHV):
	// SSRBlurH writes it, SSRBlurV reads it and writes blurred. Same
	// half-res RGBA16F layout as raw/blurred. Allocated in BOTH modes —
	// a live mode flip needs no resource rebuild (the Ensure gate does
	// not watch blurMode), ~4 MiB at 1080p.
	Target blurTemp;
	// SSSR-style thin G-buffer (see gbuffer.frag.hlsl): full-res RGBA8
	// (RG = octahedral world normal, B = roughness, A = metal), written
	// by the APP's Forward+ depth prepass (the G-buffer FS pair rides a
	// pass that already rasterizes every opaque surface — Main is
	// untouched). The trace reads REAL interpolated normals (no
	// depth-derived receiver normals — the piecewise-planar wireframe
	// source is gone by construction) + continuous roughness + metal.
	// Clear (0.5, 0.5, 1, 0) = "no data" decodes as +Z/matte/non-metal
	// (a safe non-reflector); temporal-split frames Load last frame's
	// values (static normals never change).
	Target gbuffer;
	// The F0-CHROMA layer 2 (gbuffer.frag.hlsl's SV_Target1): full-res
	// RGBA8 (RGB = the metal F0 tint — baseColorFactor x the baseColor
	// texel, so the gold brocade's reflections come out GOLD, not the
	// white-mirror flat F0=1; A = materialId/255, the palette door for
	// future per-material LUT lookups). Rides the SAME prepass leg
	// (always 1x, even under MSAA — no MSAA twin). OPTIONAL (not in
	// TargetsValid): the trace falls back to the flat F0=1 (today's
	// behavior) when unbound. Clear (1,1,1,0) = white chroma +
	// material 0 = the flat behavior.
	Target gbuffer2;
	// Half-res temporal history ping-pong (TAA of the reflection layer).
	// history[parity] is written by the temporal pass each temporal
	// record; the composite samples the just-written slot. historyValid
	// (SsrTaaState, owned by the app) gates the blend — a warmup record
	// fills the slot with the current blurred tap instead.
	Target history[2];
	// The PREVIOUS record's depth mirror (the temporal disocclusion
	// test's history side): full-res R32F, copied from the final
	// renderDepth by the SSRDepthHistory pass at the end of every
	// TEMPORAL record (see ssr_depth_history.frag.hlsl). historyValid
	// implies the previous record was temporal (SsrTaaEndFrame
	// invalidates otherwise), so the mirror is always exactly one
	// record old when the test reads it; the warmup record never reads
	// it. Same cross-frame image cycle as the history ping-pong.
	Target prevDepth;
	// Compute-chain handoff buffers (the SSSR classify->args->intersect
	// chain): rayList (packed uint per ray, halfW*halfH capacity),
	// rayCounter ([0] live count / [1] frozen count), indirectArgs (the
	// VkDispatchIndirectCommand), radiance (float4 per half-res pixel),
	// receiverData (the classify's packed receiver metadata: one uint
	// per half-res pixel — the SELECTED receiver's thin G-buffer sample
	// every consumer (blur radius bucket + the filters' normals) reads,
	// so no stage re-does the block's 4-tap depth scan; see
	// ssr_receiver.hlsli).
	CBuf rayList;
	CBuf rayCounter;
	CBuf indirectArgs;
	CBuf radiance;
	CBuf receiverData;
	// Full-res composite output (base HDR + upsampled reflection). These
	// keep the historic field names so downstream code (DLSS color input,
	// status UI) is untouched.
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 }; // full-res composite extent (= depth/HDR extent)
	Extent2D halfExtent{ 0, 0 }; // max(1, full / 2) per axis
	Format format = Format::RGBA16_Float;
	// Debug contribution attachment (desc.debugContrib, the .dbg compose
	// variant): full-res RGBA16F, carries the composite's exact ADD term
	// (ssr_compose.frag.hlsl's outContrib). Created in Create() only while
	// the flag is on; OPTIONAL everywhere (TargetsValid excludes it - the
	// chain stays valid without it).
	Target contrib;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> blurFragModule;
	Holder<ShaderModuleHandle> separableBlurFragModule; // the H+V twin (axis via push)
	Holder<ShaderModuleHandle> temporalFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	Holder<ShaderModuleHandle> composeDbgFragModule; // the .dbg twin (optional)
	Holder<ShaderModuleHandle> classifyModule;
	Holder<ShaderModuleHandle> indirectModule;
	Holder<ShaderModuleHandle> intersectModule;
	Holder<ShaderModuleHandle> copyFragModule;
	Holder<ShaderModuleHandle> depthHistoryFragModule; // the prevDepth mirror copy
	ShaderProgram blurProgram{};
	ShaderProgram separableBlurProgram{}; // H+V twin (one program, axis via push)
	ShaderProgram temporalProgram{};
	ShaderProgram composeProgram{};
	ShaderProgram composeDbgProgram{}; // optional (missing .dbg.spv -> empty)
	ShaderProgram classifyProgram{}; // compute, 8x8 groups
	ShaderProgram indirectProgram{}; // compute, 1x1x1
	ShaderProgram intersectProgram{}; // compute, indirect 8x8 groups
	ShaderProgram copyProgram{}; // fullscreen FS (tonemap VS + the copy PS)
	ShaderProgram depthHistoryProgram{}; // fullscreen FS (tonemap VS + the depth-mirror PS)
	SsrDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		// gbuffer2 rides the gbuffer family's lifecycle (created beside it
		// in Create, cascade-destroyed together) - no separate optionality.
		return raw.Valid() && blurred.Valid() && blurTemp.Valid() &&
			gbuffer.Valid() &&
			gbuffer2.Valid() &&
			history[0].Valid() && history[1].Valid() && prevDepth.Valid() &&
			rayList.Valid() && rayCounter.Valid() && indirectArgs.Valid() &&
			radiance.Valid() && receiverData.Valid() &&
			!image.empty() && !logicalView.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return classifyProgram.shaderCount != 0 && indirectProgram.shaderCount != 0 &&
			intersectProgram.shaderCount != 0 && copyProgram.shaderCount != 0 &&
			blurProgram.shaderCount != 0 && separableBlurProgram.shaderCount != 0 &&
			temporalProgram.shaderCount != 0 && composeProgram.shaderCount != 0 &&
			depthHistoryProgram.shaderCount != 0 &&
			!vsModule.empty() && !classifyModule.empty() && !indirectModule.empty() &&
			!intersectModule.empty() && !copyFragModule.empty() &&
			!blurFragModule.empty() && !separableBlurFragModule.empty() &&
			!temporalFragModule.empty() && !composeFragModule.empty() &&
			!depthHistoryFragModule.empty();
		// (composeDbgProgram deliberately excluded: it is the OPTIONAL
		// debug twin - a missing .dbg.spv must not kill the chain; the
		// composite pass checks it before binding.)
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	static Extent2D HalfExtentFor(Extent2D full) {
		return {
			std::max(1u, full.width / 2),
			std::max(1u, full.height / 2),
		};
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kSsrVsPath,
		const char* blurPath = kSsrBlurPsPath,
		const char* temporalPath = kSsrTemporalPsPath,
		const char* composePath = kSsrComposePsPath,
		const char* classifyPath = kSsrClassifyCsPath,
		const char* indirectPath = kSsrIndirectCsPath,
		const char* intersectPath = kSsrIntersectCsPath,
		const char* copyPath = kSsrCopyPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSsrVsPath;
			const std::string blur = blurPath ? blurPath : kSsrBlurPsPath;
			const std::string temporal = temporalPath ? temporalPath : kSsrTemporalPsPath;
			const std::string compose = composePath ? composePath : kSsrComposePsPath;
			const std::string classify = classifyPath ? classifyPath : kSsrClassifyCsPath;
			const std::string indirect = indirectPath ? indirectPath : kSsrIndirectCsPath;
			const std::string intersect = intersectPath ? intersectPath : kSsrIntersectCsPath;
			const std::string copy = copyPath ? copyPath : kSsrCopyPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "ssrVert");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(blur), "ssrBlurFrag");
			// Separable H+V twin: loaded from the fixed path (no parameter
			// churn — same discipline as the compose .dbg twin) in BOTH
			// modes so a live mode flip never needs a shader load.
			if (separableBlurFragModule.empty())
				separableBlurFragModule =
					ctx.CreateShaderModule(readFile(kSsrSeparableBlurPsPath), "ssrSeparableBlurFrag");
			if (temporalFragModule.empty())
				temporalFragModule = ctx.CreateShaderModule(readFile(temporal), "ssrTemporalFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(readFile(compose), "ssrComposeFrag");
			// Debug twin (optional, tolerant): a missing/stale .dbg.spv must
			// NOT kill the chain - ShadersValid excludes it and the composite
			// pass falls back to the single attachment when it is empty.
			if (composeDbgFragModule.empty()) {
				try {
					composeDbgFragModule =
						ctx.CreateShaderModule(readFile(kSsrComposeDbgPsPath), "ssrComposeDbgFrag");
				} catch (...) {
					std::fprintf(stderr,
						"[SSR] compose .dbg twin unavailable (missing %s) "
						"- the contribution debug attachment stays off\n",
						kSsrComposeDbgPsPath);
				}
			}
			if (classifyModule.empty())
				classifyModule = ctx.CreateShaderModule(readFile(classify), "ssrClassifyCS");
			if (indirectModule.empty())
				indirectModule = ctx.CreateShaderModule(readFile(indirect), "ssrIndirectCS");
			if (intersectModule.empty())
				intersectModule = ctx.CreateShaderModule(readFile(intersect), "ssrIntersectCS");
			if (copyFragModule.empty())
				copyFragModule = ctx.CreateShaderModule(readFile(copy), "ssrCopyFrag");
			// The temporal depth-history copy: fixed path (the compose
			// .dbg twin's discipline), loaded unconditionally — the
			// temporal path needs it every record.
			if (depthHistoryFragModule.empty())
				depthHistoryFragModule =
					ctx.CreateShaderModule(readFile(kSsrDepthHistoryPsPath), "ssrDepthHistoryFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || blurFragModule.empty() ||
			separableBlurFragModule.empty() ||
			temporalFragModule.empty() || composeFragModule.empty() ||
			classifyModule.empty() || indirectModule.empty() ||
			intersectModule.empty() || copyFragModule.empty() ||
			depthHistoryFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader blurPS{ .module = blurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader separableBlurPS{ .module = separableBlurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader temporalPS{ .module = temporalFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader composePS{ .module = composeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader classifyCS{ .module = classifyModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader indirectCS{ .module = indirectModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader intersectCS{ .module = intersectModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader copyPS{ .module = copyFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader depthHistoryPS{ .module = depthHistoryFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		blurProgram = { .shaders = { vs, blurPS }, .shaderCount = 2 };
		separableBlurProgram = { .shaders = { vs, separableBlurPS }, .shaderCount = 2 };
		temporalProgram = { .shaders = { vs, temporalPS }, .shaderCount = 2 };
		composeProgram = { .shaders = { vs, composePS }, .shaderCount = 2 };
		if (!composeDbgFragModule.empty()) {
			Shader composeDbgPS{ .module = composeDbgFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
			composeDbgProgram = { .shaders = { vs, composeDbgPS }, .shaderCount = 2 };
		}
		classifyProgram = { .shaders = { classifyCS }, .shaderCount = 1 };
		indirectProgram = { .shaders = { indirectCS }, .shaderCount = 1 };
		intersectProgram = { .shaders = { intersectCS }, .shaderCount = 1 };
		copyProgram = { .shaders = { vs, copyPS }, .shaderCount = 2 };
		depthHistoryProgram = { .shaders = { vs, depthHistoryPS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		blurProgram = ShaderProgram();
		separableBlurProgram = ShaderProgram();
		temporalProgram = ShaderProgram();
		composeProgram = ShaderProgram();
		composeDbgProgram = ShaderProgram();
		classifyProgram = ShaderProgram();
		indirectProgram = ShaderProgram();
		intersectProgram = ShaderProgram();
		copyProgram = ShaderProgram();
		depthHistoryProgram = ShaderProgram();
		vsModule = {};
		blurFragModule = {};
		separableBlurFragModule = {};
		temporalFragModule = {};
		composeFragModule = {};
		composeDbgFragModule = {};
		classifyModule = {};
		indirectModule = {};
		intersectModule = {};
		copyFragModule = {};
		depthHistoryFragModule = {};
	}

	static bool CreateTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D ext, Format format, const char* name, Target& out) {
		if (ext.width == 0 || ext.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { ext.width, ext.height, 1 };
		ci.format = format;
		// TransferSrc: debug stage dumps (TOURMALINE_SSR_DUMP) copy the
		// targets back through DownloadDataFromImage — armed dumps only.
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = name;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, format, name);
		if (newLogical.empty())
			return false;

		Target t;
		t.image = std::move(newImage);
		t.view = std::move(newView);
		t.bindless = std::move(newBindless);
		t.bindlessIndex = newBindlessIndex;
		t.logical = newLogical;
		out = std::move(t);
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
		return true;
	}

	static void DestroyTarget(RenderGraph& rg, Target& t) {
		if (!t.logical.empty())
			rg.FreeImageView(t.logical);
		t = Target{};
	}

	static bool CreateBuffer(ResourceManager& ctx, RenderGraph& rg,
		size_t bytes, const char* name, CBuf& out) {
		if (bytes == 0)
			return false;
		Holder<BufferHandle> newBuffer =
			ctx.CreateBuffer(bytes, BufferType_Storage, name);
		if (newBuffer.empty())
			return false;
		LogicalBufferHandle newLogical = rg.AddExternalBuffer(newBuffer, name);
		if (newLogical.empty())
			return false;
		CBuf b;
		b.buffer = std::move(newBuffer);
		b.logical = newLogical;
		out = std::move(b);
		return true;
	}

	static void DestroyBuffer(RenderGraph& rg, CBuf& b) {
		if (!b.logical.empty())
			rg.FreeBuffer(b.logical);
		b = CBuf{};
	}

	void DestroyBuffers(RenderGraph& rg) {
		DestroyBuffer(rg, rayList);
		DestroyBuffer(rg, rayCounter);
		DestroyBuffer(rg, indirectArgs);
		DestroyBuffer(rg, radiance);
		DestroyBuffer(rg, receiverData);
	}

	bool CreateTarget(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const SsrDesc& d) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		DestroyTarget(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = d.format;
		// TransferSrc: debug stage dumps (TOURMALINE_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = d.name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = d.viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, d.format, d.logicalName);
		if (newLogical.empty())
			return false;

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = targetExtent;
		format = d.format;
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
		return true;
	}

	void DestroyTarget(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindlessIndex = UINT32_MAX;
		bindless = {};
		view = {};
		image = {};
		extent = { 0, 0 };
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SsrDesc& d,
		const char* vsPath = kSsrVsPath,
		const char* blurPath = kSsrBlurPsPath,
		const char* temporalPath = kSsrTemporalPsPath,
		const char* composePath = kSsrComposePsPath,
		const char* classifyPath = kSsrClassifyCsPath,
		const char* indirectPath = kSsrIndirectCsPath,
		const char* intersectPath = kSsrIntersectCsPath,
		const char* copyPath = kSsrCopyPsPath) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		DestroyBuffers(rg);
		const Extent2D half = HalfExtentFor(full);
		Target newRaw, newBlur, newGbuffer, newGbuffer2, newHist0, newHist1;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrRaw", newRaw))
			return false;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrBlur", newBlur)) {
			DestroyTarget(rg, newRaw);
			return false;
		}
		// SSSR thin G-buffer: full-res RGBA8 (oct normal + roughness +
		// metal — see gbuffer.frag.hlsl). The depth prepass attaches it.
		if (!CreateTarget(ctx, rg, full, Format::RGBA8_UNorm,
				"ssrGbuffer", newGbuffer)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			return false;
		}
		// The F0-CHROMA layer 2 (the target block's comment): RGBA8,
		// full-res, created beside the gbuffer (OPTIONAL at consume —
		// the trace's flat-F0 fallback when the pass plumbing passes it
		// unbound; the target itself is cheap and rides the gbuffer
		// family's lifecycle).
		if (!CreateTarget(ctx, rg, full, Format::RGBA8_UNorm,
				"ssrGbuffer2", newGbuffer2)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			return false;
		}
		if (!CreateTarget(ctx, rg, half, d.format, "ssrHist0", newHist0)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			return false;
		}
		if (!CreateTarget(ctx, rg, half, d.format, "ssrHist1", newHist1)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			return false;
		}
		// Debug contribution attachment (desc.debugContrib): full-res,
		// same format as the composite. Optional - only while the flag is
		// on (the Ensure guard recreates the chain on flips).
		Target newContrib;
		if (d.debugContrib &&
				!CreateTarget(ctx, rg, full, d.format, "ssrContrib", newContrib)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			return false;
		}
		// Compute-chain handoff buffers (the classify->args->intersect
		// chain): rayList (packed uint per ray, worst case = every
		// half-res pixel shoots), rayCounter ([0] live / [1] frozen),
		// indirectArgs (the VkDispatchIndirectCommand), radiance
		// (float4 per half-res pixel).
		const size_t rayCount = (size_t)std::max(1u, half.width)
			* (size_t)std::max(1u, half.height);
		CBuf newRayList, newRayCounter, newIndirectArgs, newRadiance;
		if (!CreateBuffer(ctx, rg, rayCount * sizeof(uint32_t),
				"ssrRayList", newRayList)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			return false;
		}
		if (!CreateBuffer(ctx, rg, 2u * sizeof(uint32_t),
				"ssrRayCounter", newRayCounter)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			DestroyBuffer(rg, newRayList);
			return false;
		}
		// Creation zero-fill of the counter (the ssgi-hzb / placement-count
		// convention): the classify's wave-compaction InterlockedAdd appends
		// FROM count[0] — the frame-to-frame invariant is the args pass's
		// read-then-reset, but the FIRST frame on a fresh buffer starts from
		// whatever the DEVICE_LOCAL allocation recycled. The extent churn's
		// recreate can hand back the OLD buffer's pages — old count[1] (the
		// frozen count, hundreds of thousands of rays) landing in the new
		// count[0]: the classify appends past rayList capacity and the
		// compute shader MMU-faults (the editor drag-burst DEVICE_LOST —
		// pinned by the diagnostic checkpoint dying in SSRClassify, right
		// after the extent re-ensure). 8 bytes of staging buys the invariant
		// at the only gap in the counter's lifecycle.
		{
			const uint32_t counterZeros[2] = { 0, 0 };
			ctx.UploadDataToBuffer(newRayCounter.buffer, counterZeros,
				sizeof(counterZeros), PipelineStage(PipelineStageBit_Compute),
				AccessFlags::ShaderRead | AccessFlags::ShaderWrite);
		}
		if (!CreateBuffer(ctx, rg, 3u * sizeof(uint32_t),
				"ssrIndirectArgs", newIndirectArgs)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			DestroyBuffer(rg, newRayList);
			DestroyBuffer(rg, newRayCounter);
			return false;
		}
		if (!CreateBuffer(ctx, rg, rayCount * 4u * sizeof(float),
				"ssrRadiance", newRadiance)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			DestroyBuffer(rg, newRayList);
			DestroyBuffer(rg, newRayCounter);
			DestroyBuffer(rg, newIndirectArgs);
			return false;
		}
		// Receiver metadata (see the receiverData field comment): one
		// packed uint per half-res pixel, fully rewritten by the classify
		// every frame before any read — no creation zero-fill needed
		// (unlike the ray counter's InterlockedAdd append contract).
		CBuf newReceiverData;
		if (!CreateBuffer(ctx, rg, rayCount * sizeof(uint32_t),
				"ssrReceiverData", newReceiverData)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			DestroyBuffer(rg, newRayList);
			DestroyBuffer(rg, newRayCounter);
			DestroyBuffer(rg, newIndirectArgs);
			DestroyBuffer(rg, newRadiance);
			return false;
		}
		// Separable-blur intermediate (see the blurTemp field comment):
		// created LAST so every earlier failure block stays untouched —
		// one new failure block instead of one per earlier resource.
		// Allocated in BOTH blur modes (a live mode flip must not
		// rebuild the targets).
		Target newBlurTemp;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrBlurTemp", newBlurTemp)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			DestroyBuffer(rg, newRayList);
			DestroyBuffer(rg, newRayCounter);
			DestroyBuffer(rg, newIndirectArgs);
			DestroyBuffer(rg, newRadiance);
			DestroyBuffer(rg, newReceiverData);
			return false;
		}
		// The temporal disocclusion test's depth mirror (see the
		// prevDepth field comment): full-res R32F, written once per
		// TEMPORAL record by the SSRDepthHistory pass. Rides the same
		// last-created/failure-block pattern (~8 MB at 1080p).
		Target newPrevDepth;
		if (!CreateTarget(ctx, rg, full, Format::R32_Float,
				"ssrPrevDepth", newPrevDepth)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newGbuffer2);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg);
			DestroyTarget(rg, newContrib);
			DestroyBuffer(rg, newRayList);
			DestroyBuffer(rg, newRayCounter);
			DestroyBuffer(rg, newIndirectArgs);
			DestroyBuffer(rg, newRadiance);
			DestroyBuffer(rg, newReceiverData);
			DestroyTarget(rg, newBlurTemp);
			return false;
		}
		raw = std::move(newRaw);
		blurred = std::move(newBlur);
		blurTemp = std::move(newBlurTemp);
		prevDepth = std::move(newPrevDepth);
		gbuffer = std::move(newGbuffer);
		gbuffer2 = std::move(newGbuffer2);
		history[0] = std::move(newHist0);
		history[1] = std::move(newHist1);
		contrib = std::move(newContrib);
		rayList = std::move(newRayList);
		rayCounter = std::move(newRayCounter);
		indirectArgs = std::move(newIndirectArgs);
		radiance = std::move(newRadiance);
		receiverData = std::move(newReceiverData);
		halfExtent = half;
		format = d.format;
		desc = d;
		// Extent log (the dark-triangle hunt's dimension checklist):
		// one line per (re)creation — a resize recreates, so the broken
		// window size prints its numbers here. Eyeball against the
		// [exposure]/[Scene] extent lines: full must equal the HDR/depth
		// extent, half must equal max(1, full/2) exactly.
		std::fprintf(stderr, "[SSR] targets: full %ux%u -> half %ux%u (%s)\n",
			full.width, full.height, half.width, half.height, d.name);
		return EnsureShaders(ctx, vsPath, blurPath, temporalPath, composePath,
			classifyPath, indirectPath, intersectPath, copyPath);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, raw);
		DestroyTarget(rg, blurred);
		DestroyTarget(rg, blurTemp);
		DestroyTarget(rg, gbuffer);
		DestroyTarget(rg, gbuffer2);
		DestroyTarget(rg, history[0]);
		DestroyTarget(rg, history[1]);
		DestroyTarget(rg, prevDepth);
		DestroyTarget(rg);
		DestroyTarget(rg, contrib);
		halfExtent = { 0, 0 };
	}

	// Recreate only when full extent / format drift. Live-updates desc
	// (steps / distance / intensity need no realloc). Returns true when ready.
	// A debugContrib FLIP also recreates (the ssrContrib target exists only
	// while the flag is on - an 8 MB full-res RGBA16F is not worth keeping
	// around unarmed, and the composite binds the 2-attachment dbg variant
	// only while it is).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		const SsrDesc& d,
		const char* vsPath = kSsrVsPath,
		const char* blurPath = kSsrBlurPsPath,
		const char* temporalPath = kSsrTemporalPsPath,
		const char* composePath = kSsrComposePsPath,
		const char* classifyPath = kSsrClassifyCsPath,
		const char* indirectPath = kSsrIndirectCsPath,
		const char* intersectPath = kSsrIntersectCsPath,
		const char* copyPath = kSsrCopyPsPath) {
		if (TargetsValid() &&
				extent.width == targetExtent.width && extent.height == targetExtent.height &&
				format == d.format &&
				d.debugContrib == desc.debugContrib) {
			desc = d;
			return EnsureShaders(ctx, vsPath, blurPath, temporalPath, composePath,
				classifyPath, indirectPath, intersectPath, copyPath);
		}
		return Create(ctx, rg, targetExtent, d, vsPath, blurPath, temporalPath, composePath,
			classifyPath, indirectPath, intersectPath, copyPath);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyBuffers(rg);
		DestroyShaders();
	}

// Blur-pass emission shared by the spatial + temporal recording paths
// (they used to carry two identical copies of this block — one fix now
// lands in both). Bilateral2D keeps the original single-pass SSRBlur
// (raw -> blurred) EXACTLY as before. SeparableHV runs the same
// bilateral weights as two 1D passes — SSRBlurH (raw -> blurTemp) then
// SSRBlurV (blurTemp -> blurred): 2*(2r+1) taps/px across both vs the
// 2D kernel's (2r+1)^2 (10/14/18 vs 25/49/81 at the radius 2/3/4
// buckets). Both 1D passes weight their taps against the ORIGINAL
// scene HZB mip1 depths (never the intermediate image's — see the
// shader header), and both carry the HZB + receiver-data buffer-read
// hints the 2D pass has (BDA reads are invisible to the graph without
// them — the classify's receiver writes must be visible before these
// fragment reads). The blur no longer SAMPLES the G-buffer texture
// (the receiver material comes from the receiver-data buffer), so
// gbufferView left the input set — gbufferBindlessIndex stays as the
// push's availability gate. The V pass reads blurTemp as an input
// image so the graph owns the color-attachment-write ->
// shader-read transition. Names stay the fixed SSRBlurH/SSRBlurV in
// the separable branch (GPU-timeline A/B readability); blurName only
// names the 2D baseline pass.
void AddSsrBlurPasses(RenderGraph& rg,
	LogicalImageViewHandle gbufferView,
	uint32_t gbufferBindlessIndex,
	uint64_t hzbAddr,
	LogicalBufferHandle hzbLogical,
	uint64_t receiverDataAddr,
	LogicalBufferHandle receiverDataLogical,
	uint32_t hzbMip1Offset,
	uint32_t hzbMip1Stride,
	uint64_t frameUniformsAddr,
	uint32_t linearSamplerIndex,
	const SsrDesc& overrideDesc,
	const char* blurName) const {
	const bool gbufferOn = !gbufferView.empty() && gbufferBindlessIndex != UINT32_MAX;
	const float halfW = static_cast<float>(std::max(1u, halfExtent.width));
	const float halfH = static_cast<float>(std::max(1u, halfExtent.height));

	if (overrideDesc.blurMode == SsrBlurMode::SeparableHV) {
		for (uint32_t axis = 0; axis < 2; ++axis) {
			const bool horizontal = (axis == 0u);
			const Target& source = horizontal ? raw : blurTemp;
			const Target& destination = horizontal ? blurTemp : blurred;

			SsrSeparableBlurPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hzb = hzbAddr;
			push.receiverData = receiverDataAddr;
			push.reflTextureIndex = source.bindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.texelX = 1.0f / halfW;
			push.texelY = 1.0f / halfH;
			push.hzbMip1Offset = hzbMip1Offset;
			push.hzbMip1Stride = hzbMip1Stride;
			push.halfW = halfExtent.width;
			push.halfH = halfExtent.height;
			push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
			push.axis = axis;

			RenderPass& pass = rg.AddPass(horizontal ? "SSRBlurH" : "SSRBlurV");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ source.logical })
				.SetColor({ destination.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			{
				// HZB buffer read (the mip1 reps — the SAME original-scene
				// depths in both passes): barrier the SsrHzb compute
				// writes before these fragment reads.
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(hzbLogical, hzbRead);
				// Receiver metadata read (the radius-bucket material):
				// barrier the classify's compute writes.
				BufferSyncRequest recvRead{};
				recvRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				recvRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(receiverDataLogical, recvRead);
			}
			ShaderProgram progCopy = separableBlurProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrSeparableBlurPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
		return;
	}

	// ---- Original 2D bilateral (the quality baseline, unchanged) ----
	{
		SsrBlurPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.hzb = hzbAddr;
		push.receiverData = receiverDataAddr;
		push.reflTextureIndex = raw.bindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
		push.texelX = 1.0f / halfW;
		push.texelY = 1.0f / halfH;
		push.hzbMip1Offset = hzbMip1Offset;
		push.hzbMip1Stride = hzbMip1Stride;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;

		RenderPass& pass = rg.AddPass(blurName ? blurName : "SSRBlur");
		pass.SetOffset(0, 0)
			.SetExtent(halfExtent)
			.SetInputImages({ raw.logical })
			.SetColor({ blurred.logical },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
		{
			// HZB buffer read (the mip1 reps): the SsrHzb compute writes
			// must be barriered before these fragment reads.
			BufferSyncRequest hzbRead{};
			hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(hzbLogical, hzbRead);
			// Receiver metadata read (the radius-bucket material):
			// barrier the classify's compute writes.
			BufferSyncRequest recvRead{};
			recvRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			recvRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(receiverDataLogical, recvRead);
		}
		ShaderProgram progCopy = blurProgram; // copyable (handles only)
		pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexLayoutId::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::Opaque(),
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(SsrBlurPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
	}
}

// Emits trace (HDR + full-res depth [+ stencil] -> half-res reflection)
// + blur (half-res reflection + full-res depth -> half-res) + composite
// (half-res reflection + full-res depth + full-res HDR -> full-res HDR).
// gbufferView/gbufferBindlessIndex: the thin G-buffer (RGBA8 — oct
// normal + roughness + metal, see gbuffer.frag.hlsl) written by the
// app's Forward+ depth prepass. Empty view / invalid index = legacy
// mirror (perfect-mirror uniform-F0).
// hzbAddr/hzbMips/hzbLogical: the MAX-reduce trace Hi-Z (see
// hzb_max.cs.hlsl — the NEAREST-surface pyramid over the complete
// renderDepth, built by the app AFTER all depth passes). 0/0/empty =
// the plain adaptive-stride linear march (the empty-space skip stays
// off). hzbLogical carries the buffer READ dependency so the graph
// barriers the SsrHzb compute writes before the trace's fragment
// reads — without it there is no ordering/visibility edge and the
// trace races the pyramid build.
// Captures only copyable handles/indices, so the passes are safe once
// this call returns.
void AddSsrPasses(RenderGraph& rg,
	LogicalImageViewHandle hdrView,
	uint32_t hdrBindlessIndex,
	LogicalImageViewHandle depthView,
	uint32_t depthBindlessIndex,
	LogicalImageViewHandle gbufferView = LogicalImageViewHandle{},
	uint32_t gbufferBindlessIndex = UINT32_MAX,
	// The F0-chroma layer 2 (gbuffer2 — gbuffer.frag.hlsl's
	// SV_Target1: RGB = the metal F0 tint, A = materialId/255): the
	// trace's tinted-Fresnel source (the gold brocade's reflections
	// come out gold). Rides the SAME prepass leg as the gbuffer pair
	// above. Empty/MAX = the flat-F0 fallback (today's behavior).
	LogicalImageViewHandle gbuffer2View = LogicalImageViewHandle{},
	uint32_t gbuffer2BindlessIndex = UINT32_MAX,
	// The env-specular export (the replacement-composite contract, see
	// SsrComposePush::envSpecTextureIndex): the AO-compose-scaled env-spec
	// slice — the EXACT IBL the confident hit replaces. Empty/MAX = the
	// additive fallback (the old stacking composite).
	LogicalImageViewHandle envSpecView = LogicalImageViewHandle{},
	uint32_t envSpecBindlessIndex = UINT32_MAX,
	uint64_t rayListAddr = 0,
	uint64_t rayCounterAddr = 0,
	uint64_t radianceAddr = 0,
	uint64_t indirectArgsAddr = 0,
	// The classify's receiver-metadata buffer BDA (packed per-half-res-
	// pixel receiver sample — see ssr_receiver.hlsli). Written by the
	// classify pass below (RW hint), read by the blur (radius bucket).
	uint64_t receiverDataAddr = 0,
	uint64_t hzbAddr = 0,
	uint32_t hzbMips = 0,
	LogicalBufferHandle hzbLogical = LogicalBufferHandle{},
	uint32_t hzbMip1Offset = 0,
	uint32_t hzbMip1Stride = 0,
	uint64_t frameUniformsAddr = 0,
	uint32_t linearSamplerIndex = 0,
	const SsrDesc& overrideDesc = SsrDesc{},
	const char* traceName = "SSR",
	const char* blurName = "SSRBlur",
	const char* composeName = "SSRComposite",
	uint32_t frameIndex = 0) const {
	assert(Valid());
	assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
	assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
	assert(frameUniformsAddr != 0);
	assert(extent.width != 0 && extent.height != 0);
	assert(halfExtent.width != 0 && halfExtent.height != 0);
	// EXTENT INVARIANTS (the dark-triangle hunt — see the temporal
	// variant's pair for the reasoning).
	assert(halfExtent.width == std::max(1u, extent.width / 2u));
	assert(halfExtent.height == std::max(1u, extent.height / 2u));
	assert(hzbMip1Stride >= halfExtent.width);

 	const bool gbufferOn = !gbufferView.empty() && gbufferBindlessIndex != UINT32_MAX;
	const bool gbuffer2On = !gbuffer2View.empty()
		&& gbuffer2BindlessIndex != UINT32_MAX;
 	const bool hzbOn = hzbAddr != 0 && hzbMips != 0 && !hzbLogical.empty()
 		&& hzbMip1Stride != 0;
 	if (!hzbOn) {
 		// HZB unavailable (disabled or not built): skip the entire SSR chain.
		// The app should gate this via m_passes.ssr = false when HZB is unavailable.
		return;
	}
	const float fullW = static_cast<float>(std::max(1u, extent.width));
	const float fullH = static_cast<float>(std::max(1u, extent.height));
	const float halfW = static_cast<float>(std::max(1u, halfExtent.width));
	const float halfH = static_cast<float>(std::max(1u, halfExtent.height));

	// ---- Classify: full half-res dispatch, wave-compacts the wanted
	// rays (see ssr_classify.cs.hlsl). Reads the G-buffer + depth;
	// writes the ray list + the counter + the radiance silence for
	// non-ray pixels + the receiver metadata (EVERY in-range pixel —
	// the blur/compose taps reach non-ray pixels). Every half-res
	// pixel is owned by exactly one of {classify, intersect} per frame
	// — no clear, no stale.
	{
		SsrClassifyPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.rayListAddr = rayListAddr;
		push.rayCounterAddr = rayCounterAddr;
		push.radianceAddr = radianceAddr;
		push.receiverDataAddr = receiverDataAddr;
		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
		push.depthTextureIndex = depthBindlessIndex;
		push.glossThreshold = overrideDesc.glossThreshold;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		RenderPass& pass = rg.AddComputePass("SSRClassify");
		{
			BufferSyncRequest rw{};
			rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
			rw.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(rayList.logical, rw);
			pass.AddBufferHint(rayCounter.logical, rw);
			pass.AddBufferHint(radiance.logical, rw);
			// Receiver metadata: classified WRITE (the graph orders it
			// before the blur/compose fragment reads via their read
			// hints on the same logical buffer).
			pass.AddBufferHint(receiverData.logical, rw);
		}
		{
			ImageSyncRequest depthReq{};
			depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
			pass.AddImageHint(depthView, depthReq);
		}
		if (gbufferOn) {
			ImageSyncRequest gbReq{};
			gbReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			gbReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			gbReq.layout = ImageLayout::ShaderReadOnlyOptimal;
			pass.AddImageHint(gbufferView, gbReq);
		}
		ShaderProgram progCopy = classifyProgram; // copyable (handles only)
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(SsrClassifyPush), &push, ShaderStage::Compute);
			cmd.Dispatch((this->halfExtent.width + 7) / 8, (this->halfExtent.height + 7) / 8, 1);
		});
	}

	// ---- Indirect args: 1 thread builds the intersect's
	// VkDispatchIndirectCommand from the ray count (see
	// ssr_prepare_indirect.cs.hlsl), then resets the counter.
	{
		SsrIndirectPush push{};
		push.rayCounterAddr = rayCounterAddr;
		push.indirectArgsAddr = indirectArgsAddr;
		RenderPass& pass = rg.AddComputePass("SSRIndirectArgs");
		{
			BufferSyncRequest rw{};
			rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
			rw.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(rayCounter.logical, rw);
			pass.AddBufferHint(indirectArgs.logical, rw);
		}
ShaderProgram progCopy = indirectProgram;
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(SsrIndirectPush), &push, ShaderStage::Compute);
			cmd.Dispatch(1, 1, 1);
		});
	}

	// ---- Intersect: indirect compute dispatch (see
	// ssr_intersect.cs.hlsl) — GGX VNDF + the classic Hi-Z traversal,
	// only for the compacted rays. The empty ray count dispatches a
	// zero-group indirect (a no-op record).
	{
		SsrIntersectPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.rayListAddr = rayListAddr;
		push.rayCounterAddr = rayCounterAddr;
		push.radianceAddr = radianceAddr;
		push.ssrHzbAddr = hzbOn ? hzbAddr : 0;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
 		push.samplerIndex = linearSamplerIndex;
 		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
		push.gbuffer2TextureIndex = gbuffer2On ? gbuffer2BindlessIndex : UINT32_MAX;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		push.glossThreshold = 0.94f;
		push.invDepthW = 1.0f / fullW;
		push.invDepthH = 1.0f / fullH;
		push.maxDistance = (std::max)(0.1f, overrideDesc.maxDistance);
		push.thickness = (std::max)(0.01f, overrideDesc.thickness);
		push.intensity = (std::max)(0.0f, overrideDesc.intensity);
		push.f0 = std::clamp(overrideDesc.f0, 0.0f, 1.0f);
		push.frameIndex = frameIndex;
		push.mostDetailedMip = 1;
		push.maxIntersections = std::clamp(overrideDesc.maxIntersections, 4u, 64u);
		push.minOccupancy = 4;
		push.ssrHzbMipCount = hzbOn ? hzbMips : 0u;
		RenderPass& pass = rg.AddComputePass(traceName ? traceName : "SSRIntersect");
		{
			BufferSyncRequest r{};
			r.barrierMasks.accessMask = AccessFlags::ShaderRead;
			r.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(rayList.logical, r);
			pass.AddBufferHint(rayCounter.logical, r);
			BufferSyncRequest w{};
			w.barrierMasks.accessMask = AccessFlags::ShaderWrite;
			w.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(radiance.logical, w);
			BufferSyncRequest indirect{};
			indirect.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
			indirect.barrierMasks.stageMask = PipelineStageBit_DrawIndirect;
			pass.AddBufferHint(indirectArgs.logical, indirect);
			if (hzbOn) {
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Compute;
				pass.AddBufferHint(hzbLogical, hzbRead);
			}
		}
		std::vector<LogicalImageViewHandle> intersectInputs = { hdrView, depthView };
		if (gbufferOn)
			intersectInputs.push_back(gbufferView);
		if (gbuffer2On)
			intersectInputs.push_back(gbuffer2View);
		pass.SetInputImages(intersectInputs);
ShaderProgram progCopy = intersectProgram;
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(SsrIntersectPush), &push, ShaderStage::Compute);
			cmd.DispatchIndirect(this->indirectArgs.buffer, 0);
		});
	}

	// ---- Copy: the radiance buffer -> the raw reflection image (the
	// bilateral blur reads it bilinearly, so it stays an image).
	{
		SsrCopyPush push{};
		push.radianceAddr = radianceAddr;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		RenderPass& pass = rg.AddPass("SSRCopy");
		pass.SetOffset(0, 0)
			.SetExtent(halfExtent);
		{
			BufferSyncRequest radRead{};
			radRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			radRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(radiance.logical, radRead);
		}
		pass.SetColor({ raw.logical },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
ShaderProgram progCopy = copyProgram;
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexLayoutId::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::Opaque(),
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(SsrCopyPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
	}

	// ---- Blur: raw reflection -> blurred (depth edges via HZB mip1).
	// Mode-dependent (AddSsrBlurPasses): the original single-pass 2D
	// bilateral, or the separable H+V pair through blurTemp.
	AddSsrBlurPasses(rg, gbufferView, gbufferBindlessIndex,
		hzbAddr, hzbLogical, receiverDataAddr, receiverData.logical,
		hzbMip1Offset, hzbMip1Stride,
		frameUniformsAddr, linearSamplerIndex, overrideDesc, blurName);

	// ---- Composite: blurred half-res + HZB depth authority + HDR ----
	// -> HDR. The depth reads (center from mip0, tap reps from mip1) come
	// from the HZB buffer — the depth image left this pass's input set.
	{
		SsrComposePush push{};
		push.frameUniforms = frameUniformsAddr;
		push.hzb = hzbAddr;
		push.receiverData = receiverDataAddr;
		push.blurTextureIndex = blurred.bindlessIndex;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		// The env-specular export (the replacement-composite contract, see
		// the push comment + ssao_compose.frag.hlsl): the exact IBL slice
		// a confident hit REPLACES. Absent (empty view / UINT32_MAX) = the
		// additive fallback.
		push.envSpecTextureIndex =
			(envSpecView.empty() || envSpecBindlessIndex == UINT32_MAX)
			? UINT32_MAX : envSpecBindlessIndex;
		push.invDepthW = 1.0f / fullW;
		push.invDepthH = 1.0f / fullH;
		push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
		push.fullW = extent.width;
		push.fullH = extent.height;
		push.hzbMip1Offset = hzbMip1Offset;
		push.hzbMip1Stride = hzbMip1Stride;
		push.vignetteWidth = overrideDesc.vignetteWidth;
		// The material-eligibility gate (the classify's own threshold):
		// a matte dielectric output pixel must not receive its
		// reflective neighbor's layer across a shared plane.
		push.glossThreshold = overrideDesc.glossThreshold;
		// The upsample's normal agreement: the full-res center normal
		// (the thin G-buffer) + the tap normals (the receiver metadata).
		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
		// The diagnostic mode (SsrComposeDebug — the dark-triangle hunt).
		push.debugMode = static_cast<uint32_t>(overrideDesc.composeDebug);

		std::vector<LogicalImageViewHandle> composeInputs = { blurred.logical, hdrView };
		if (gbufferOn)
			composeInputs.push_back(gbufferView); // the center normal
		if (!envSpecView.empty() && envSpecBindlessIndex != UINT32_MAX)
			composeInputs.push_back(envSpecView);
		// Contribution debug (desc.debugContrib): bind the .dbg twin (the
		// composite writes its exact ADD term to SV_Target1) + the ssrContrib
		// target as the second color attachment. Falls back to the normal
		// single-attachment program when the twin or the target is missing.
		const bool contribDbgOn = overrideDesc.debugContrib &&
			composeDbgProgram.shaderCount != 0 && contrib.Valid();
		// DIAGNOSTIC H (ClearFillCoverage): a REAL Clear load op
		// (magenta, scene-domain) on the composite attachment — the
		// shader fills cyan; any magenta left visible is a region the
		// fullscreen draw never covered (viewport/scissor/extent).
		const bool clearFillCoverage =
			overrideDesc.composeDebug == SsrComposeDebug::ClearFillCoverage;
		RenderPass& pass = rg.AddPass(composeName ? composeName : "SSRComposite");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages(composeInputs)
				.SetColor(contribDbgOn
					? std::vector<LogicalImageViewHandle>{ logicalView, contrib.logical }
					: std::vector<LogicalImageViewHandle>{ logicalView },
					clearFillCoverage ? AttachmentLoadOp::Clear : AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store,
					clearFillCoverage
						? ClearColor{ 300.0f, 0.0f, 300.0f, 1.0f }
						: ClearColor{});
		{
			// HZB buffer read (mip0 center + mip1 reps): barrier the
			// SsrHzb compute writes before these fragment reads.
			BufferSyncRequest hzbRead{};
			hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(hzbLogical, hzbRead);
			// Receiver metadata read (the tap normals): barrier the
			// classify's compute writes.
			BufferSyncRequest recvRead{};
			recvRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			recvRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(receiverData.logical, recvRead);
		}
		ShaderProgram progCopy = contribDbgOn ? composeDbgProgram : composeProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrComposePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}

// Temporal (TAA) variant: trace + blur (identical pushes to the
// spatial path above, stencil included), then the
// temporal pass (blurred + history[parity^1] + motion + depth ->
// history[parity]), then the composite sampling the just-written
// history slot instead of the blurred target. The base HDR path is
// untouched — only the reflection layer is temporally filtered.
// historyValid == false warms up: the temporal pass copies curr into
// the slot so the next record has something to blend against.
// Captures only copyable handles/indices, so the passes are safe once
// this call returns.
void AddSsrTemporalPasses(RenderGraph& rg,
	LogicalImageViewHandle hdrView,
	uint32_t hdrBindlessIndex,
	LogicalImageViewHandle depthView,
	uint32_t depthBindlessIndex,
	LogicalImageViewHandle gbufferView,
	uint32_t gbufferBindlessIndex,
	// The F0-chroma layer 2 (see AddSsrPasses' pair): the tinted-
	// Fresnel source. Empty/MAX = the flat-F0 fallback.
	LogicalImageViewHandle gbuffer2View,
	uint32_t gbuffer2BindlessIndex,
	// The env-specular export (the replacement-composite contract, see
	// SsrComposePush::envSpecTextureIndex + AddSsrPasses' pair): the
	// AO-compose-scaled env-spec slice the confident hit replaces.
	// Empty/MAX = the additive fallback.
	LogicalImageViewHandle envSpecView,
	uint32_t envSpecBindlessIndex,
	uint64_t rayListAddr,
	uint64_t rayCounterAddr,
	uint64_t radianceAddr,
	uint64_t indirectArgsAddr,
	// The classify's receiver-metadata buffer BDA (see AddSsrPasses'
	// pair): written by the classify below, read by the blur.
	uint64_t receiverDataAddr,
	uint64_t hzbAddr,
	uint32_t hzbMips,
	LogicalBufferHandle hzbLogical,
	uint32_t hzbMip1Offset,
	uint32_t hzbMip1Stride,
	LogicalImageViewHandle motionView,
	uint32_t motionBindlessIndex,
	uint64_t frameUniformsAddr,
	uint32_t linearSamplerIndex,
	const SsrDesc& overrideDesc,
	float jitterDeltaX,
	float jitterDeltaY,
	float taaBlend,
	bool historyValid,
	uint32_t parity,
	const char* traceName = "SSR",
	const char* blurName = "SSRBlur",
	const char* temporalName = "SSRTemporal",
	const char* composeName = "SSRComposite",
	uint32_t frameIndex = 0) const {
	assert(Valid());
	assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
	assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
	assert(!motionView.empty() && motionBindlessIndex != UINT32_MAX);
	assert(frameUniformsAddr != 0);
	assert(extent.width != 0 && extent.height != 0);
	assert(halfExtent.width != 0 && halfExtent.height != 0);
	assert(parity < 2);
	// EXTENT INVARIANTS (the dark-triangle hunt): the half extent is
	// the exact integer halving (the classify's receiver-buffer stride
	// and the blur/compose tap domain), and the HZB mip1 row stride
	// must cover the half-extent column domain (mip1 = ceil(full/2) >=
	// half by construction — a violation means the pyramid and the SSR
	// targets disagree about the render extent).
	assert(halfExtent.width == std::max(1u, extent.width / 2u));
	assert(halfExtent.height == std::max(1u, extent.height / 2u));
	assert(hzbMip1Stride >= halfExtent.width);

 	const bool gbufferOn = !gbufferView.empty() && gbufferBindlessIndex != UINT32_MAX;
	const bool gbuffer2On = !gbuffer2View.empty()
		&& gbuffer2BindlessIndex != UINT32_MAX;
 const bool hzbOn = hzbAddr != 0 && hzbMips != 0 && !hzbLogical.empty()
 		&& hzbMip1Stride != 0;
 	if (!hzbOn) {
 		// HZB unavailable: skip temporal SSR path as well.
		return;
	}
	const float fullW = static_cast<float>(std::max(1u, extent.width));
	const float fullH = static_cast<float>(std::max(1u, extent.height));
	const float halfW = static_cast<float>(std::max(1u, halfExtent.width));
	const float halfH = static_cast<float>(std::max(1u, halfExtent.height));

	// ---- THE COMPUTE CHAIN (classify -> indirect -> intersect -> copy)
	// ---- SELF-CONTAINED (the temporal-path repair): the temporal
	// variant used to ASSUME "the raw target is populated by the compute
	// chain" — but the caller's either/or record never ran it alongside
	// (AddSsrPasses owns the chain; taking the temporal branch left raw
	// empty), so the temporal composite blended blur(zeros) into the
	// history and the reflections read ZERO (the re-enable hunt: the
	// on/off captures bit-identical, the ssrdump's raw/blur/hist all
	// 0.0). That empty-raw wiring was almost certainly the original
	// "smear" the path got parked for, too — reprojected nothing blended
	// with nothing. The chain below is the AddSsrPasses one verbatim
	// (classify/indirect/intersect/copy; the classify also writes the
	// receiver metadata, every in-range pixel — same as the spatial
	// path); the ONE behavioral difference: the intersect's frameIndex
	// is the caller's dither EPOCH (the record counter), not the
	// spatial path's static pattern — the animated ray-origin dither
	// decorrelates consecutive traces so the temporal pass converges
	// the per-frame speckle to the lobe mean (a static dither would
	// park the pattern instead).
	{
		SsrClassifyPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.rayListAddr = rayListAddr;
		push.rayCounterAddr = rayCounterAddr;
		push.radianceAddr = radianceAddr;
		push.receiverDataAddr = receiverDataAddr;
		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
		push.depthTextureIndex = depthBindlessIndex;
		push.glossThreshold = overrideDesc.glossThreshold;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		RenderPass& pass = rg.AddComputePass("SSRClassify");
		{
			BufferSyncRequest rw{};
			rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
			rw.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(rayList.logical, rw);
			pass.AddBufferHint(rayCounter.logical, rw);
			pass.AddBufferHint(radiance.logical, rw);
			pass.AddBufferHint(receiverData.logical, rw);
		}
		{
			ImageSyncRequest depthReq{};
			depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
			pass.AddImageHint(depthView, depthReq);
		}
		if (gbufferOn) {
			ImageSyncRequest gbReq{};
			gbReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			gbReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			gbReq.layout = ImageLayout::ShaderReadOnlyOptimal;
			pass.AddImageHint(gbufferView, gbReq);
		}
		ShaderProgram progCopy = classifyProgram; // copyable (handles only)
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(SsrClassifyPush), &push, ShaderStage::Compute);
			cmd.Dispatch((this->halfExtent.width + 7) / 8, (this->halfExtent.height + 7) / 8, 1);
		});
	}

	// ---- Indirect args: 1 thread builds the intersect's
	// VkDispatchIndirectCommand from the ray count (see
	// ssr_prepare_indirect.cs.hlsl), then resets the counter.
	{
		SsrIndirectPush push{};
		push.rayCounterAddr = rayCounterAddr;
		push.indirectArgsAddr = indirectArgsAddr;
		RenderPass& pass = rg.AddComputePass("SSRIndirectArgs");
		{
			BufferSyncRequest rw{};
			rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
			rw.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(rayCounter.logical, rw);
			pass.AddBufferHint(indirectArgs.logical, rw);
		}
		ShaderProgram progCopy = indirectProgram;
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(SsrIndirectPush), &push, ShaderStage::Compute);
			cmd.Dispatch(1, 1, 1);
		});
	}

	// ---- Intersect: indirect compute dispatch (see
	// ssr_intersect.cs.hlsl) — GGX VNDF + the classic Hi-Z traversal,
	// only for the compacted rays. The empty ray count dispatches a
	// zero-group indirect (a no-op record).
	{
		SsrIntersectPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.rayListAddr = rayListAddr;
		push.rayCounterAddr = rayCounterAddr;
		push.radianceAddr = radianceAddr;
		push.ssrHzbAddr = hzbOn ? hzbAddr : 0;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
 		push.samplerIndex = linearSamplerIndex;
 		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
		push.gbuffer2TextureIndex = gbuffer2On ? gbuffer2BindlessIndex : UINT32_MAX;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		push.glossThreshold = 0.94f;
		push.invDepthW = 1.0f / fullW;
		push.invDepthH = 1.0f / fullH;
		push.maxDistance = (std::max)(0.1f, overrideDesc.maxDistance);
		push.thickness = (std::max)(0.01f, overrideDesc.thickness);
		push.intensity = (std::max)(0.0f, overrideDesc.intensity);
		push.f0 = std::clamp(overrideDesc.f0, 0.0f, 1.0f);
		push.frameIndex = frameIndex; // the dither EPOCH (see the note above)
		push.mostDetailedMip = 1;
		push.maxIntersections = std::clamp(overrideDesc.maxIntersections, 4u, 64u);
		push.minOccupancy = 4;
		push.ssrHzbMipCount = hzbOn ? hzbMips : 0u;
		RenderPass& pass = rg.AddComputePass(traceName ? traceName : "SSRIntersect");
		{
			BufferSyncRequest r{};
			r.barrierMasks.accessMask = AccessFlags::ShaderRead;
			r.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(rayList.logical, r);
			pass.AddBufferHint(rayCounter.logical, r);
			BufferSyncRequest w{};
			w.barrierMasks.accessMask = AccessFlags::ShaderWrite;
			w.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(radiance.logical, w);
			BufferSyncRequest indirect{};
			indirect.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
			indirect.barrierMasks.stageMask = PipelineStageBit_DrawIndirect;
			pass.AddBufferHint(indirectArgs.logical, indirect);
			if (hzbOn) {
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Compute;
				pass.AddBufferHint(hzbLogical, hzbRead);
			}
		}
		std::vector<LogicalImageViewHandle> intersectInputs = { hdrView, depthView };
		if (gbufferOn)
			intersectInputs.push_back(gbufferView);
		if (gbuffer2On)
			intersectInputs.push_back(gbuffer2View);
		pass.SetInputImages(intersectInputs);
		ShaderProgram progCopy = intersectProgram;
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(SsrIntersectPush), &push, ShaderStage::Compute);
			cmd.DispatchIndirect(this->indirectArgs.buffer, 0);
		});
	}

	// ---- Copy: the radiance buffer -> the raw reflection image (the
	// bilateral blur reads it bilinearly, so it stays an image).
	{
		SsrCopyPush push{};
		push.radianceAddr = radianceAddr;
		push.halfW = halfExtent.width;
		push.halfH = halfExtent.height;
		RenderPass& pass = rg.AddPass("SSRCopy");
		pass.SetOffset(0, 0)
			.SetExtent(halfExtent);
		{
			BufferSyncRequest radRead{};
			radRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			radRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(radiance.logical, radRead);
		}
		pass.SetColor({ raw.logical },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
		ShaderProgram progCopy = copyProgram;
		pass.SetRecordingFunc([this, progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexLayoutId::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::Opaque(),
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(SsrCopyPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
	}

	// ---- Blur: raw reflection -> blurred (depth edges via HZB mip1).
	// Mode-dependent (AddSsrBlurPasses): the original single-pass 2D
	// bilateral, or the separable H+V pair through blurTemp — the same
	// emission as the spatial path (the shared helper is the
	// temporal-path repair's second half: the two copies can no longer
	// drift).
		AddSsrBlurPasses(rg, gbufferView, gbufferBindlessIndex,
			hzbAddr, hzbLogical, receiverDataAddr, receiverData.logical,
			hzbMip1Offset, hzbMip1Stride,
			frameUniformsAddr, linearSamplerIndex, overrideDesc, blurName);

		// ---- Temporal: blurred + history[parity^1] + motion + depth ----
		// -> history[parity]. The base HDR is not an input here: only
		// the reflection layer accumulates.
		const Target& histPrev = history[(parity ^ 1u) & 1u];
		const Target& histCurr = history[parity & 1u];
		{
			SsrTemporalPush push{};
push.frameUniforms = frameUniformsAddr;
		push.currTextureIndex = blurred.bindlessIndex;
		push.historyTextureIndex = histPrev.bindlessIndex;
		push.motionTextureIndex = motionBindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
		// The disocclusion test's history side: the PREVIOUS record's
		// depth mirror (the current depth at a previous-frame coordinate
		// was a different camera's geometry — see the shader's
		// disocclusion note).
		push.prevDepthTextureIndex = prevDepth.bindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.historyValid = historyValid ? 1u : 0u;
		push.jitterDeltaX = jitterDeltaX;
		push.jitterDeltaY = jitterDeltaY;
		push.texelX = 1.0f / halfW;
		push.texelY = 1.0f / halfH;
		push.invDepthW = 1.0f / fullW;
		push.invDepthH = 1.0f / fullH;
		push.blend = std::clamp(overrideDesc.temporalBlend, 0.0f, 0.98f);
		push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);

			RenderPass& pass = rg.AddPass(temporalName ? temporalName : "SSRTemporal");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ blurred.logical, histPrev.logical, motionView,
					depthView, prevDepth.logical })
				.SetColor({ histCurr.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = temporalProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrTemporalPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

	// ---- Composite: temporal half-res + HZB depth authority + HDR ----
	// -> HDR. Same depth-guided upsample as the spatial path (center from
	// HZB mip0, tap reps from mip1 — no depth image input), but the
	// reflection source is the just-written history slot.
	{
		SsrComposePush push{};
		push.frameUniforms = frameUniformsAddr;
		push.hzb = hzbAddr;
		push.receiverData = receiverDataAddr;
		push.blurTextureIndex = histCurr.bindlessIndex;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		// The env-specular export (the replacement-composite contract —
		// see the spatial variant's compose + the push comment): the exact
		// IBL slice a confident hit REPLACES. Absent = the additive
		// fallback.
		push.envSpecTextureIndex =
			(envSpecView.empty() || envSpecBindlessIndex == UINT32_MAX)
			? UINT32_MAX : envSpecBindlessIndex;
		push.invDepthW = 1.0f / fullW;
		push.invDepthH = 1.0f / fullH;
		push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
		push.fullW = extent.width;
		push.fullH = extent.height;
		push.hzbMip1Offset = hzbMip1Offset;
		push.hzbMip1Stride = hzbMip1Stride;
		// The temporal composite never carried the vignette width (a
		// pre-existing omission — it rode the push default 0.15 instead
		// of the desc's; the spatial variant set it). Paired with the
		// eligibility gate below for one consistent temporal compose.
		push.vignetteWidth = overrideDesc.vignetteWidth;
		// The material-eligibility gate (the spatial variant's pair):
		// a matte dielectric output pixel must not receive its
		// reflective neighbor's layer across a shared plane.
		push.glossThreshold = overrideDesc.glossThreshold;
		// The upsample's normal agreement (the spatial variant's pair):
		// the full-res center normal + the tap normals.
		push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
		// The diagnostic mode (SsrComposeDebug — the spatial variant's
		// pair; the SAME shader serves both paths).
		push.debugMode = static_cast<uint32_t>(overrideDesc.composeDebug);

		std::vector<LogicalImageViewHandle> composeInputs = { histCurr.logical, hdrView };
		if (gbufferOn)
			composeInputs.push_back(gbufferView); // the center normal
		if (!envSpecView.empty() && envSpecBindlessIndex != UINT32_MAX)
			composeInputs.push_back(envSpecView);
		// Contribution debug (desc.debugContrib): bind the .dbg twin (the
		// composite writes its exact ADD term to SV_Target1) + the ssrContrib
		// target as the second color attachment. Falls back to the normal
		// single-attachment program when the twin or the target is missing.
		const bool contribDbgOn = overrideDesc.debugContrib &&
			composeDbgProgram.shaderCount != 0 && contrib.Valid();
		// DIAGNOSTIC H (ClearFillCoverage): a REAL Clear load op
		// (magenta, scene-domain) on the composite attachment — the
		// shader fills cyan; any magenta left visible is a region the
		// fullscreen draw never covered (viewport/scissor/extent).
		const bool clearFillCoverage =
			overrideDesc.composeDebug == SsrComposeDebug::ClearFillCoverage;
		RenderPass& pass = rg.AddPass(composeName ? composeName : "SSRComposite");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages(composeInputs)
				.SetColor(contribDbgOn
					? std::vector<LogicalImageViewHandle>{ logicalView, contrib.logical }
					: std::vector<LogicalImageViewHandle>{ logicalView },
					clearFillCoverage ? AttachmentLoadOp::Clear : AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store,
					clearFillCoverage
						? ClearColor{ 300.0f, 0.0f, 300.0f, 1.0f }
						: ClearColor{});
		{
			// HZB buffer read (mip0 center + mip1 reps).
			BufferSyncRequest hzbRead{};
			hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(hzbLogical, hzbRead);
			// Receiver metadata read (the tap normals): barrier the
			// classify's compute writes.
			BufferSyncRequest recvRead{};
			recvRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
			recvRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
			pass.AddBufferHint(receiverData.logical, recvRead);
		}
		ShaderProgram progCopy = contribDbgOn ? composeDbgProgram : composeProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrComposePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Depth history copy: the final renderDepth -> the prevDepth
		// R32F mirror for the NEXT temporal record's disocclusion test
		// (the temporal pass above read the PREVIOUS copy — emission
		// order gives the read->write barrier within the frame; the next
		// record's read is a cross-frame cycle exactly like the history
		// ping-pong). Emitted ONLY on the temporal path on purpose:
		// historyValid implies the previous record was temporal
		// (SsrTaaEndFrame invalidates otherwise), so the mirror is
		// always exactly one record old when the test reads it; the
		// warmup record never reads it. The depth is final here (the
		// HZB build — the SSR chain's gate — already consumed the
		// complete renderDepth).
		{
			SsrDepthHistoryPush push{};
			push.depthTextureIndex = depthBindlessIndex;
			RenderPass& pass = rg.AddPass("SSRDepthHistory");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages({ depthView })
				.SetColor({ prevDepth.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = depthHistoryProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrDepthHistoryPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}

	// Temporal variant using the stored desc from Ensure().
	void AddSsrTemporalPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		LogicalImageViewHandle gbufferView,
		uint32_t gbufferBindlessIndex,
		// The F0-chroma layer 2 (see AddSsrPasses' pair): the tinted-
		// Fresnel source. Empty/MAX = the flat-F0 fallback.
		LogicalImageViewHandle gbuffer2View,
		uint32_t gbuffer2BindlessIndex,
		LogicalImageViewHandle envSpecView,
		uint32_t envSpecBindlessIndex,
		uint64_t rayListAddr,
		uint64_t rayCounterAddr,
		uint64_t radianceAddr,
		uint64_t indirectArgsAddr,
		// The classify's receiver-metadata buffer BDA (see the full
		// variant's pair).
		uint64_t receiverDataAddr,
		uint64_t hzbAddr,
		uint32_t hzbMips,
		LogicalBufferHandle hzbLogical,
		uint32_t hzbMip1Offset,
		uint32_t hzbMip1Stride,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		float jitterDeltaX,
		float jitterDeltaY,
		float taaBlend,
		bool historyValid,
		uint32_t parity,
		const char* traceName = "SSR",
		const char* blurName = "SSRBlur",
		const char* temporalName = "SSRTemporal",
		const char* composeName = "SSRComposite",
		uint32_t frameIndex = 0) const {
		AddSsrTemporalPasses(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			gbufferView, gbufferBindlessIndex,
			gbuffer2View, gbuffer2BindlessIndex,
			envSpecView, envSpecBindlessIndex,
			rayListAddr, rayCounterAddr, radianceAddr, indirectArgsAddr,
			receiverDataAddr,
			hzbAddr, hzbMips, hzbLogical, hzbMip1Offset, hzbMip1Stride,
			motionView, motionBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, desc,
			jitterDeltaX, jitterDeltaY, taaBlend, historyValid, parity,
			traceName, blurName, temporalName, composeName, frameIndex);
	}
};

} // namespace smvk
