// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldRenderer: whole-frame renderer over a WORLD. The host (App's
// main.cpp, the editor's live view) owns all content loading — scenes
// arrive as a resolved smvk::World whose clusters/resources are already
// uploaded to the context and render by handles. The renderer knows
// nothing about paks, scene files or scene discovery; it owns every
// framegraph resource (pipelines, targets, per-frame buffers) and
// renders whatever world it was handed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/app.h>
#include <Engine/App/keybindings.h>
#include <Engine/App/RunConfig.h>
#include <Engine/App/world.h>
#include <Engine/Profile/profile.h>
#include <Engine/Camera/camera.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/bc6h.h>
#include <Engine/GI/cubemap_volume.h>
#include <Engine/GI/lightmap.h>
#include <Engine/GI/regular_volume.h>
#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Renderer/texture_streamer.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/deferredshadow.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/dlss.h>
#include <Engine/Rendergraph/dof.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Rendergraph/evsm.h>
#include <Engine/Rendergraph/exposure.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/motionblur.h>
#include <Engine/Rendergraph/motionvector.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/particles.h>
#include <Engine/Rendergraph/smaa.h>
#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Rendergraph/ssgi.h>
#include <Engine/Rendergraph/ssgi_diffuse.h>
#include <Engine/Rendergraph/ssr.h>
#include <Engine/Rendergraph/envzones.h>
#include <Engine/Rendergraph/probeball.h>
#include <Engine/Rendergraph/probepremix.h>
#include <Engine/Rendergraph/skinning.h>
#include <Engine/Rendergraph/terrain.h>
#include <Engine/Rendergraph/volumetric.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/ui.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/Vfs/vfs.h>
#include <Engine/WorldFields/WorldField.h>
#include <Engine/WorldFields/WorldFieldProvider.h>
#include <Engine/WorldFields/WorldFieldCodec.h>
#include <Engine/WorldPartition/WorldPartition.h>
#include <Engine/WorldPartition/WorldEnvTiles.h>

#include <Engine/Rendergraph/ForwardPlusRenderer.h>
#include <Engine/Rendergraph/FrameContext.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/GfxRenderer.h>
#include <Engine/App/Element.h>

namespace engine {

// ---------------------------------------------------------------------------
// Configurable pass set. RenderFrame consults these flags, so scene samples
// (or A/B experiments) toggle passes without touching pass code. Defaults
// are the full pipeline (previous behavior).
// ---------------------------------------------------------------------------
struct WorldRendererConfig {
	bool directionalShadows = true; // CSM cascade passes
	bool spotShadows = true;        // spot atlas selection + tile passes
	bool forwardPlusPrepass = true; // depth prepass feeding the tile cull
	bool forwardPlusCull = true;    // tile light cull
	bool gfxFilter = true;          // GPU-driven per-view triangle filter
	bool gfxMesh = true;            // task-less mesh-shader main leg (needs device mesh shaders)
	bool filteredMotion = true;     // filtered motion-vector path
	bool motionVectors = true;      // motion-vector target pass
	bool motionBlur = true;         // HDR velocity-gather blur
	bool dof = true;                // depth-of-field (low single-pass / HQ bokeh)
	bool ssr = true;                // screen-space reflections (depth-reconstructed)
	bool volumetric = true;         // raymarched GI-probe volumetric fog (HDR composite)
	bool ssao = true;               // AO master switch (SSAO/GTAO/SAO per AoMode)
	bool ssgiDiffuse = true;        // diffuse screen-space GI master switch (the runtime
	                                 // enable is m_ssgiDiffuseEnabled — MSAA scenes gate
	                                 // off at the frame site, no stable 1x G-buffer)
	bool bloom = true;              // HDR bloom chain
	bool exposure = true;           // histogram meter + CPU eye adaptation
	bool tonemap = true;            // HDR -> swapchain tonemap
	bool ui = true;                 // separated ImGui target + composite
	bool msaa = true;               // 2x MSAA Main (when device supports it)
	bool skydome = true;            // fullscreen-triangle sky in Main
	bool terrain = false;             // tiled heightfield dressing in Main (off by default)
	bool smaa = true;               // SMAA post-process chain (mode via UI AA combo)
	bool deferredShadows = true;    // tiled/zBin deferred-shadow pipeline (H toggles at runtime)
	bool particles = true;          // GPU particle system (compute emitters + tiled-lit billboards)

	static WorldRendererConfig All() { return WorldRendererConfig{}; }
	static WorldRendererConfig Minimal() {		WorldRendererConfig c{};
		c.directionalShadows = false;
		c.spotShadows = false;
		c.forwardPlusPrepass = false;
		c.forwardPlusCull = false;
		c.gfxFilter = false;
		c.filteredMotion = false;
		c.gfxMesh = false;
		c.motionVectors = false;
		c.motionBlur = false;
		c.dof = false;
		c.ssr = false;
		c.volumetric = false;
		c.ssao = false;
		c.ssgiDiffuse = false;
		c.bloom = false;
		c.exposure = false;
		c.ui = false;
		c.msaa = false;
		c.skydome = false;
		c.terrain = false;
		c.smaa = false;
		c.deferredShadows = false;
		c.particles = false;
		return c;
	}

	// Bisection config: filtered Main + HZB + tonemap only (depth prepass
	// feeds the occluder/HZB chain, the GPU filter picks draws, Main shades
	// survivors). Everything else off.
	static WorldRendererConfig FilterOnly() {
		WorldRendererConfig c = Minimal();
		c.forwardPlusPrepass = true;
		c.gfxFilter = true;
		return c;
	}
};

// ---------------------------------------------------------------------------
// (Debug-UI dock position: removed - the panel is a fixed full-height side
// bar on the Right. The top-left corner stays free for the stats HUD.)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Antialiasing mode (ImGui combo). MSAA 2x and SMAA are mutually exclusive:
// running SMAA on top of an MSAA-resolved image wastes the resolve without
// adding subpixel coverage (SMAA S2x/4x are separate, unported modes).
// ---------------------------------------------------------------------------
enum class AaMode : int {
	Off = 0,
	Msaa2x = 1,
	Smaa1x = 2,
	SmaaT2x = 3,
};

// ---------------------------------------------------------------------------
// Texture filtering mode (renderer-configurable, UI toggle).
// Trilinear = linear min/mag, linear-between-mips (lowest option, default).
// AnisoNx  = trilinear + anisotropic filtering with maxAnisotropy N
// (VK_SAMPLER_ANISOTROPY, clamped to the device maxSamplerAnisotropy).
// Applies to the bindless texture samplers (slot 0 Repeat + slot 2 Clamp,
// which also covers cubemaps via dedup) through in-place sampler recreate.
// ---------------------------------------------------------------------------
enum class TextureFiltering : int {
	Trilinear = 0,
	Anisotropic2x = 1,
	Anisotropic4x = 2,
	Anisotropic8x = 3,
	Anisotropic16x = 4,
};
inline const char* TextureFilteringName(TextureFiltering m) {
	switch (m) {
	case TextureFiltering::Trilinear: return "Trilinear";
	case TextureFiltering::Anisotropic2x: return "Anisotropic 2x";
	case TextureFiltering::Anisotropic4x: return "Anisotropic 4x";
	case TextureFiltering::Anisotropic8x: return "Anisotropic 8x";
	case TextureFiltering::Anisotropic16x: return "Anisotropic 16x";
	}
	return "?";
}
inline float TextureFilteringAnisotropy(TextureFiltering m) {
	switch (m) {
	case TextureFiltering::Trilinear: return 1.0f;
	case TextureFiltering::Anisotropic2x: return 2.0f;
	case TextureFiltering::Anisotropic4x: return 4.0f;
	case TextureFiltering::Anisotropic8x: return 8.0f;
	case TextureFiltering::Anisotropic16x: return 16.0f;
	}
	return 1.0f;
}
// Parses TOURMALINE_TEXTURE_FILTER / UI strings: trilinear | 2x | 4x | 8x |
// 16x (also accepts "anisoNx"/"anisotropicNx"/bare "N"). Legacy "bilinear"
// maps to Trilinear. Returns true on success.
inline bool TextureFilteringFromString(std::string_view s, TextureFiltering& out) {
	// Lower-case + strip "anisotropic"/"aniso" prefix so "Anisotropic 8x",
	// "aniso8x", "8x", "8" all converge below.
	char buf[32]{};
	size_t n = 0;
	for (char c : s) {
		if (n + 1 >= sizeof(buf))
			break;
		buf[n++] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
	}
	buf[n] = '\0';
	std::string_view v{ buf, n };
	// Trim ASCII whitespace.
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
		v.remove_prefix(1);
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
		v.remove_suffix(1);
	if (v == "trilinear" || v == "tri") {
		out = TextureFiltering::Trilinear;
		return true;
	}
	if (v == "bilinear" || v == "bi") {
		out = TextureFiltering::Trilinear; // lowest option is trilinear now
		return true;
	}
	if (v.starts_with("anisotropic"))
		v.remove_prefix(11);
	else if (v.starts_with("aniso"))
		v.remove_prefix(5);
	while (!v.empty() && v.front() == ' ')
		v.remove_prefix(1);
	if (!v.empty() && v.back() == 'x')
		v.remove_suffix(1);
	if (v == "2") {
		out = TextureFiltering::Anisotropic2x;
		return true;
	}
	if (v == "4") {
		out = TextureFiltering::Anisotropic4x;
		return true;
	}
	if (v == "8") {
		out = TextureFiltering::Anisotropic8x;
		return true;
	}
	if (v == "16") {
		out = TextureFiltering::Anisotropic16x;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Ambient / GI source selection (N cycles): exactly one active source set
// for clean isolation — All is the production state, the rest isolate a
// single GI contribution for bisection. The ambient is the only thing
// this drives; the visible sky dome (K) and the direct sun / cascaded
// shadows are unaffected (Off = direct light only).
//   All:        every baked source (per-pixel priority: lightmap >
//               regular grid > probe volume); the sky IBL is the
//               FALLBACK for uncovered pixels only, never additive
//   Lightmaps:  baked 2D SG lightmaps ONLY (static draws)
//   Probes:     baked 3D probes ONLY (regular grids + octree volume —
//               the palette view splits grid vs volume per pixel)
//   SkyIbl:     analytic sky IBL only (all baked sources disabled)
//   Off:        no GI/ambient contribution at all (pure direct sun frame)
//   Baked:      All's baked sources with the sky IBL fallback OFF —
//               uncovered pixels and the GI-volume composite base take
//               direct only (baked-field isolation: what the bakes
//               contribute alone, no analytic sky anywhere in the
//               ambient)
// ---------------------------------------------------------------------------
enum class AmbientMode : int {
	All = 0,        // baked GI; sky IBL fallback for uncovered pixels
	Lightmaps = 1,  // baked lightmaps only
	Probes = 2,     // baked probe systems only (grids + volume)
	SkyIbl = 3,     // analytic sky IBL only
	Off = 4,        // direct light only
	Baked = 5,      // All minus the sky fallback (baked GI isolation)
};
inline const char* AmbientModeName(AmbientMode m) {
	switch (m) {
	case AmbientMode::All: return "All (baked GI; sky fallback)";
	case AmbientMode::Lightmaps: return "Lightmaps only";
	case AmbientMode::Probes: return "Probes only (grids + volume)";
	case AmbientMode::SkyIbl: return "Sky IBL only";
	case AmbientMode::Off: return "Off (direct only)";
	case AmbientMode::Baked: return "Baked (no sky fallback)";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// RENDER QUALITY CONFIG (host-owned, never scene content): HOW the
// renderer runs — the GI/ambient source, the per-source opt-ins, the
// probe pre-mix, EVSM shadows. This is machine/session state exactly
// like render scale or DLSS mode: the editor drives it live from the
// Render tab (PushRenderQuality, re-applied after every WYSIWYG attach
// so the session's choices beat the loaders' on-when-baked defaults),
// the App's panels/keys drive the same members per session, headless
// runs pin them via TOURMALINE_* (RunConfig). Nothing here rides tours/
// paks — the scene's SceneRenderSettings owns the LOOK (bloom/tonemap/
// exposure/AO/SSR/AA/cubemap grades) only. Defaults mirror the
// engine's unauthored state.
// ---------------------------------------------------------------------------
struct RenderQualityConfig {
	int giMode = 0;                  // 0..5 — AmbientMode (All/Lightmaps/
	                                 // Probes/SkyIbl/Off/Baked; the N cycle)
	float giIntensity = 1.0f;        // 0..2 baked-GI + sky-IBL ambient scale
	bool lightmapsEnabled = false;   // proxy lightmap branch (opt-in)
	bool regProbesEnabled = true;    // regular probe grids (dynamic-object GI)
	bool giVolumesEnabled = true;    // convex GI volumes (confined-space GI)
	bool probePremixEnabled = true;  // GPU probe pre-mix (single-fetch path)
	bool evsmEnabled = false;        // EVSM moment shadows (else PCF)
	float evsmSoftness = 1.0f;       // EVSM penumbra artistic scale (0 = hard)
};

// GI permutation variants (matches SPEC_GI_VARIANT in common.hlsli):
// 0 = GI_NONE (sky IBL only), [1 = RESERVED — GI_OCTREE, deleted with
// the .sggi runtime; the shared specialization ABI keeps the ID],
// 2 = GI_REGULAR (regular grids), 3 = GI_VOLUMES (the GI_FULL rename:
// convex volumes over the sky base — the octree underneath is gone)
enum class GIVariant : uint32_t {
	None = 0,
	// (1 reserved: the retired octree)
	Regular = 2,
	Volumes = 3,
};

// ---------------------------------------------------------------------------
// Main-view debug visualization (F1..F6, F9/F10). Lit is the full PBR path;
// the rest replace the opaque leg's fragment shader with a flat debug FS so
// lighting/culling/LOD artifacts bisect out in one keypress. Meshlets/Lods
// reuse the flat meshDebugProgram (per-cluster / per-LOD colors, drawn
// direct BY DESIGN — culled content stays visible); the shading views (see
// DebugFs below) pair their FS with BOTH vertex paths — default.vert on the
// direct fallback leg, gfxfiltered.vert on the GPU-driven leg — so they draw
// exactly what Lit draws with the FS swapped.
// ---------------------------------------------------------------------------
enum class ViewMode : int {
	Lit = 0,      // F1: full PBR (default_lit)
	Unlit = 1,    // F2: baseColor + emissive, no lighting (default_unlit)
	Meshlets = 2, // F3: per-cluster colors, bypasses GPU cull (meshdebug)
	Cascades = 3, // F4: shadow cascade coverage (default_cascade_debug)
	Lods = 4,     // F5: LOD index by color (flat, direct draws)
	VertexColors = 5, // F6: vertex-color layer weights as false color (default_vertexcolor_debug)
	Uv0 = 6,      // F9: base UV (TEXCOORD0) checker + gradient (default_uv0_debug)
	Uv1 = 7,      // F10: lightmap atlas UV (uv2) checker + seam glow (default_uv1_debug)
	Pom = 8,      // F11: parallax occlusion mapping (default_pom)
	SsrContribution = 9, // env/UI only (no F-key: F-keys exhausted):
	                     // SSR contribution isolated (ssr_contribution_debug)
	SsrMask = 10, // env/UI only: SSR receiver mask by material
	              // (default_ssrmask_debug) - green smooth dielectric,
	              // blue smooth metal, black matte (trace early-outs),
	              // dimmed Blend (never writes the G-buffer the trace
	              // reads). Mirrors the G-buffer writer + the trace's
	              // 0.94 matte gate exactly: what you see is what marches.
	Normals = 12, // combo/env only (no free F-key: F7/F8 are taken - see
	              // keybindings.h): world-space geometric normals as
	              // false color (default_normals_debug, n*0.5+0.5 ->
	              // RGB). The interpolated vertex normal only - no
	              // normal-map perturbation (the POM view owns mapped
	              // normals).
	Cubemap = 11, // F10 (reused from the lightmap-UV view, which is now
	              // combo/env-only): cubemap-only IBL - the frame's
	              // cubemapDebug pinned to 1 while active (raw probe
	              // radiance, no Fresnel/roughness weighting; the
	              // cubemaps panel's "Cubemap only (no Fresnel)" mode,
	              // see environment.hlsli). NOT a fragment swap: the view
	              // stays on the filtered lit path - only the env
	              // specular term changes.
	// ---- Diffuse SSGI debug views (combo/env only — no free F-key).
	// Unlike every other non-Lit view these KEEP the lit fragment
	// path AND let the scene post-fx run (IsSsgiDiffuseDebugView
	// excludes them from DebugVisualizationActive): the diffuse SSGI
	// chain itself renders the visualization through its compose
	// shader's debugView push, so the view needs the chain LIVE. The
	// diffuse composite REPLACES the pixel with the visualization
	// (values are raw HDR — exposure holds its last scene value under
	// a debug view; pair with TOURMALINE_AUTO_EXPOSURE=0 for readable
	// captures). The chain must be enabled (TOURMALINE_SSGI=1 / the
	// render-settings checkbox) or the view silently shows Lit. ----
	SsgiConfidence = 13,   // hit confidence (grayscale; the smooth
	                       // screen-edge loss is the expected read)
	SsgiRaw = 14,          // raw trace radiance (pre-blur — noisy,
	                       // recognizable incoming lighting)
	SsgiLayer = 15,        // the filtered GI layer the compose consumes
	                       // (the temporal slot when TAA ran)
	SsgiBaseline = 16,     // the diffuse ambient baseline being replaced
	                       // (the existing probe/sky diffuse)
	SsgiContribution = 17, // the SIGNED s*(ssgi - baseline) delta
	                       // (positive brightens, negative darkens)
};

// The diffuse SSGI debug views (see the ViewMode block): rendered by
// the SSGI compose's debugView push, not a fragment swap — these views
// need the post-fx chain LIVE, so DebugVisualizationActive excludes
// them (the flat-FS views replace the scene; these annotate it).
[[nodiscard]] inline bool IsSsgiDiffuseDebugView(ViewMode v) {
	switch (v) {
	case ViewMode::SsgiConfidence:
	case ViewMode::SsgiRaw:
	case ViewMode::SsgiLayer:
	case ViewMode::SsgiBaseline:
	case ViewMode::SsgiContribution:
		return true;
	default:
		return false;
	}
}

// The compose shader's debugView push value for an Ssgi view (0 = the
// normal composite; see SsgiDiffuseComposePush::debugView).
[[nodiscard]] inline uint32_t SsgiDiffuseDebugViewId(ViewMode v) {
	switch (v) {
	case ViewMode::SsgiConfidence: return 1u;
	case ViewMode::SsgiRaw: return 2u;
	case ViewMode::SsgiLayer: return 3u;
	case ViewMode::SsgiBaseline: return 4u;
	case ViewMode::SsgiContribution: return 5u;
	default: return 0u;
	}
}

// ---------------------------------------------------------------------------
// Flat shading debug-view FS slots (one FS per view; ViewMode above): the
// program PAIRS key on this — the direct twin (default.vert + FS) rides the
// direct fallback leg (unfilterable / shader-LOD-far instances; the whole leg
// only when the pass config kills the filter or setup latched it off), the filtered twin (gfxfiltered.vert + FS, built via
// GfxRenderer::FilteredProgramWith) rides the GPU-driven leg exactly like
// Lit: same cull chain, same LOD selection, same skinned fetch. SsrContribution
// is NOT here on purpose: its FS declares its own 64B push layout (not the
// scene pc the FS family shares), so it keeps its legacy no-opaque-leg
// behavior until a shader-side rework.
// ---------------------------------------------------------------------------
enum class DebugFs : int {
	Unlit = 0,    // default_unlit (F2)
	Cascade,      // default_cascade_debug (F4)
	VertexColor,  // default_vertexcolor_debug (F6)
	Uv0,          // default_uv0_debug (F9)
	Uv1,          // default_uv1_debug (F10)
	Pom,          // default_pom (F11)
	SsrMask,      // default_ssrmask_debug
	Normals,      // default_normals_debug (combo/env only)
	Count
};

// Maps a shading debug view onto its FS slot; nullopt for everything else
// (Lit, the flat meshDebug views, Cubemap, SsrContribution).
[[nodiscard]] inline std::optional<DebugFs> DebugFsForView(ViewMode v) {
	switch (v) {
	case ViewMode::Unlit: return DebugFs::Unlit;
	case ViewMode::Cascades: return DebugFs::Cascade;
	case ViewMode::VertexColors: return DebugFs::VertexColor;
	case ViewMode::Uv0: return DebugFs::Uv0;
	case ViewMode::Uv1: return DebugFs::Uv1;
	case ViewMode::Pom: return DebugFs::Pom;
	case ViewMode::SsrMask: return DebugFs::SsrMask;
	case ViewMode::Normals: return DebugFs::Normals;
	default: return std::nullopt;
	}
}

#if defined(DS_DEBUG_VIZ)
// ---------------------------------------------------------------------------
// Deferred-shadow debug visualizers (paper sections 5.3/5.4; debug builds
// only — DS_DEBUG_VIZ, see app/CMakeLists.txt + CompileShaders.cmake).
// Rendered in default_lit.frag.hlsl, blended over the lit image or
// replacing it. Numeric values must match the DS_VIZ_* defines in
// engine/shaders/deferredshadow.hlsli.
// ---------------------------------------------------------------------------
enum class DsDebugViz : int {
	Off = 0,            // visualizer off (lit image untouched)
	CountApprox = 1,    // 5.4a: phase-2 approximated lights per Shadow Tile
	CountAccurate = 2,  // 5.4b: phase-3 early-list size per Shadow Tile
	CountFinal = 3,    // phase-4 final-list size per Shadow Tile
	CountDiff = 4,      // 5.4c: approx - accurate (where phase 2 is weakest)
	CullStep = 5,      // 5.3a: last culling step passed by the selected light
	LightList = 6,      // 5.3b: which light list holds the selected light
	LightVis = 7,       // selected light per-pixel visibility byte
};
#endif

// Resolved scene lights (KHR_lights_punctual via the pak/glTF import,
// snapshotted from the live world at load/edit time — NOT cooker output,
// despite the old name). A resolved directional overwrites a sceneless
// env's default sun values at bind time and is otherwise env-owned
// (single source; nothing retained here). Points/spots upload directly as the Forward+ set
// (static, no orbit/bob). Empty on scenes without authored
// lights (e.g. Sponza) — the Forward+ set is then empty and only the
// sun/ambient terms light the frame.
struct ResolvedSceneLights {
	// World-space static lights, spots FIRST (per-tile lists keep only
	// the first FP_MAX_PER_TILE passing lights — cone lights must not
	// compete with the point crowd for those slots).
	// No sun flag: the scene env ALWAYS carries sun values (SceneEnv
	// defaults) and a resolved directional overwrites them at bind time —
	// there is no authored-vs-default sun state to retain. The election
	// lives in RefreshSceneLights locals and dies on return.
	std::vector<smvk::ForwardPlusLight> forwardPlusLights;
	void clear() { forwardPlusLights.clear(); }
	bool hasLights() const { return !forwardPlusLights.empty(); }
};

// ---------------------------------------------------------------------------
// GPU particle demo (see smvk::Particles + particles_emit/sim/draw shaders).
// The CPU only owns the emitter PARAMETER rows below: spawn scheduling
// (rate carry), simulation and drawing all run on the GPU. Rows re-upload
// every frame (ring slice like the decal splats), so these sliders retouch
// live emitters immediately; the GPU keeps its own fractional-rate carry
// in a separate buffer the uploads never touch.
// ---------------------------------------------------------------------------
struct ParticleDemo {
	bool enabled = true;
	// Global sim gravity (m/s^2; per-emitter gravityScale multiplies it).
	float gravityY = -9.81f;
	// Live emitter parameters (capped at PARTICLE_MAX_EMITTERS on upload).
	std::vector<smvk::ParticleEmitterGpu> emitters;
	int selected = 0; // UI combo index into `emitters`
	// Steady-state alive estimate for the UI readout: sum(rate * avg life).
	float SteadyStateCount() const {
		double n = 0.0;
		for (const auto& e : emitters)
			n += (double)e.life.z * (double)(e.life.x + e.life.y) * 0.5;
		return (float)n;
	}
};

// Deferred-shadow visualizer parameters (paper 5.3/5.4).
// Standalone 32-byte payload, uploaded into a tiny per-flight ring and
// reached through FrameUniforms::dsVizParams — deliberately NOT inlined
// in FrameUniforms: viz-gated #ifdefs inside GPU-pushed structs once made
// the CPU/GPU layout config-dependent (per-flight stride bugs). This
// struct is UNCONDITIONAL on both sides (C++/HLSL mirror in
// engine/shaders/deferredshadow.hlsli); only its FILL (UpdateFrameUniforms)
// and READ (default_lit viz overlay) are DS_DEBUG_VIZ-gated.
struct DsVizParams {
	uint64_t tileMasks = 0;      // phase-1 CPU TileGroup masks (viz)
	uint64_t tileGroupMasks = 0; // phase-2 GPU TileGroup masks (viz)
	uint32_t mode = 0;           // engine::DsDebugViz (0 = off)
	uint32_t light = 0;          // selected light (sorted index)
	uint32_t flags = 0;          // bit0 = blend over lit
	uint32_t countCeil = 32;     // count-heat ceiling (lights)
};
static_assert(sizeof(DsVizParams) == 32, "DsVizParams must stay 32 bytes (HLSL mirror)");

// FrameUniforms::flags bits (unconditional on both sides; HLSL mirror:
// DS_FLAG_VIZ in common.hlsli). Lets the shader skip payload dereferences
// for off-by-default features without changing the struct layout.
static constexpr uint64_t kFrameFlagDsViz = 1ull << 0; // DS viz overlay active this view
// JHFAA-style shading-normal validity correction active this view
// (shading_normal_fix.hlsli; TOURMALINE_NORMAL_FIX pin — experimental,
// default off while under evaluation, see docs/jhfaa_normal_fix.md).
static constexpr uint64_t kFrameFlagNormalFix = 1ull << 1;
// Violation-overlay debug view (TOURMALINE_NORMAL_FIX_DEBUG=1): flat
// red/orange palette from the UN-corrected normal test — independent of
// the correction bit so the disease (off) and the residue (on) can be
// viewed apart.
static constexpr uint64_t kFrameFlagNormalFixDebug = 1ull << 2;

struct FrameUniforms
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewProj;

	glm::mat4 invView;
	glm::mat4 invProj;
	glm::mat4 invViewProj;
	glm::mat4 prevViewProj; // last frame's proj*view (post Y-flip) – motion vectors

	glm::vec3 cameraPos;
	float     time;

	glm::vec2 viewportSize;
	glm::vec2 invViewportSize;

	float nearPlane;
	float farPlane;
	float exposure;
	// AO (DORMANT since the SSGI rework): the lighting pass no longer
	// samples AO — Main exports the AO-sensitive lighting through the
	// aoTerm MRT target and the SSGI compose pass applies it (see ssgi.h
	// + ssao_compose.frag.hlsl). The FIELDS stay: FrameUniforms offsets
	// are asserted vs the HLSL mirror — removing them would shift every
	// later field. Always UINT32_MAX / 0 ("no AO here") for any future
	// reader.
	// Must match HLSL FrameUniforms in common.hlsli — offsets asserted below.
	uint32_t aoTextureIndex = UINT32_MAX; // dormant (was: full-res composed AO bindless)
	float aoIntensity = 0.0f;            // dormant (was: ambient composite strength)
	// Explicit pad so env lands at offset 512 (16-byte aligned), matching
	// HLSL FrameUniforms::aoPad. Without this the HLSL compiler packs env
	// at offset 500, which is both a spirv-val Block-layout violation for
	// PhysicalStorageBuffer and a CPU/GPU desync (garbage sun/IBL/fog).
	float aoPad_[3] = {};
	// Configurable skydome IBL + fog (matches HLSL FrameUniforms.env).
	smvk::EnvironmentFrameData env;

	// Reflection cubemap for specular IBL (bindless TextureCube index).
	// NOTE: this 16-byte block must stay exactly 16 bytes (offsets
	// asserted below): index + sampler + intensity + debug. Growing it
	// shifts probes/DS/GI/evsm and corrupts shadow reads on the GPU.
	uint32_t cubemapIndex = UINT32_MAX; // 640 global fallback cubemap
	uint32_t cubemapSampler = 0;        // 644 clamp sampler for TextureCube
	float cubemapIntensity = 1.0f;      // 648 specular IBL scale (UI)
	uint32_t cubemapDebug = 0;          // 652 0 off, 1 cubemap-only, 2 probe weight
	// Reflection probes for local specular IBL ride the Forward+ light
	// buffer as type-2 entries (see smvk::kForwardPlusProbe) — no probe
	// storage buffer, no parallel tile lists. probeEntries is the tail
	// length: the shader's index bound is fpLightCount + probeEntries
	// (lights [0,L), probes [L,L+P) — a lights-only bound skips probes).
	float cubemapLodBias = 0.0f;       // 656 added to roughness LOD (UI)
	uint32_t probeTileFresh = 0;       // 660 1 = tile cull filed probe entries
	uint32_t probeEntries = 0;         // 664 type-2 tail length
	uint32_t probePad_ = 0;            // 668 16-align tail
	// ---- Deferred shadows (tiled/zBin pipeline, see
	// smvk::DeferredShadows + deferredshadow*.hlsli). Raw BDAs: the HLSL
	// side declares matching vk::BufferPointer fields. dsEnabled flips the
	// default_lit consumer between the deferred lists and the inline
	// forward+ shadow path (A/B). ----
	uint64_t dsTileState = 0;   // 672 per-tile {slot, count, approx, early}
	uint64_t dsLightList = 0;   // 680 final packed entries
	uint64_t dsVisibility = 0;  // 688 64B visibility slots
	uint32_t dsTileCountX = 0;  // 696 8px shadow tiles per row
	uint32_t dsEnabled = 0;     // 700 1 = shade from the deferred lists
	// ---- SG-GI baked probe volume (see smvk::gi + sggi.hlsli). Static
	// buffers created + uploaded once at scene load; giEnabled flips the
	// default_lit ambient between the probe field and the analytic sky
	// IBL (A/B). giDebug = F8 flat irradiance view. ----
	uint64_t giProbes = 0;      // 704 SgProbeGpu array (96 B each)
	uint64_t giGrid = 0;        // 712 SgGiGrid level descriptors
	uint64_t giCells = 0;       // 720 uint2 (start, count) per cell
	uint64_t giIndices = 0;     // 728 u32 probe index list
	uint32_t giEnabled = 0;     // 736 1 = probes replace sky ambient
	float giIntensity = 1.0f;   // 740 ambient scale
	uint32_t giDebug = 0;       // 744 F8 irradiance debug view
	// EVSM shadow mode (offset 748, was padding): 0 = PCF depth atlases,
	// 1 = prefiltered EVSM moments (blurred RG32F, 1-frame lag). The
	// shadow_map_index / spotShadowMapIndex fields then address the
	// blurred EVSM atlas instead of the depth atlas. Same offset/size as
	// HLSL FrameUniforms::evsmMode — see the static_asserts below.
	uint32_t evsmMode = 1;      // 748
	// ---- Forward+ decals (tiled projected splats, see
	// smvk::ForwardPlusDecal + forwardplus.cs.hlsl). decals points at
	// this frame's flight slice of the decal ring; the tile lists are the
	// ForwardPlus builder's decal buffers (same tiling as the light
	// lists; the tile-cull dispatch refreshes them every frame it runs,
	// storing 0 with no decals). decalCount > 0 only when the cull
	// dispatch runs this frame, so the shader never reads stale lists —
	// and the tile pointers stay valid whenever the cull can run. ----
	uint64_t decals = 0;            // 752 per-frame decal ring slice
	uint64_t decalTileCounts = 0;   // 760 builder decal list counts
	uint64_t decalTileIndices = 0;  // 768 builder decal list indices
	uint32_t decalCount = 0;        // 776 live decals this frame
	uint32_t decalMaxPerTile = 0;   // 780 list stride (matches allocation)
	// (Historical slot of the SG 2D lightmap fields: restored at the
	// APPEND-ONLY tail instead, so every offset below stayed put.)
	// ---- SG regular OBBs (dynamic geometry, Engine/GI/lightmap.h): the
	// per-object OBB array lives in a uniform buffer. regCount is the
	// active OBB count; the shader's loop bound is min(regCount, MAX_REG).
	// regEnabled flips the default_lit ambient between the OBB field and
	// the lightmap branch (which itself may be off). ----
	// Offsets below are POST-lightmap-deprecation (#if 0 block above is
	// compiled out): everything shifted 64 bytes down from the stale
	// pre-deprecation comments.
	uint64_t regObbs = 0;       // 784 SgRegularObbGpu array (56 B each)
	uint32_t regLayers[56] = {};    // 792 8 volumes x 7 layer slots
	uint32_t regCount = 0;      // 1016 active OBB count
	uint32_t regEnabled = 0;    // 1020 1 = OBB branch active
	// 12 bytes: pads giVolumes to 1040, matching HLSL regPad (uint4,
	// 16 bytes at 1024). A single 4-byte pad here drifted the GI-volume
	// fields 8 bytes against the HLSL layout (latent GI-volume/reg
	// corruption; fixed 10/2, under the historical cvol name).
	uint32_t regPad_[3] = {};   // 1024 (16-align to giVolumes@1040)
	// ---- Convolution volumes (dynamic geometry, Engine/GI/lightmap.h):
	// giVolumes points at one GiVolumesBuffer BDA (8 volume headers +
	// the shared packed plane pool, sg_lightmap_types.hlsli). giVolumeCount
	// is the active volume count; giVolumesEnabled toggles the branch.
	// Static/dynamic separation: draws carrying giVolumeCount > 0 sample the
	// volumes; static draws skip. ----
	uint64_t giVolumes = 0;   // 1040 GiVolumesBuffer (headers + plane pool)
	uint32_t giVolumeLayers[56] = {};  // 1048 8 volumes x 7 layer slots
	uint32_t giVolumeCount = 0;     // 1272 active volume count
	uint32_t giVolumesEnabled = 0;   // 1276 1 = volume branch active
	// Convex-volume tile masks (the cull's per-tile bitmask, see
	// forwardplus.cs.hlsl): address + tile grid publish together under
	// the decal tile-list freshness contract; zeros together mean "no
	// tile cull refreshed the masks this view/frame" — the shader then
	// walks every volume (per-volume AABB reject keeps it correct).
	uint64_t giVolumeTileMasks = 0;  // 1280 per-tile volume bitmask buffer
	uint32_t giVolumeTileCountX = 0; // 1288 mask tile grid width (0 = no mask)
	uint32_t giVolumeTileSize = 0;   // 1292 mask tile size (16; fp tiling)
	// ---- FramePSHot slab BDA (the FS-only hot struct, see FramePSHot
	// below): the pixel shader reaches the hot slab through FrameUniforms
	// (`FramePSHot hot = frame.frameHot.Get()`) instead of a pc member —
	// a pc member shifted every sub-192 pc field and broke the HLSL
	// mirror structs (the 10/1 device-loss crash; see
	// Engine/Renderer/Renderer.h layout invariant). Filled per frame in
	// UpdateFrameUniforms. ----
	uint64_t frameHot = 0;      // 1296 this view's FramePSHot slab
	// ---- Deferred-shadow visualizer params (paper 5.3/5.4): BDA of this
	// view's DsVizParams slice — the standalone 32 B payload (per-flight
	// ring + one zero slice shared by secondary views, see
	// UpdateFrameUniforms). Deliberately a BDA, not inlined fields:
	// viz-gated #ifdefs inside this struct once made the CPU/GPU layout
	// config-dependent. This field is UNCONDITIONAL (same offset/size in
	// every build); only its fill and read are DS_DEBUG_VIZ-gated. ----
	uint64_t dsVizParams = 0;   // 1304 this view's DsVizParams slice
	// ---- Per-frame feature flags (bitmask; upper bits reserved). Lets
	// shaders skip off-by-default payloads (like dsVizParams above)
	// without a BDA chase. Unconditional like every other field. ----
	uint64_t flags = 0;         // 1312 kFrameFlag* bits
	// ---- Dense octree resample (the BC6H Texture3D runtime path — see
	// m_giLevelTextures): per level 9 SG-lobe RGB layers (BC6H 3D) + 1
	// weight layer (R16F 3D) in the shared volumeTextures bindless
	// array; UINT32_MAX = absent level (beyond the baked levelCount).
	// The diffuse sampler trilinear-filters these instead of walking
	// probe lists per pixel (the probes never move). NOTE: uint32
	// arrays align to 4 (not 16) on both sides — this member sits at
	// 1320, matching the HLSL mirror exactly (see the asserts below).
	uint32_t giLevelLayers[80] = {}; // 1320 8 levels x (9 amp + 1 weight)
	// ---- Convex-volume isotropic-mean textures (the volumetric fog's
	// one-fetch-per-volume payload — PackGpuMeanLayer +
	// SgCvSampleVolumeMean, blended by SgCvRayComposite): 8 bindless
	// slots, UINT32_MAX = none. Deliberate
	// APPEND-ONLY tail (after giLevelLayers): every pinned offset above
	// stays put when this grows. ----
	uint32_t giVolumeMeanLayers[8] = {}; // 1640 8 volumes x 1 mean texture slot
	// ---- World env tiles (Engine/WorldPartition/WorldEnvTiles.h — the
	// runtime rework of the old OBB-volume zone system): the zone tables
	// BAKE onto the 64 m world-tile grid (a dirty-hash rebuild, not a
	// per-frame pack) and the shaders resolve a zone by TILE ADDRESS, not
	// per-pixel OBB intersection. envZones is the RAW BDA of the
	// EnvTileRecord array (records[0] = the base/default look — the old
	// zone-zero), envZoneCount = live records (0 = every consumer takes
	// the plain env path), envZoneFlags bit0 = tiles live, bit1 = ANY
	// live record authors volumetric fog (a positive absolute
	// volumetricDensity — the volumetric pass's zone-density gate leg;
	// packed from WorldEnvTiles::AnyVolumetricFog, the SAME scan the
	// CPU volOn gates read). The old
	// reserved block is re-split IN PLACE into the tile-grid header +
	// a shorter reserved — every pinned offset below stays put
	// (lmLayers@1888, probe@1952, worldFields@1976).
	// APPEND-ONLY tail (after giVolumeMeanLayers): every pinned offset
	// above stays put when this grows. ----
	uint64_t envZones = 0;      // 1672 EnvTileRecord array BDA (raw)
	uint32_t envZoneCount = 0;  // 1680 live records (incl. base)
	uint32_t envZoneFlags = 0;  // 1684 bit0 = tiles live, bit1 = zone fog authored
	float envTileMinX = 0.0f;   // 1688 tile-grid min corner X (m)
	float envTileMinZ = 0.0f;   // 1692 min corner Z
	uint32_t envTileDimX = 0;   // 1696 tiles per axis (64 m each)
	uint32_t envTileDimZ = 0;   // 1700
	uint64_t envTileCells = 0;  // 1704 the EnvTileCell grid BDA (raw)
	uint32_t envTileReserved[42] = {}; // 1712 (keeps lmLayers@1888 pinned)
	// ---- SG 2D lightmaps (STATIC geometry, Engine/GI/lightmap.h):
	// atlas layers ride the bindless texture slots. lmLayerKind selects
	// the layout: 0 = the classic 9 RGBA16F stream layers, 1 = BC6H
	// one-texture-per-lobe (12 slots, texture l = lobe l; slots past
	// the scene's lobe count stay UINT32_MAX). APPEND-ONLY tail (after
	// envZoneReserved): every pinned offset above stays put. ----
	uint32_t lmPad0_[2] = {};          // 1880 (16-align to lmLayers@1888)
	uint32_t lmLayers[12] = {};        // 1888 atlas bindless slots
	uint32_t lmEnabled = 0;            // 1936 1 = lightmap branch active
	uint32_t lmLayerKind = 0;          // 1940 0 = RGBA16F stream, 1 = BC6H
	uint32_t lmPad_[2] = {};           // 1944 (16-align tail; struct was 1952)
	// ---- GPU probe pre-mix (docs/probe_premix_plan.md; the Remember Me
	// architecture, GPU-only selection). probePremixIndex = the BLENDED
	// cubemap's bindless index (UINT32_MAX = off: the per-pixel walk owns
	// the specular env unchanged). probePremixMips = the blended chain's
	// mip count (the roughness LOD clamp). probePremixSel = the weights
	// pass's selection buffer BDA - the material's single-fetch branch
	// reads the DOMINANT probe's packed depth-fitted parallax box
	// (boxMinus/boxPlus plane distances, the same packing the lit walk
	// reads off the light tail) straight from it: no CPU readback, no
	// staging, no classification anywhere. The selection is GPU-owned
	// (written by the weights pass each frame it runs). APPEND-ONLY tail
	// (after lmPad_): every pinned offset above stays put when this
	// grows. ----
	uint32_t probePremixIndex = UINT32_MAX; // 1952 blended cubemap (off = max)
	uint32_t probePremixMips = 0;           // 1956 blended chain mip count
	uint64_t probePremixSel = 0;            // 1960 selection BDA (weights pass output)
	uint32_t probePremixPad_[2] = {};      // 1968 (16-align tail; struct was 1984)
	// ---- World voxel fields (Engine/WorldFields, the atlas-backed
	// virtual world volume system; sampler in world_field.hlsli, layout
	// contract in Engine/WorldFields/WorldFieldGpu.h). worldFields = the
	// BDA of the field/page-table storage buffer (0 = subsystem off —
	// SampleWorldField returns zeros); the buffer + BDA are stable for
	// the system's lifetime, the CONTENT re-publishes on residency moves.
	// worldFieldCount = live fields (ids are registration order, fog
	// first). APPEND-ONLY tail: every pinned offset above stays put. ----
	uint64_t worldFields = 0;      // 1976 field/page-table BDA (0 = off)
	uint32_t worldFieldCount = 0;  // 1984 live GPU fields
	uint32_t worldFieldsPad_ = 0; // 1988 (16-align tail; struct was 1992)
	// ---- GI-volume VISIBILITY layers (the through-wall rejection —
	// SgGiVolumeSample in sg_gi_volume.hlsli): 2 RGBA8_UNORM 3D
	// textures per volume (visA = +x,-x,+y,-y obstacle distances; visB
	// = +z,-z, the forward-cell clean flag, spare — unorm8 over [0,15]
	// spacing units). UINT32_MAX = absent (the pre-visibility behavior:
	// the plain hardware trilerp). APPEND-ONLY tail (after
	// worldFieldsPad_): every pinned offset above stays put. ----
	uint32_t giVolumeVisLayers[16] = {}; // 1992 8 volumes x 2 vis slots
};
// FrameUniforms layout insurance: the HLSL mirror (common.hlsli) reads
// every field by absolute offset — any drift desyncs the GPU reads.
// dsVizParams must directly follow frameHot (no config-dependent tail).
static_assert(offsetof(FrameUniforms, frameHot) == 1296, "frameHot offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, dsVizParams) == 1304, "dsVizParams offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, flags) == 1312, "flags offset: HLSL mirror");
// GI-volume block pinned end-to-end like the GI block above: the volumes'
// BDA/count/enable/mask fields are HLSL-mirrored by absolute offset.
static_assert(offsetof(FrameUniforms, giVolumes) == 1040, "giVolumes offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumeCount) == 1272, "giVolumeCount offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumesEnabled) == 1276, "giVolumesEnabled offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumeTileMasks) == 1280, "giVolumeTileMasks offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumeTileCountX) == 1288, "giVolumeTileCountX offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumeTileSize) == 1292, "giVolumeTileSize offset: HLSL mirror");
// GI block (the lit shader's frame.gi* reads + the F8 debug views live
// or die by these — the debug view once silently read the wrong word
// here, so every member of the block is pinned, not just the tail).
static_assert(offsetof(FrameUniforms, giProbes) == 704, "giProbes offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giGrid) == 712, "giGrid offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giCells) == 720, "giCells offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giIndices) == 728, "giIndices offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giEnabled) == 736, "giEnabled offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giIntensity) == 740, "giIntensity offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giDebug) == 744, "giDebug offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, evsmMode) == 748, "evsmMode offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giLevelLayers) == 1320, "giLevelLayers offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumeMeanLayers) == 1640, "giVolumeMeanLayers offset: HLSL mirror");
// Env-zone block (the append-only tail): the per-frame EnvZoneGpu slice
// BDA + count + reserved words, pinned like every other HLSL-mirrored
// member. The reserved array keeps the old 208-byte block size so the
// lightmap/probe tail offsets below do not move.
static_assert(offsetof(FrameUniforms, envZones) == 1672, "envZones offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envZoneCount) == 1680, "envZoneCount offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envZoneFlags) == 1684, "envZoneFlags offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envTileMinX) == 1688, "envTileMinX offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envTileMinZ) == 1692, "envTileMinZ offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envTileDimX) == 1696, "envTileDimX offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envTileDimZ) == 1700, "envTileDimZ offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envTileCells) == 1704, "envTileCells offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, envTileReserved) == 1712, "envTileReserved offset: HLSL mirror");
// SG 2D lightmap tail (append-only; see the block above envZoneReserved).
static_assert(offsetof(FrameUniforms, lmLayers) == 1888, "lmLayers offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, lmEnabled) == 1936, "lmEnabled offset: HLSL mirror");
// Probe pre-mix block (the append-only tail): pinned like every other
// HLSL-mirrored member (the material's single-fetch branch reads these
// by absolute offset).
static_assert(offsetof(FrameUniforms, probePremixIndex) == 1952, "probePremixIndex offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, probePremixMips) == 1956, "probePremixMips offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, probePremixSel) == 1960, "probePremixSel offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, lmLayerKind) == 1940, "lmLayerKind offset: HLSL mirror");
// World-fields tail (the append-only block): the table BDA + live count
// the shared sampler reads.
static_assert(offsetof(FrameUniforms, worldFields) == 1976, "worldFields offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, worldFieldCount) == 1984, "worldFieldCount offset: HLSL mirror");
static_assert(offsetof(FrameUniforms, giVolumeVisLayers) == 1992, "giVolumeVisLayers offset: HLSL mirror");
static_assert(sizeof(FrameUniforms) == 2064, "FrameUniforms size: HLSL mirror (2056 of fields + the 16-align tail pad)");

// FramePSHot — tiny per-frame data frequently accessed by the pixel shader.
// Mirrors HLSL FramePSHot in common.hlsli (96 bytes, 16-byte aligned).
// Uploaded as a separate buffer slice per frame to keep PS register pressure low.
struct FramePSHot
{
	glm::vec3 cameraPos;        // 0
	float     time;             // 12 -> 16
	glm::vec2 viewportSize;     // 16
	glm::vec2 invViewportSize;  // 24 -> 32
	float     nearPlane;        // 32
	float     farPlane;         // 36
	float     exposure;         // 40
	uint32_t  aoTextureIndex;   // 44
	float     aoIntensity;      // 48
	uint32_t  evsmMode;         // 52
	uint32_t  dsEnabled;        // 56
	uint32_t  dsTileCountX;     // 60
	float     fogEndAndFlags_w; // 64 (env.fogEndAndFlags.w = iblEnabled)
	float     giIntensity;      // 68
	uint32_t  giDebug;          // 72
	uint32_t  giVolumesEnabled;      // 76
	uint32_t  pad_[3];          // 80 -> 96 (16-byte aligned total)
};
#if 0 // Lightmaps deprecated - size/offsets changed
static_assert(sizeof(FramePSHot) == 96);
static_assert(offsetof(FramePSHot, cameraPos) == 0);
static_assert(offsetof(FramePSHot, time) == 12);
static_assert(offsetof(FramePSHot, viewportSize) == 16);
static_assert(offsetof(FramePSHot, invViewportSize) == 24);
static_assert(offsetof(FramePSHot, nearPlane) == 32);
static_assert(offsetof(FramePSHot, farPlane) == 36);
static_assert(offsetof(FramePSHot, exposure) == 40);
static_assert(offsetof(FramePSHot, aoTextureIndex) == 44);
static_assert(offsetof(FramePSHot, aoIntensity) == 48);
static_assert(offsetof(FramePSHot, evsmMode) == 52);
static_assert(offsetof(FramePSHot, dsEnabled) == 56);
static_assert(offsetof(FramePSHot, dsTileCountX) == 60);
static_assert(offsetof(FramePSHot, fogEndAndFlags_w) == 64);
static_assert(offsetof(FramePSHot, giIntensity) == 68);
static_assert(offsetof(FramePSHot, giDebug) == 72);
static_assert(offsetof(FramePSHot, lmEnabled) == 76);
static_assert(offsetof(FramePSHot, giVolumesEnabled) == 80);
#endif

// Resources
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::Holder<smvk::BufferHandle> frameHot;
	smvk::Holder<smvk::BufferHandle> lightsBuffer;
	smvk::Holder<smvk::BufferHandle> materialsBuffer;
	smvk::Holder<smvk::BufferHandle> directionalCascadeBuffer; // GpuDirectionalLightDescription per frame
	smvk::Holder<smvk::BufferHandle> fpLightsBuffer; // ForwardPlusLight * FP_MAX_LIGHTS per frame
	smvk::LogicalBufferHandle fpLightsLogical{}; // rendergraph alias (stable: buffer never resized)
	// Decal ring (ForwardPlusDecal * FP_MAX_DECALS per flight frame):
	// rebuilt + uploaded every frame from the WORLD's decal entities
	// (see UpdateDecals — per-frame re-derivation, live edits included).
	smvk::Holder<smvk::BufferHandle> decalsBuffer;
	smvk::LogicalBufferHandle decalsLogical{}; // rendergraph alias (stable)
	// World env tiles (Engine/WorldPartition/WorldEnvTiles.h — the runtime
	// rework of the old per-frame zone ring): the zone LOOK records
	// (EnvTileRecord[64], records[0] = the base) + the 64 m tile CELLS
	// (EnvTileCell grid, recreated when the grid extent changes). BAKED
	// from the WORLD's zone entities on a dirty hash (see UpdateEnvTiles —
	// load/edit, not per frame). No rendergraph alias: reached through
	// FrameUniforms::envZones/envTileCells (raw BDAs) by the terrain/
	// volumetric FS via the tile-address resolver.
	smvk::Holder<smvk::BufferHandle> envTileRecordsBuffer;
	smvk::Holder<smvk::BufferHandle> envTileCellsBuffer;
	smvk::Holder<smvk::BufferHandle> spotShadowBuffer; // GpuCascadeDescription * kMaxLocalShadowTiles per frame (spot tiles + point faces)
	smvk::LogicalBufferHandle spotShadowLogical{}; // rendergraph alias (stable: buffer never resized)
	// DS visualizer params ring (DsVizParams per flight + one zero slice
	// shared by secondary views, which never viz; reached through
	// FrameUniforms::dsVizParams). No rendergraph alias: written by upload,
	// read by the lit FS only.
	smvk::Holder<smvk::BufferHandle> dsVizParamsBuffer;
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader; // default_lit (F1)
	smvk::ShaderProgram mainProgram;
	// Alpha-to-coverage lit FS (default_lit_a2c): same VS, coverage-from-
	// alpha cutout without discard — bound for Masked materials in 2x MSAA
	// Main. Missing module leaves shaderCount 0 and Main keeps the
	// discard variant everywhere.
	smvk::Holder<smvk::ShaderModuleHandle> fragA2CShader;
	smvk::ShaderProgram mainProgramA2C;
	// GI permutation lit programs (one per GI variant: NONE/OCTREE/REGULAR/FULL).
	// Selected at draw time based on loaded GI assets (WorldRenderer::GIVariant).
	smvk::ShaderProgram mainProgramGi[4];
	// Standard-material twins (default_lit .std/.giNstd — MAT_STANDARD: the
	// layered machinery compiled out). Bound for materials with
	// layerMaterials.w <= 1; layered materials keep the programs above
	// (fail-safe: missing std modules fall back to the monolith picks).
	smvk::Holder<smvk::ShaderModuleHandle> fragStdModule;
	smvk::ShaderProgram mainProgramStd;
	smvk::Holder<smvk::ShaderModuleHandle> fragLitGiStdModules[4];
	smvk::ShaderProgram mainProgramGiStd[4];
	// Fixed-lobe lit FS twins (default_lit SG_LM_LOBE_COUNT variants):
	// same VS + per-count SG evaluation, bound per filtered material
	// range (see GfxRenderer::FilteredLitProgram). Missing modules leave
	// that count on the generic lit program.
	smvk::Holder<smvk::ShaderModuleHandle> fragLitSgModules[4];
	// GI permutation lit FS variants (default_lit GI_NONE/OCTREE/REGULAR/FULL):
	// same VS + per-permutation GI path. Missing modules leave the base
	// mainProgram as fallback. See SPEC_GI_VARIANT in common.hlsli.
	smvk::Holder<smvk::ShaderModuleHandle> fragLitGiModules[4];
	// Shading debug-view program PAIRS (F2/F4/F6/F9/F10/F11 + SsrMask):
	// one entry per DebugFs slot — direct (default.vert + FS) for the
	// direct fallback leg, filtered (gfxfiltered.vert + FS, via
	// GfxRenderer::FilteredProgramWith) for the GPU-driven leg (see
	// DebugFs above). Missing modules leave the pair's shaderCount 0 and
	// the view falls back to Lit at record time.
	struct DebugProgramPair {
		smvk::ShaderProgram direct;
		smvk::ShaderProgram filtered;
	};
	DebugProgramPair debugPrograms[size_t(DebugFs::Count)]{};
	smvk::Holder<smvk::ShaderModuleHandle> fragUnlitShader;   // default_unlit (F2)
	smvk::Holder<smvk::ShaderModuleHandle> fragCascadeShader; // default_cascade_debug (F4)
	smvk::Holder<smvk::ShaderModuleHandle> fragVertexColorShader; // default_vertexcolor_debug (F6)
	// UV debug views (F9 base UV / F10 lightmap uv2): checker + gradient
	// FS pairs, channel selected per compiled module.
	smvk::Holder<smvk::ShaderModuleHandle> fragUv0DebugShader; // default_uv0_debug (F9)
	smvk::Holder<smvk::ShaderModuleHandle> fragUv1DebugShader; // default_uv1_debug (F10)
	// Parallax occlusion mapping view (F11): occlusion-map heightfield
	// march + sun self-shadow, sun/sky shading sampled at the displaced
	// uv (see default_pom.frag.hlsl). Missing module leaves shaderCount 0
	// and the view falls back to Lit.
	smvk::Holder<smvk::ShaderModuleHandle> fragPomShader; // default_pom (F11)
	// SSR contribution debug view (env/UI only): isolated SSR layer
	// (ssr_contribution_debug.frag.hlsl). NOT part of debugPrograms: the
	// FS declares its own push layout (SsrContributionDebugPush), so it
	// cannot ride the scene-push FS family; the view keeps its legacy
	// no-opaque-leg behavior.
	smvk::Holder<smvk::ShaderModuleHandle> fragSsrContributionDebugShader; // ssr_contribution_debug
	smvk::ShaderProgram ssrContributionDebugProgram;
	// SSR receiver-mask debug view (env/UI only): per-material classification
	// mirroring the thin G-buffer writer + the trace matte gate
	// (default_ssrmask_debug.frag.hlsl). Missing module keeps shaderCount 0
	// and the view falls back to Lit at record time (same as unlit/etc.).
	smvk::Holder<smvk::ShaderModuleHandle> fragSsrMaskDebugShader;
	// World-space normals debug view (combo/env only): the interpolated
	// geometric normal as false color (default_normals_debug.frag.hlsl).
	smvk::Holder<smvk::ShaderModuleHandle> fragNormalsDebugShader;
	// Shader-LOD farthest-level program (see World::UseVertexLitShaderAt):
	// its own VS computes the lighting per vertex (see
	// default_vertexlit.vert.hlsl) and the FS is a fog pass-through.
	// Missing modules leave shaderCount 0 and every level stays lit.
	smvk::Holder<smvk::ShaderModuleHandle> vertexLitVertShader;
	smvk::Holder<smvk::ShaderModuleHandle> vertexLitFragShader;
	smvk::ShaderProgram vertexLitProgram;
	// Wireframe (P) flat-line twins: default_wire.frag pairs with the same
	// VSs as the shaded legs, so wireframe line pixels skip the heavy lit
	// FS (Forward+ loop, GI, texture feedback). Missing module leaves
	// shaderCount 0 and wireframe keeps the shaded FS (raster override
	// only, the pre-swap behavior).
	smvk::Holder<smvk::ShaderModuleHandle> fragWireShader;
	smvk::ShaderProgram mainWireProgram;       // default.vert + wire FS
	smvk::ShaderProgram vertexLitWireProgram;  // default_vertexlit.vert + wire FS
	// Alpha-tested shadow-depth program for MASK cutout casters
	// (shadow.vert mainVS + shadow.frag mainPS). Opaque casters keep
	// the engine builder's depth-only program. shadow.frag declares
	// conservative depth for the atlas's normal-Z convention.
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedVertShader;
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedFragShader;
	smvk::ShaderProgram shadowMaskedProgram;
	// SDSM depth statistics (sdsm_stats.cs.hlsl mainCS): decimated
	// min/max view depth over the main render depth -> the async-read
	// stats buffers -> the cascade split fit (see the SDSM block).
	smvk::Holder<smvk::ShaderModuleHandle> sdsmStatsModule;
	smvk::ShaderProgram sdsmStatsProgram;
	// Reversed-Z conservative twin of shadow.frag (shadow_rev.frag):
	// the Forward+ prepass / temporal seed / remainder masked phases run
	// clear-0 GreaterEqual (reversed-Z), so their bound must promise
	// "never nearer" instead of the atlas's "never farther".
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedFragRevShader;
	// Alpha-tested depth-prepass program for MASK cutout
	// (default.vert mainVS + shadow_rev.frag mainPS): writes cutout-accurate
	// depth so background behind foliage holes survives the main pass.
	// Reuses the main VS and the reversed-Z masked-shadow FS module.
	smvk::ShaderProgram prepassMaskedProgram;
	// SSSR-style thin G-buffer programs (see gbuffer.frag.hlsl): default.vert
	// + the trivial solid/cutout G-buffer FS pair — the Forward+ depth
	// prepass's G-buffer writers (bound instead of the VS-only
	// depthProgram + prepassMaskedProgram when the G-buffer layer is
	// wanted). Empty when the shaders failed to load (the prepass keeps
	// the legacy VS-only path).
	smvk::Holder<smvk::ShaderModuleHandle> gbufferFragShader;
	smvk::Holder<smvk::ShaderModuleHandle> gbufferMaskedFragShader;
	smvk::ShaderProgram gbufferProgram;
	smvk::ShaderProgram gbufferMaskedProgram;
	// NOTE: shadow/bloom/tonemap/forwardplus programs + modules are owned by
	// their engine builders (CascadedShadowAtlas / BloomChain / Tonemapper /
	// ForwardPlus).
};

struct ViewportResources {
	smvk::CascadedShadowAtlas directionalShadows; // atlas + default shadow program + Ensure
	smvk::SpotShadowAtlas spotShadows; // perspective atlas for shadowed spots + Ensure
	smvk::EvsmAtlas evsmDir;  // EVSM moments + blurred output (directional CSM tiles)
	smvk::EvsmAtlas evsmSpot; // EVSM moments + blurred output (spot tiles)
	smvk::BloomChain bloom; // separated per-mip HDR chain + default down/up programs + Ensure
	smvk::Tonemapper tonemap; // default tonemap program + pass helper
	smvk::ForwardPlus forwardPlus; // tile buffers + cull/depth programs + Ensure
	smvk::DeferredShadows deferredShadows; // fixed-size ring buffers + 3 phase programs
	smvk::Particles particles; // GPU pool/free-list/emitter-ring buffers + emit/sim/draw programs
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::DepthTarget renderDepth; // scene depth at internal resolution + Ensure
	// Occluder-only depth for the HZB pyramid (Option A split): filled by
	// ForwardPlusRenderer::AppendOccluderPrepass with big solid occluders
	// at their crudest LOD. HZB + the GPU filter read this; Main, SSAO,
	// tile-cull and deferred shadows keep reading the full renderDepth.
	smvk::DepthTarget hizDepth;
	smvk::HdrTarget hdr; // HDR intermediate (image + view + bindless + logical) + Ensure
	smvk::MsaaTarget msaa; // transient 2x color+depth for Main, resolves to hdr
	smvk::MotionVectors motionVectors; // RG16F motion target + program + pass helper
	smvk::MotionBlur motionBlur; // blurred HDR output + velocity-gather program + pass
	smvk::DepthOfField dof; // DOF HDR output + CoC target + low/bokeh programs + passes
	smvk::ScreenSpaceReflections ssr; // SSR HDR composite output + program + pass
	smvk::VolumetricFog volumetric; // GI-probe raymarched fog HDR output + program + pass
	smvk::ExposureMeter exposure; // histogram meter + compute program (auto-exposure)
	smvk::Ssgi ssgi; // SSGI post chain: shared MAX-HZB + AO raw/blur/compose + Main's aoTerm MRT targets
	smvk::ScreenSpaceDiffuseGi ssgiDiffuse; // diffuse SSGI chain: half-res cosine trace + blur/temporal + the ambient-replacement compose (Engine/Rendergraph/ssgi_diffuse.h)
	smvk::UiTarget ui; // separated UI pass target (ui scale) + Ensure
	smvk::UiComposite uiComposite; // UI -> swapchain alpha composite program + pass
	smvk::Smaa smaa; // LDR target + edges/weights/history + LUTs + programs (see smaa.h)
	smvk::Dlss dlss; // HDR output (swap extent) + 1x1 exposure scalar (see dlss.h)
	bool msaaEnabled = true; // runtime toggle (effective only if device supports)
};

// Pure frame renderer: builds the rendergraph + records the frame (scene,
// post chain, its own settings/debug panel) for the current camera state.
// All CONTROL (input polling, keybinds, camera drive, UI mode, bench
// flight, host lifecycle) lives in WorldRendererElement — the element owns
// an instance of this class and drives it through the public API below.
// The element is a friend: its shortcuts flip render settings directly
// (the same knobs the panel's widgets touch) without a parallel
// toggle-method for every checkbox; the control FLOW (who decides, when)
// is still entirely the element's.
class WorldRenderer : public IRenderer {
	// The control layer (see WorldRendererElement.h): drives the camera,
	// polls the input, owns the host lifecycle. Friend scope = the
	// shortcuts + the decal autosplat trigger; everything else goes
	// through the public API.
	friend class WorldRendererElement;
public:
	const char* RendererName() const override { return "World"; }

	// ------------------------------------------------------------------
	// HOST: the App owns the window/context/swapchain (the frame-sync
	// owner) and provides services this renderer queries (bench budget,
	// present mode, acquire wait). Construction is cheap; Attach (run
	// time, after the host's Initialize — the ELEMENT forwards its
	// onAttach) resolves the host members and builds the framegraph;
	// Detach (host shutdown, reverse stack order) releases everything
	// while the context is alive.
	// ------------------------------------------------------------------
	explicit WorldRenderer(smvk::App& host);
	~WorldRenderer();
	smvk::App& Host() { return m_host; }

	// Lifecycle pair (the ELEMENT forwards its IAppElement hooks here):
	// the old Initialize/Shutdown verbatim.
	bool onAttach();
	void onDetach();
	// The host recreated its swapchain (the only sync-legal place):
	// rebuild the swap views + invalidate temporal histories.
	void OnSwapchainRecreated();
	// Frame clock advance (the element's onUpdate calls this BEFORE the
	// render): camera-time integration + the headless decal autosplat
	// trigger key on it.
	void Tick(float dt);
	// One frame: layer adopt (the host's per-frame rect; see
	// ClampedLayer) + stats + RenderFrame. The element's onRender calls
	// this with the host's ElementFrame.
	void RenderLayerInto(const ElementFrame& frame);

	// ------------------------------------------------------------------
	// LAYER (nvpro-style element): the renderer draws into a sub-rect of
	// the host's swapchain image. Sync (acquire/present/resize) stays
	// OUTSIDE — the host acquires fb, calls the element's onRender, then
	// presents. Default = full swap image.
	// ------------------------------------------------------------------
	void SetLayerRect(LayerRect rect) { m_layerRect = rect; }
	void SetLayerFull() { m_layerRect = LayerRect::Full(); }
	[[nodiscard]] LayerRect GetLayerRect() const { return m_layerRect; }
	// ------------------------------------------------------------------
	// INLINE OUTPUT (an embedded host's live view — the editor's
	// WYSIWYG). When active, the frame's final LDR leg (Tonemap / SMAA
	// resolve / UI composite) writes a renderer-PRIVATE image at a
	// HOST-DRIVEN extent instead of the host swapchain — the graph
	// never touches swap images at all. History: writing the host's
	// viewport RECT of its swap image directly reproducibly wedged
	// nvlddmkm past the TDR window (VK_ERROR_DEVICE_LOST, deterministic
	// 5/5; vanished the moment the final leg targeted a private image
	// instead — same command buffer, same single submit, only the swap
	// write removed). The host records this renderer's frame inside
	// its own (Execute appends into the host's command buffer), then
	// composites the private target into its viewport rect itself.
	//
	// The extent is the ACTIVE VIEWPORT's size: every render target
	// sizes off it and the lens aspects to it, so the view matches the
	// host's own framing for that rect (full writes at (0,0), no
	// non-zero render-area origins — see the driver pathology above).
	//
	// Refuses the async-compute split implicitly through the host: the
	// embedded host must keep the split off (its process sets
	// TOURMALINE_ASYNC_COMPUTE=0 — a mid-recording G1 submit would cut
	// the host's command buffer in two).
	// ------------------------------------------------------------------
	void SetInlineOutput(bool on) { m_inlineOutput = on; }
	[[nodiscard]] bool InlineOutputActive() const { return m_inlineOutput; }
	// The scene-lens FOV override (degrees). The embedded host drives
	// the camera externally; this aligns the renderer's lens with the
	// host's own camera so the framed view matches.
	void SetSceneFovY(float degrees) {
		m_sceneFovYDeg = degrees > 1.0f ? degrees : kCameraFovYDegrees;
	}
	// (Re)creates the private LDR target at the new extent (the host's
	// viewport drift — moves are free, resizes recreate via the
	// deferred-retire pattern; see EnsureInlineTarget). True when the
	// target is valid to render into. The target's format MIRRORS the
	// host swapchain's surface format so the host's 1:1 composite needs
	// no re-encode.
	bool EnsureInlineExtent(smvk::Extent2D extent);
	[[nodiscard]] smvk::Extent2D InlineTargetExtent() const {
		return m_inlineTargetExtent;
	}
	// The composited output for the HOST's own composite pass: the raw
	// physical view (import into the HOST's rendergraph) + the
	// context-global bindless index (sample through the global set —
	// bindless indices are context-wide, graph-agnostic) + the format.
	// View empty / index UINT32_MAX until the first EnsureInlineExtent.
	[[nodiscard]] smvk::ImageViewHandle InlineTargetView() const {
		return m_inlineTargetView;
	}
	[[nodiscard]] uint32_t InlineTargetBindlessIndex() const {
		return m_inlineTargetBindlessIndex;
	}
	// The raw image handle (debug readbacks: the host dumps the target's
	// bytes through the RMI's sync download).
	[[nodiscard]] smvk::ImageHandle InlineTargetImage() const {
		return m_inlineTargetImage;
	}
	[[nodiscard]] smvk::Format InlineTargetFormat() const;
	// The frame camera positioner (the element's camera drive moves
	// this; the frame build reads position/orientation off it). Named
	// CameraPositioner, not Camera: a Camera()-named member would shadow
	// the smvk::Camera TYPE at the m_camera member declaration below.
	CameraPositioner_FirstPerson& CameraPositioner() { return m_cameraPositioner; }
	// Pass-set config (the framegraph shape). Everything else about the
	// world arrives through the host's loader below.
	// The RunConfig overload applies the central TOURMALINE_* environment
	// (parsed once by main); the bare overload parses it locally for
	// hosts that have not migrated yet (editor). Prefer the two-arg form.
	void Configure(WorldRendererConfig passes);
	void Configure(WorldRendererConfig passes, const RunConfig& run);
	void ApplyRunConfig(const RunConfig& run);

	// WORLD SOURCE
	// The host is responsible for all content loading. The renderer knows
	// nothing about paks, scene files, or scene discovery.
	//
	// TWO ownership forms (the embedded host picks; the windowed App uses
	// the source):
	//  - OWNED (SetWorldSource): the source returns a LinearPtr the
	//    renderer keeps — the host TRANSFERS the world in. The App's
	//    form.
	//  - BORROWED (SetBorrowedWorld): the host KEEPS owning + mutating
	//    the world and lends the raw pointer — the editor's form: its
	//    editing systems all dereference their own world; edits show up
	//    per frame because it is the SAME object. The host must keep
	//    the world alive across every rendered frame (stop or relaunch
	//    the renderer before the world dies).
	using WorldSource = std::function<smvk::mem::LinearPtr<smvk::World>(
		smvk::ResourceManager& ctx, smvk::OverlayVfs& contentVfs)>;
	void SetWorldSource(WorldSource source) { m_worldSource = std::move(source); }
	// The borrowed form above (mutually exclusive with a set source; the
	// last one installed wins — an unset source + a null borrowed world
	// fails onAttach with an error, same as before).
	void SetBorrowedWorld(smvk::World* world) {
		m_borrowedWorld = world;
		m_worldSource = nullptr;
	}

	// Texture streamer for the world's v4 texture tails (see
	// Engine/Renderer/texture_streamer.h): null until Initialize creates
	// it (before the world source runs) and when streaming is disabled.
	// The host's world source reads it to register pak digest textures
	// for demand upgrades; pak blobs without digests stay full-resident
	// (the pre-v4 behavior).
	smvk::TextureStreamer* GetTextureStreamer() const {
		return m_textureStreamer.get();
	}
	// Active GI permutation variant based on loaded GI assets
	[[nodiscard]] GIVariant GetActiveGIVariant() const {
		// Priority: Volumes > Regular > None. The convex GI volumes
		// carry the baked ambient over the sky base (the legacy octree
		// underneath is RETIRED — deleted with the .sggi runtime; the
		// GI_VOLUMES shader handles the uncovered fade via the sky).
		if (m_giVolumesLoaded && m_giVolumesEnabled && ProbesWanted()) {
			return GIVariant::Volumes;
		}
		if (m_regLoaded && m_regEnabled && ProbesWanted()) {
			return GIVariant::Regular;
		}
		return GIVariant::None;
	}
	// Selects the direct-path lit program for a material.
	//
	// Uses the active GI variant's program, falling back to the base program.
	// For non-layered materials (layerMaterials.w <= 1), substitutes the
	// standard-material twin when available. Layered materials or missing
	// twins retain the original monolithic program as a safe fallback.
	//
	// Mirrors the filtered path's GfxRenderer::FilteredProgramGi and
	// GfxRenderer::FilteredLitProgram selection logic.
	[[nodiscard]] smvk::ShaderProgram& MainLitProgramFor(uint32_t materialId);

	// One-shot capture readback (TOURMALINE_CAPTURE): the last rendered
	// frame's TonemapCapture wrote m_captureImage and the submit landed
	// — onUpdate polls this at the next frame's top (before the host's
	// acquire); it stalls (device idle) and exports the PNG only on the
	// pending frame.
	void PollCaptureReadback();
	// Direct debug-view selection (the F1..F6/F9..F11 keys, the Debug
	// tab's view combo, the editor): a direct selection TAKES OVER —
	// the cycled overlay views (the GI debug views R/F8, the DS
	// visualizers Y) render through the lit fragment shader and would
	// otherwise keep masking the freshly selected view (F8 ->
	// irradiance, then F1 still showed irradiance: m_debugView was Lit
	// but m_giDebug stayed 1). Clears the overlays; selecting Lit
	// through this path returns to the full PBR reference. The
	// env-parse pin (TOURMALINE_DEBUGVIEW) assigns the member raw —
	// startup, no overlays exist yet.
	void SetDebugView(ViewMode v) {
		m_debugView = v;
		m_giDebug = 0;
#if defined(DS_DEBUG_VIZ)
		m_dsDebugMode = DsDebugViz::Off;
#endif
	}
	// The GI debug views (R/F8) render through the LIT shader: callers
	// activating one (the cycle keybinds, the combo) snap the base view
	// to Lit or the overlay is invisible under a flat mode.
	void SetGiDebug(int d) { m_giDebug = d ? 1 : 0; }
	void SetAmbientMode(AmbientMode m) { m_ambientMode = m; }
	void SetAaMode(AaMode m) { m_aaMode = m; }
	void SetWireframe(bool v) { m_wireframe = v; }
	void PushVolumetricEnv(const SceneEnv& env) {
		const bool volTier = env.fogQuality == 2 && env.volumetrics > 0.0f;
		m_volumetricEnabled = volTier;
		if (volTier) {			m_volumetricIntensity = env.volumetrics;
			m_volumetricSteps = int(env.volumetricSteps);
			m_volumetricMaxDistance = env.volumetricMaxDistance;
			m_volumetricDensity = env.volumetricDensity;
			m_volumetricAnisotropy = env.volumetricAnisotropy;
			m_volumetricSun = env.volumetricSun;
			m_volumetricGi = env.volumetricGi;
			m_volumetricUseGi = env.volumetricUseGi;
			m_volumetricUseSun = env.volumetricUseSun;
			m_volumetricJitter = env.volumetricJitter;
			m_volumetricHeightFalloff = env.volumetricHeightFalloff;
			m_volumetricHeightBase = env.volumetricHeightBase;
			m_volumetricQuality = smvk::VolumetricQuality::Custom;
		}
	}
	// Headless volumetric pins (the Configure snapshot): the AGENTS.md
	// contract is "scene fog settings lose" — the scene seed above would
	// otherwise stomp a headless force-on at every scene load/rebind, so
	// the pins re-apply after the seed (and at startup Configure). The UI
	// panel stays live afterwards.
	void ApplyHeadlessVolumetricOverrides();
	// Postfx look seed from the RENDER SETTINGS (the split look — see
	// SceneRenderSettings). The env block still contributes its
	// "exposure" row (the editor VIEWPORT brightness multiplier — an
	// environment-authored value, folded as EV below); everything else
	// look-related reads the render settings. Runs once per scene
	// load/rebind; the panels/keys stay live afterwards (the volumetric
	// contract). Headless TOURMALINE_* pins win over the seed (see
	// ApplyHeadlessPostfxOverrides). LOOK ONLY by design: the quality/
	// renderer knobs (the GI/ambient source, the per-source opt-ins,
	// probe pre-mix, EVSM) are HOST-OWNED session state — the editor's
	// PushRenderQuality prefs, the App's panels/keys, the TOURMALINE_*
	// pins — and never scene content (see RenderQualityConfig).
	void PushPostfxEnv(const SceneRenderSettings& r, const SceneEnv& env) {
		m_bloomEnabled = r.bloomEnabled;
		m_bloomIntensity = std::clamp(r.bloomIntensity, 0.0f, 2.0f);
		m_bloomThreshold = std::max(r.bloomThreshold, 0.0f);
		m_tonemapMode = std::min(r.tonemapMode, 3u);
		m_autoExposureEnabled = r.autoExposure;
		m_manualExposure = std::clamp(r.manualExposure,
			0.00001f, 0.05f);
		// EV bias + the editor VIEWPORT exposure row folded together: the
		// env's "exposure" (0.1..4 slider, serialized at floats[10]) is a
		// plain brightness multiplier. Fold it as log2 (stops): 1.0 =
		// neutral (log2(1)=0 — every existing scene renders identically),
		// the slider range is ±2 EV inside the ±4 clamp.
		const float viewportEv = env.exposure > 0.0f
			? std::log2(env.exposure) : 0.0f;
		m_exposureEvBias = std::clamp(
			r.exposureEvBias + viewportEv, -4.0f, 4.0f);
		m_exposureSpeedToDark = std::max(r.exposureSpeedToDark, 0.01f);
		m_exposureSpeedToBright =
			std::max(r.exposureSpeedToBright, 0.01f);
		m_exposureMin = std::max(r.exposureMin, 0.000001f);
		m_exposureMax = std::min(std::max(r.exposureMax,
			m_exposureMin), 1.0f);
		m_aoMode = static_cast<AoMode>(std::clamp(r.aoMode, 0, 2));
		m_ssaoRadius = std::clamp(r.aoRadius, 0.05f, 2.0f);
		m_ssaoBias = std::clamp(r.aoBias, 0.0f, 0.1f);
		m_ssaoIntensity = std::clamp(r.aoIntensity, 0.0f, 2.0f);
		m_ssaoMaxDistance = std::clamp(r.aoMaxDistance, 2.0f, 100.0f);
		m_ssaoSamples = std::clamp(r.aoSsaoSamples, 1, 32);
		m_ssaoBlurSharpness = std::clamp(r.aoBlurSharpness, 0.0f, 32.0f);
		m_saoSamples = std::clamp(r.aoSaoSamples, 1, 32);
		m_saoSpiralTurns = std::clamp(r.aoSaoSpiralTurns, 1, 16);
		m_ssrEnabled = r.ssrEnabled;
		m_ssrIntensity = std::clamp(r.ssrIntensity, 0.0f, 4.0f);
		m_motionBlurEnabled = r.motionBlurEnabled;
		m_dofEnabled = r.dofEnabled;
		m_aaMode = static_cast<AaMode>(std::clamp(r.aaMode, 0, 3));
		m_cubemapEnabled = r.cubemapEnabled;
		m_cubemapIntensity = std::clamp(r.cubemapIntensity, 0.0f, 4.0f);
	}
	// RENDER QUALITY push (host-owned): the same members the App's
	// GI/shadow panels and the N key drive. The editor applies its
	// stored prefs after the WYSIWYG attach (AFTER the GI loaders —
	// they flip the per-source opt-ins ON for baked content, the
	// prefs win) and live per panel edit.
	void PushRenderQuality(const RenderQualityConfig& q) {
		m_ambientMode = static_cast<AmbientMode>(std::clamp(q.giMode, 0, 5));
		m_giIntensity = std::clamp(q.giIntensity, 0.0f, 2.0f);
		m_lmEnabled = q.lightmapsEnabled;
		m_regEnabled = q.regProbesEnabled;
		m_giVolumesEnabled = q.giVolumesEnabled;
		m_probePremixEnabled = q.probePremixEnabled;
		m_evsmEnabled = q.evsmEnabled;
		m_evsmSoftness = std::max(q.evsmSoftness, 0.0f);
	}
	// Headless postfx pins (the Configure snapshot): same "scene settings
	// lose" contract as the volumetric ones — called at startup Configure
	// AND after the per-scene PushPostfxEnv seed (scene load/rebind would
	// otherwise stomp a headless pin).
	void ApplyHeadlessPostfxOverrides();

	WorldRendererConfig& Passes() { return m_passes; }
	// Scene id (host-provided): names the GI bake files (gi/<id>.sggi/
	// .sglm/.regmanifest/.cvmanifest, superseded by the loaded world's
	// Gi bindings) and gates the sponza check-scene extras (preset
	// splats). The pak host passes the pak stem.
	void SetSceneId(const std::string& id) { m_sceneId = id; }
	// Project root: when the scene lives in a project, the project root
	// mounts into the content VFS below the bundled content/ (project
	// files win on collisions) so project-side cook/bake files
	// (gi/<stem>.*, cubemaps, …) resolve exactly where the cooker wrote
	// them. Empty = no project mount.
	void SetProjectRoot(const std::string& dir) { m_projectRoot = dir; }
	// --no-vfs: mute the loose VFS discovery mounts (MountContentVfs
	// skips project root + content/), leaving the pak backend mounted
	// by the host as the only scene-content source.
	void SetNoVfs(bool v) { m_noVfs = v; }
	// Present-mode request: the HOST owns the swapchain — this forwards
	// to it (the pump applies it at the next frame's rebuild check; see
	// App::SetPresentMode / RequestSwapchainRebuild).
	void SetPresentMode(smvk::PresentMode m) { m_host.SetPresentMode(m); }
	// (Bench freeze (--disable_control) + bench flight (--bench_move)
	// moved to WorldRendererElement: they gate the CAMERA DRIVE, which
	// is the element's. See SetDisableControl/SetBenchMoveSpeed there.)
	// Host-owned demo content (the APP host opts in): seeds the demo
	// particle emitters at init and the sponza preset decal splats at
	// scene load. Default OFF — the renderer never seeds content
	// on its own. Transitional: once decals/particles become world
	// entities this gate dies and the host seeds through the world
	// instead.
	void SetDemoContentEnabled(bool v) { m_demoContent = v; }
	// Bench determinism: fixed manual exposure (no eye adaptation), so
	// captures from different runs are directly comparable. main.cpp
	// forces this off for --bench; X toggles it back live.
	void SetAutoExposureEnabled(bool v) { m_autoExposureEnabled = v; }
	// Seed-proof variant for --bench (see m_autoExposurePin): applied in
	// ResolveSceneLights after the seed + headless overrides; the
	// plain setter above does not survive the seed. X still toggles live
	// afterwards.
	void SetAutoExposurePinned(bool v) { m_autoExposurePin = v ? 1 : 0; }
	// CLI override (main.cpp --no-dlss): force the DLSS mode (Off skips
	// the feature passes entirely). Pairs with
	// smvk::DlssSetStartupEnabled(false), which keeps the Streamline
	// interposer out of the Vulkan call chain for RenderDoc captures.
	void SetDlssMode(smvk::DlssMode mode) { m_dlssMode = mode; }
	// Cubemap debug view (same modes as the App panel combo + the
	// TOURMALINE_CUBEMAP_DEBUG headless switch): 0 off, 1 raw cubemap
	// (no Fresnel/roughness — which probe serves each pixel), 2 probe
	// blend weight as heat. The frame reads it through FrameUniforms.
	void SetCubemapDebug(int mode) {
		m_cubemapDebug = (mode < 0) ? 0 : ((mode > 2) ? 2 : mode);
	}
	// ---- Runtime renderer settings (the App's debug panel) ----
	// The App drives these through its debug panel. The m_passes
	// SHAPE stays untouched (Configure owns the pipeline shape) — these
	// are the runtime toggles every pass gates on.
	void SetBloomEnabled(bool v) { m_bloomEnabled = v; }
	bool BloomEnabled() const { return m_bloomEnabled; }
	void SetMotionBlurEnabled(bool v) { m_motionBlurEnabled = v; }
	bool MotionBlurEnabled() const { return m_motionBlurEnabled; }
	void SetDofEnabled(bool v) { m_dofEnabled = v; }
	bool DofEnabled() const { return m_dofEnabled; }
	void SetSsrEnabled(bool v) { m_ssrEnabled = v; }
	bool SsrEnabled() const { return m_ssrEnabled; }
	void SetVolumetricEnabled(bool v) { m_volumetricEnabled = v; }
	bool VolumetricEnabled() const { return m_volumetricEnabled; }
	// Zone-authored volumetric fog is live (the env-tile bake published
	// + any record carries ABSOLUTE volumetricDensity): the volOn
	// gates' THIRD density leg — the zone tables can raise extinction
	// from a zero pass density (the base record carries the scene
	// env's own volumetrics; node zones carry theirs), exactly like
	// the world-field leg beside it. Every volOn mirror (the pass
	// build, the fog degrade, the fog tile-list gate) and the shader's
	// early-out (FrameUniforms::envZoneFlags bit1) read the SAME scan
	// (WorldEnvTiles::AnyVolumetricFog) — never let one drift.
	[[nodiscard]] bool ZoneVolumetricFogLive() const {
		return m_envTilesPublished && m_envTiles.AnyVolumetricFog();
	}
	// World voxel fields (Engine/WorldFields): renderers/passes needing
	// CPU-side access (e.g. a wind consumer feeding physics) reach the
	// system here; GPU consumers go through FrameUniforms::worldFields +
	// world_field.hlsli instead.
	engine::worldfields::WorldFieldSystem& WorldFields() { return m_worldFields; }
	const engine::worldfields::WorldFieldSystem& WorldFields() const { return m_worldFields; }
	void SetSkydomeEnabled(bool v) { m_skyEnabled = v; }
	bool SkydomeEnabled() const { return m_skyEnabled; }

protected:
	// Host (the App this renderer rides on): the frame-sync owner
	// (context + swapchain) and the bench budget the [perf] split keys
	// on. Borrowed — the element (which owns this renderer) is added to
	// the host's stack and detached before teardown. NO Input here: the
	// element owns the input and drives the camera; this class never
	// polls it (the gamepad widgets below read m_inputUi, a UI-config
	// back-pointer the element installs).
	smvk::App& m_host;
	// Element clock (Tick latches the element's dt): camera
	// integration + exposure adaptation key on it.
	float m_deltaTime = 0.0f;
	float m_timeSinceStart = 0.0f;
	// Borrowed graphics members (onAttach resolves them off the host —
	// they exist by then). The rendergraph + world below are the
	// ELEMENT's own (its passes record into its own graph; the world
	// arrives through the host's WorldSource).
	smvk::ResourceManager* m_context = nullptr;
	smvk::Swapchain* m_swapchain = nullptr;
	smvk::mem::LinearPtr<smvk::RenderGraph> m_rendergraph;
	// The WORLD the frame reads (owned OR borrowed — see the WORLD
	// SOURCE contract): owned = the moved-in source result (below),
	// borrowed = the host's live pointer (kept alive by the host).
	smvk::World* m_world = nullptr;
	smvk::mem::LinearPtr<smvk::World> m_worldOwned; // the source-moved form
	smvk::World* m_borrowedWorld = nullptr;        // the embedded-host form

	// (Camera drive + UI-mode switching moved to WorldRendererElement:
	// the element polls the input, accumulates the virtual cursor and
	// moves Camera() — see UpdateCamera/SetUIMode there.)
	// Keeps the projection aspect in sync with the rendered (internal)
	// extent. Must run whenever the swapchain extent or render scale
	// changes (init + resize); otherwise the image stretches and
	// projection-derived data (CSM splits/frusta, selection) uses a stale
	// aspect. Takes the internal extent (not the swapchain): the scene
	// rasterizes at renderExtent, and LOD pixel budgets, the GPU filter
	// dimensions and motion-vector UVs all derive from it, so the aspect
	// must describe the same image (differs from the swapchain ratio by
	// rounding once renderScale != 1).
	// NOTE: Camera::setPerspective forwards to MakeReversedZProjRH, whose
	// fovY is in radians and whose depth maps [near, far] -> [1, 0]
	// (reversed-Z: near maps to depth 1, far to 0, like the old infinite
	// form, whose limit it recovers as far -> infinity).
	void UpdateCameraAspect(smvk::Extent2D extent) {
		if (extent.width == 0 || extent.height == 0)
			return;
		// The scene's default-camera lens (m_sceneFovYDeg/
		// m_sceneNearClip/m_sceneFarClip, set by the default-camera
		// framing at load).
		m_camera.setPerspective(
			glm::radians(m_sceneFovYDeg),
			float(extent.width) / float(extent.height),
			m_sceneNearClip,
			kMainViewFarPlane
		);
	}
	// (m_uiMode/m_virtualMouse/m_disableControl/m_benchMoveSpeed moved to
	// WorldRendererElement — pure control state. The panel footer + gamepad
	// widgets below read the hint/input back-pointers the element pushes.)
	// The element's UI-mode hint (FPS vs UI): panel footer text only.
	bool m_uiModeHint = false;
	// Input back-pointer for the panel's gamepad CONFIG widgets (axis
	// readouts + deadzone sliders) — the renderer never polls input for
	// control; the element installs this at attach.
	Input* m_inputUi = nullptr;
	// Installed extra-UI hook (see SetExtraUi): app-owned overlay draw,
	// invoked inside the ImGui frame build.
	std::function<void()> m_extraUi;
public:
	void SetUiModeHint(bool ui) { m_uiModeHint = ui; }
	void SetInputForUi(Input* in) { m_inputUi = in; }
	// Host-installed extra UI hook (app-owned overlays): drawn inside
	// the frame's ImGui build, after the debug panel + floating tools,
	// right before the UI pass records. Lets app hosts draw through the
	// renderer's ImGui pass (the sponza invaders HUD) without a
	// renderer subclass. No-op unless installed; runs only when the UI
	// pass runs (m_passes.ui + a live overlay).
	void SetExtraUi(std::function<void()> fn) { m_extraUi = std::move(fn); }
protected:
	// Configurable resolution knobs (see "Resolution config" in the .cpp).
	float m_renderScale = 1.0f;
	float m_uiScale = 1.0f;
	float m_uiDpiScale = 1.0f; // synced from ImGuiOverlay at init (auto = monitor DPI)
	// Frame timing: CPU wall time vs GPU execution time (EMA-smoothed ms
	// for the debug overlay). GPU lags 1-2 frames (retired-frame readback).
	double m_cpuMsSmooth = 0.0;
	double m_gpuMsSmooth = 0.0;
	bool m_gpuTimingValid = false;
	// GPU stage timings (shadow-path instrumentation, both PCF + EVSM —
	// see GpuStageTimer): last retired frame's per-stage ms + validity,
	// EMA-smoothed copies for the UI overlay, and --bench accumulators
	// (avg per measured frame, printed with the [perf] split).
	double m_stageGpuMs[smvk::kGpuStageCount] = {};
	bool m_stageGpuValid[smvk::kGpuStageCount] = {};
	double m_stageGpuMsSmooth[smvk::kGpuStageCount] = {};
	bool m_stageGpuTimingsSupported = false;
	double s_benchStageMs[smvk::kGpuStageCount] = {};
	uint64_t s_benchStageFrames = 0;
	// Acquire wait (in-flight fence + vkAcquireNextImageKHR) plus the
	// vkQueuePresentKHR throttle (vsync-paced under FIFO): idle time,
	// reported separately so CPU stays busy-only.
	double m_waitMsSmooth = 0.0;
	// ---- GPU memory stats (VMA budgets, refreshed at most 1 Hz, UI only) ----
	std::vector<smvk::GpuMemoryHeap> m_gpuMemHeaps{};
	double m_gpuMemLastQuerySec = -1e9; // throttle: max 1 Hz
	double m_gpuMemLastLogSec = -1e9; // console: max ~1 per 10 s (when enabled)
	double m_gpuMemQueryUs = 0.0; // last query wall time (µs, shown in UI)
	bool m_gpuMemShowAllHeaps = false; // collapsed by default: VRAM heap only
	bool m_gpuMemConsoleAutoLog = false; // periodic stderr snapshot; button always works
	void RefreshGpuMemoryStats(bool force = false); // throttled query
	void LogGpuMemoryToConsole(); // fprintf(stderr) one snapshot
	// Text-only stats HUD (top-left overlay): framerate, GPU, camera,
	// filter culls, LODs, exposure, streaming. No-op unless m_showHud.
	// Reads the same members as the panel rows it replaces (see the
	// Stats tab + panel header); the panel keeps controls, explanations
	// and detail breakdowns.
	void DrawHudOverlay();
	// Debug-view hint (auto-visible while a debug visualization is
	// active): a small top-center textbox naming the active view and its
	// COLOR CODING — the compact replacement for the per-view
	// explanation blocks the old Debug tab carried (the legends live
	// where the view is, not four tabs away). NoInputs: pure annotation.
	void DrawDebugViewHint();

	// (The per-frame CONTROL block — Update()/HandleShortcuts(): keybind
	// polling, feature toggles, decal autosplat, camera-drive gating,
	// bench flight — moved to WorldRendererElement (its onUpdate). This
	// class is the pure frame builder; see WorldRendererElement.h.)

public:
	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent, smvk::Extent2D swapExtent, const smvk::EnvironmentSettings& env);
	smvk::GpuDirectionalLightDescription UpdateLights(uint32_t currentFrame, const smvk::EnvironmentSettings& env);
	void UpdateMaterials(uint32_t currentFrame);
	// RHI texture POOL index -> bindless descriptor slot for the world's
	// cluster textures (see UpdateMaterials: the pool==bindless identity
	// only holds by creation-order luck, and live worlds churn the\r\n\t// bindless pool across re-imports). Rebuilt
	// per call (cheap: one pass over the clusters' registered textures)
	// so live editor texture edits are picked up without a rebind.
	std::unordered_map<uint32_t, uint32_t> BuildPoolToBindlessMap() const;

	// Procedural Forward+ demo lights (no scene nodes; see UpdateForwardPlusLights).
	uint32_t UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame);

	// ---- Forward+ decals (tiled projected splats; world entities, see
	// SceneDecal + scene.decalForNode + smvk::ForwardPlusDecal) ----
	// Single gate for every consumer (FrameUniforms decalCount/BDAs, the
	// decal leg of the forward+ tile cull, Main read hints) so the shader
	// flag and the GPU lists can never disagree. Requires the forward+
	// tile buffers: the cull dispatch refreshes the decal lists (storing
	// 0 per tile with no decals), so decals ride the same pass as lights —
	// including when the deferred-shadow pipeline owns the LIGHT lists
	// (lightCount=0 cull; decals keep working). The world carries the
	// CONTENT (decal entities in cluster pools); this is only visibility
	// + machinery.
	bool DecalsActive() const {
		return m_decalsEnabled && m_passes.forwardPlusCull
			&& m_viewportResources.forwardPlus.Valid()
			&& !m_resources.decalsBuffer.empty()
			&& m_world && !m_world->decalLocations.empty();
	}
	// Re-derives the GPU decal list from the WORLD's decal entities
	// (decalForNode -> decalLocations -> Cluster decals pool; textures
	// resolve through the cluster pools like light cookies) and uploads
	// the flight-sliced ring; returns the live count. Runs before
	// UpdateFrameUniforms (which reads the count via m_decalFrameCount).
	// Per-frame re-derivation: dynamically-created decals (app presets,
	// J spawns) appear the same frame without any refresh call.
	uint32_t UpdateDecals(uint32_t currentFrame);
	// World voxel fields (Engine/WorldFields): registers the frame's
	// field set ONCE (onAttach, after the world resolves — field ids are
	// registration order, fog first per WORLD_FIELD_ID_* in
	// world_field.hlsli). v1 registers the TOURMALINE_WORLD_FIELDS=1
	// checkerboard shakedown field; fog/wind register here as their
	// consumers land. The field set closes at the first GPU publish.
	void InitWorldFields();
	// J: lazily builds the CPU pick BVH (scene triangle soup, see
	// smvk::gi::BuildTraceSceneFromCluster) and raycasts the camera
	// center ray; on hit spawns a blood-splat ENTITY (World::
	// CreateDecalAt into the dynamic cluster).
	void SpawnDecalAtCrosshair();
	// One-shot particle burst at a world position (the Asteroids game
	// host's death/hit pops): `count` particles spawn over a short fixed
	// window as a TRANSIENT emitter row (sphere volume, outward
	// velocity, gravity + drag, additive emissive tint), then the row
	// removes itself. Framerate-independent — the rate spreads the count
	// over the window and the GPU's fractional carry lands the remainder
	// exactly. Bursts stack with the demo emitters up to the emitter
	// cap; overflow drops the oldest burst. No-op when the particle pass
	// cannot run (ParticlesActive gates the upload; queued bursts age
	// out on the next active frame).
	void SpawnParticleBurst(const glm::vec3& pos, const glm::vec3& color,
		float speed, float size, float life, uint32_t count);
	// Lazy CPU pick scene (same triangle soup the GI baker sees): shared
	// by crosshair picking and the probe-overlay occlusion cull. Builds
	// once (one-time glTF re-import hitch, noted on console); true when
	// the BVH is ready for raycasts.
	bool EnsurePickScene();
	// Headless J (TOURMALINE_DECAL_AUTOSPLAT=<frame>): auto-runs the
	// crosshair spawn at the given frame so capture/A-B runs verify the
	// pick path without window focus. -1 = off.
	int m_decalAutoSplatAtFrame = -1;
	// Decal visibility (UI checkbox + TOURMALINE_DECALS env): a render
	// toggle over whatever entities the world carries, never content.
	bool m_decalsEnabled = true;
	// Unresolved decal texture URIs already warned about (warn-once-until-
	// resolved, so a dangling ref doesn't spam per frame).
	std::unordered_set<std::string> m_decalTexWarnedUris;

	// ---- Deferred shadows (Square-Enix-style tiled/zBin pipeline, see
	// smvk::DeferredShadows) ----
	// Single gate for every consumer (FrameUniforms dsEnabled, phase-1
	// uploads, phases 2-4 dispatch, Main read hints) so they can never
	// disagree and feed the lighting pass stale buffer contents.
	bool DsActive() const {
		return m_passes.deferredShadows && m_dsEnabled
			&& m_viewportResources.deferredShadows.Valid()
			&& m_viewportResources.deferredShadows.ShadersValid();
	}
	// Phases 2-4 compute passes (called after the Forward+ tile cull
	// spot, i.e. over complete depth).
	void AppendDeferredShadowPhases(
		uint32_t frameIndex,
		uint64_t frameUniformsAddress, 
		uint64_t fpLightsAddress,
		uint64_t spotShadowBufferAddress, 
		uint32_t fpLightCount,
		smvk::LogicalImageViewHandle depthView,
		smvk::LogicalBufferHandle fpLightsLogical
	);

	void RebuildSwapViews();
	// By reference: RenderGraph::Execute publishes the async-split second
	// graphics part (G2 CB + compute wait) back through fb for
	// EndAndPresent. By value would drop it and G1's CB would be ended +
	// submitted twice (GPU hang on lights-off, when the split engages).
	void RenderFrame(smvk::Framebuffer& fb);

	// Error-driven LOD selection (see Engine/Asset/mesh_optimizer.h): the
	// scene references LOD0; cooked "/lodN" siblings are picked per instance
	// from their world-space simplifier error projected to pixels at the
	// instance distance. Returns the LOD mesh (LOD0 fallback when disabled
	// or uncooked). `outLod` receives the selected index for stats.
	smvk::StaticMesh* LodMeshForNode(NodeId node, uint32_t worldMesh,
		uint32_t* outLod = nullptr);
	// Cheap LOD index for filter/direct split decisions (cached chains, no
	// mesh fetch). Returns 0 when LODs are disabled or uncooked.
	uint32_t LodIndexForNode(NodeId node, uint32_t worldMesh) const;

	// ---- Transparent list (AlphaMode::Blend + dithered) ---------------
	// Shared by Main's transparent leg and the SSR  redraw so
	// both draw the same geometry against the same cull view: items are
	// Renderable|Transparent|StaticVertexFmt, frustum-tested against the
	// frozen transparent cull view (conservative LOD union, matching
	// FilterMainView). sortBackToFront orders by distance-to-camera for
	// the classic AlphaBlend leg; the pane pass passes false (its Max
	// blend is order-independent).
	struct TransparentDrawItem {
		NodeId node;
		uint32_t meshId;
		uint32_t materialId;
		float dist2;
	};
	std::vector<TransparentDrawItem> CollectTransparentDraws(
		const smvk::RenderView& cullView,
		bool sortBackToFront,
		std::optional<glm::vec3> sortCamPos = std::nullopt
	);

	void ResetLodStats();
	// ---- Shader LODs (per-level shading programs) -------------------
	// Runtime LOD switch: pixel budget for the projected simplifier error
	// (~1px = conservative, higher = more aggressive). Disabled = always LOD0.
	// Per-node decisions are cached in the World and refreshed EVERY frame
	// (full rate, the default; halfrate parity opt-out — see World::
	// LodIndexForNode and m_lodFullRate); m_lodFrameInputs.frameId is the
	// monotonic key. Selection is hysteretic around the budget
	// (kLodSelectHysteresis), so switch distances don't flip siblings
	// frame to frame while the camera moves.
	bool m_lodEnabled = true;
	smvk::LodFrameInputs m_lodFrameInputs{};
	float m_lodPixelBudget = 1.0f;      // mirrors m_lodFrameInputs.pixelBudget
	float m_lodViewportHeight = 1080.0f; // mirrors m_lodFrameInputs.viewportHeight
	uint64_t m_lodFrameId = 0;         // monotonic per-frame counter
	float m_lodBudgetApplied = -1.0f;  // budget at last InvalidateLodCache
	// LOD refresh cadence: true (default) = every node recomputes every
	// frame; false = the World's halfrate parity split (perf A/B; also
	// TOURMALINE_LODFULL=0 at startup).
	bool m_lodFullRate = true;
	// Debug: TOURMALINE_SPLIT=0 forces the legacy depth assembly (full
	// prepass before the HZB + remainder pass) even when the seed primed
	// depth bisects regressions between the split and the rest.
	bool m_splitDisabled = false;
	// Debug: one-shot [GfxAudit] dump at this frame (TOURMALINE_AUDIT=N;
	// same stderr dump as the UI button; stalls the GPU).
	int m_auditAtFrame = -1;
	// (Per-frame LOD draws/switched counters removed: they only fired on
	// the DIRECT legs (LodMeshForNode), reading 0/0/0 with the gfx filter
	// on - the default. The live numbers are World-level per-frame decision
	// stats now (World::LodStatDraws/LodStatSwitched - every leg's first
	// query counts once; see World::LodIndexForNode).)
	uint32_t m_lodChainMeshes = 0; // world meshes with a cooked chain (per frame)
	uint32_t m_lodChainLods = 0;   // total cooked siblings (per frame)
	// The farthest level of a cooked chain shades with the vertex-lit
	// program instead of full PBR when the material allows it (same
	// coarsest geometry — see World::UseVertexLitShaderAt). Toggle =
	// pure fallback: off/A/B (UI checkbox or TOURMALINE_SHADERLOD=0)
	// keeps every level full-lit.
	bool m_lodShaderEnabled = false;
	uint64_t m_lodShaderDraws = 0; // Main far-shading draws this frame (vertex-lit + specialized)
	// Effective decision for an instance's selected level + material
	// (see World::FarShadingAt): vertex-lit substitution, specialized
	// lit, or default full-lit. Shares the halfrate LOD cache with every
	// geometry consumer, so shading and geometry always resolve off the
	// SAME level within a frame. Full-lit when LODs/shader-LODs are
	// disabled, uncooked, or the program is missing.
	smvk::World::FarShading FarShadingForNode(NodeId node, uint32_t worldMesh,
		uint32_t material) const;
	// Far-level specialized lit twins (see ShaderLodQuality):
	// mainProgram / mainProgramA2C modules + the material's quality spec
	// payload on the fragment stage. Keyed by quality bits (specular?1:0
	// | normalMap?2:0); the default quality never queries (the plain
	// twins bind instead), so entries are exactly the non-default combos
	// in use. Module handles are shared (no duplication); pipelines key
	// on the spec payload, so each combo mints its own PSOs on first
	// bind. The m_resources twins are init-once, so cached entries never
	// go stale within a run.
	struct FarSpecPrograms { smvk::ShaderProgram main{}; smvk::ShaderProgram a2c{}; };
	std::unordered_map<uint32_t, FarSpecPrograms> m_farSpecPrograms;
	// Specialized pair for a far-level quality (builds + caches on miss).
	// Programs are empty when the base twins are missing — the caller
	// falls back to the plain twins then. Never null (unordered_map
	// nodes stay put, so the pointer survives later inserts).
	FarSpecPrograms* FarSpecProgramsFor(const smvk::ShaderLodQuality& quality);
	// True when prog is one of the cached far-level specialized twins
	// (for the wireframe twin swap — they shade through the lit FS
	// family like mainProgram, so they take the same flat twin).
	bool IsFarSpecProgram(const smvk::ShaderProgram& prog) const {
		for (const auto& [key, pair] : m_farSpecPrograms)
			if (&pair.main == &prog || &pair.a2c == &prog)
				return true;
		return false;
	}
	bool ShaderLodActive() const {
		return m_lodShaderEnabled && m_resources.vertexLitProgram.shaderCount == 2;
	}

	static constexpr float default_near_clip = 0.01f;
	static constexpr float kCameraFovYDegrees = 45.0f;
	// Finite far plane for the MAIN VIEW projection (reversed-Z). The scene
	// far clip (m_sceneFarClip, fitted to the framed bounds at load) stays
	// per-scene for shadows/LOD; only the projection is finite, so
	// unprojecting far/sky depth yields finite
	// positions and the frustum far plane is functional.
	static constexpr float kMainViewFarPlane = 100000.0f;

	WorldRendererConfig m_passes{};
	// Host world loader (see SetWorldSource): called once during
	// onAttach (after the context exists).
	WorldSource m_worldSource;
	// Scene id: names the GI bake files (gi/<id>.sggi/.sglm/.regmanifest/
	// .cvmanifest, superseded by the loaded world's Gi bindings) and
	// gates the sponza check-scene extras (preset splats).
	std::string m_sceneId;
	// Scene far clip (shadow splits, LOD distances, DLSS depth scale),
	// fitted to the framed bounds at load (generic framing parks the
	// camera radius*2.2 away unless TOURMALINE_CAM_POS/FWD pin it).
	float m_sceneFarClip = 2000.0f;
	// Scene lens (default-camera framing only — see the World settings'
	// camera.default typed field): when the scene carries an authored
	// DEFAULT CAMERA, its perspective lens replaces the App defaults in
	// UpdateCameraAspect (generic framing leaves these untouched).
	float m_sceneFovYDeg = kCameraFovYDegrees;
	float m_sceneNearClip = default_near_clip;
	// Project root for the content VFS (see SetProjectRoot).
	std::string m_projectRoot;
	// --no-vfs (host opt-in, see SetNoVfs): mute the loose VFS discovery
	// mounts so the pak backend is the only scene-content source.
	bool m_noVfs = false;
	// (Camera-control freeze + bench-flight speed moved to
	// WorldRendererElement with the camera drive.)
	// Host demo-content opt-in (see SetDemoContentEnabled). Default OFF.
	bool m_demoContent = false;

	smvk::mem::LinearPtr<smvk::ImGuiOverlay> m_imgui{};
	AppResources m_resources{};
	ViewportResources m_viewportResources{};
	// Resolved KHR_lights_punctual set for the loaded scene (snapshotted
	// once in ResolveSceneLights after Resolve; refreshed silently by
	// RefreshSceneLights on editor edits — see world_renderer.cpp).
	// If the scene carries ANY lights, they render — no enable toggle.
	ResolvedSceneLights m_sceneLights{};
	void ResolveSceneLights();
	// Light resolve shared by the bind-time path (log + env-sun seeding)
	// and silent per-edit refreshes (pack only - no env writes, no world
	// mutation, no log). Reads the LIVE world, so edits apply.
	// seedEnvSun: write the elected sun into a sceneless env (bind time
	// only - the edit path must not stomp user retouches).
	void RefreshSceneLights(bool log, bool seedEnvSun);
	// Unresolved cookie URIs already warned about (warn-once-until-
	// resolved, so a dangling cookie doesn't spam per refresh).
	std::unordered_set<std::string> m_cookieWarnedUris;
	bool HasSceneLights() const {
		return m_sceneLights.hasLights();
	}
	// Spot-shadow selection for the resolved spot lights: per-light opt-in
	// comes from the authored cast-shadow flag; the frame selection below
	// ranks requesters by the screenspace/distance heuristic and uploads
	// the top-N atlas tiles (upper limit + hard camera-distance cutoff).
	bool m_spotShadowsEnabled = true;
	int m_spotShadowMax = 8;               // upper limit, clamped to kMaxSpotShadows (16)
	float m_spotShadowMaxDistance = 45.0f; // hard cutoff on camera->light distance (m)
	// Point-shadow selection (same shape, six tiles per light): the
	// per-light opt-in is the same authored cast-shadow flag; each selected
	// point claims 6 consecutive atlas tiles (cube faces) + 6 depth passes,
	// so the budget stays at kMaxPointShadowLights.
	bool m_pointShadowsEnabled = true;
	int m_pointShadowMax = 1;               // shadowed points, clamped to kMaxPointShadowLights
	float m_pointShadowMaxDistance = 45.0f; // hard cutoff on camera->light distance (m)
	bool m_showLightPoints = false;
	bool m_showLightNumbers = false;
	float m_lightPointSize = 6.0f; // ImGui circle radius (px)
	int m_maxLightLabels = 256;    // cap text draws (points are all drawn)

	// ---- Forward+ decals (world entities; see the decl block above) ----
	uint32_t m_decalFrameCount = 0; // live decal count (set by UpdateDecals,
	                                // read by UpdateFrameUniforms)
	uint32_t m_decalSpawnSeed = 1;  // splat yaw RNG stream (Pcg32)
	// Lazy crosshair-pick acceleration (J): full scene triangle soup +
	// CPU BVH, built on first use from the resolved world clusters.
	// 0 = not built, 1 = ready, -1 = unavailable (cooked worlds carry no
	// CPU triangle soup — consumers fail open).
	int m_decalPickState = 0;
	std::unique_ptr<smvk::gi::GiTraceScene> m_decalPickScene;
	// ---- GPU particles (compute emitters, see ParticleDemo + smvk::Particles) ----
	// Single gate for every consumer (Ensure, emitter ring upload, emit/sim
	// dispatches, draw pass) so the flag and the GPU state can never
	// disagree. Requires the Forward+ light pipeline only for the TILED
	// lighting path — the draw shader falls back to a brute-force light
	// loop when the tile lists are not fresh (deferred shadows own them).
	bool m_particlesEnabled = true;
	ParticleDemo m_particleDemo{};
	// Transient one-shot burst emitters (SpawnParticleBurst): full rows
	// with a TTL. UpdateParticles appends them after the demo rows,
	// ages the TTL (drops expired) and trims overflow (oldest first) so
	// demo + bursts never exceed PARTICLE_MAX_EMITTERS.
	struct ParticleBurst {
		smvk::ParticleEmitterGpu row;
		float ttl = 0.0f;
	};
	std::vector<ParticleBurst> m_particleBursts;
	uint32_t m_particleBurstSeed = 1; // per-burst RNG stream salt
	bool ParticlesActive() const {
		return m_passes.particles && m_particlesEnabled
			&& m_viewportResources.particles.Valid()
			&& m_viewportResources.particles.ShadersValid();
	}
	// Re-uploads the emitter parameter ring slice (every frame; carry lives
	// GPU-side) and returns the live emitter count.
	uint32_t UpdateParticles(uint32_t currentFrame);
	// Demo default emitters (embers + drifting motes at the scene origin).
	void SeedParticleEmitters();

	// ---- Forward+ light crowd (the light-spawner stress tool) ----
	// Procedural point lights appended to the frame's light list in
	// UpdateForwardPlusLights (after the scene lights, before the probe
	// tail): no scene nodes, no editor entities - the GPU-side twin of
	// the particle demo. Purpose: STRESS the Forward+ tile lists (a
	// dense crowd in a small disc saturates every tile's light budget
	// and the per-tile cap; the fallback brute-force loop lights the
	// rest). Deterministic: a fixed golden-angle spiral seeds the
	// placement, and the optional animation advances with the frame time
	// (fixed dt headless - byte-identical captures, like the particles).
	// TOURMALINE_LIGHT_CROWD=<n> pins the crowd headless for benches.
	struct LightDemo {
		bool enabled = false;      // off by default (stress tool, not content)
		int count = 32;            // lights in the crowd (capped on upload)
		float radius = 10.0f;      // spawn spiral radius around `center`
		float height = 2.5f;       // base height above `center`
		float bob = 1.2f;          // animated height oscillation (m)
		// Candela per light. Physically-scaled: the default sun is 10000
		// lux, so a tens-of-cd bulb is invisible outdoors (the original 24
		// read as "the crowd does not spawn"). 20k cd reads clearly in
		// sunlit scenes and is still sane in a night scene.
		float intensity = 20000.0f;
		float range = 6.0f;        // per-light cutoff radius (m)
		float speed = 0.35f;       // orbit rad/s (0 = frozen crowd)
		float hueSpread = 0.55f;   // per-light hue spread around baseHue
		float baseHue = 0.08f;     // base hue (0.08 = warm ember)
		glm::vec3 center{ 0.0f, 0.0f, 0.0f }; // crowd center (world)
		// Crowd shadow participation: when on, the first `shadowCount`
		// crowd lights (strided across the spiral for even spatial spread)
		// carry the kForwardPlusFlag_CastShadow request so the SAME
		// point-shadow selection + six-cube-face-tile atlas picks them up
		// like authored point lights. The cap keeps a 96-light crowd from
		// claiming the whole point atlas budget (the global m_pointShadowMax
		// still tops the selection; a flagged crowd light costs SIX
		// cube-face tiles + six depth passes per frame). Requires the Point
		// shadows toggle in Spot Shadows (the atlas gate).
		bool castShadows = false;  // off by default (stress tool, not content)
		int shadowCount = 2;       // crowd lights requesting shadows (stride-spread)
	};
	LightDemo m_lightDemo{};
	// Crowd lights actually appended this frame (spawned-report readout;
	// the count caps at FP_MAX_BUILD_LIGHTS minus the scene lights).
	uint32_t m_lightCrowdCount = 0;
	// The live Forward+ point/spot set the frame built (the scene's
	// lights; the spawn helper appends to it directly - one list, one
	// count, no separate light category).
	uint32_t m_fpLightCount = 0;
	smvk::CubemapLibrary m_cubemaps{}; // reflection cubemaps for IBL
	// ---- Reflection cubemap UI + cached probe rows ----
	// Probe data rides the Forward+ light buffer as type-2 entries
	// (smvk::kForwardPlusProbe, appended post-selection in
	// UpdateForwardPlusLights): no probe storage buffer, no parallel tile
	// lists. m_cubemapProbes is the upload source (widest-first order, so
	// tile overflow keeps the widest entries); the overlay reads it too.
	bool m_cubemapEnabled = true;   // specular IBL on/off (UI)
	float m_cubemapIntensity = 1.0f;// specular IBL scale (UI, FrameUniforms)
	float m_cubemapLodBias = 0.0f;  // added to roughness LOD (UI, FrameUniforms)
	int m_cubemapDebug = 0;         // 0 off, 1 cubemap-only, 2 probe weight (UI)
	bool m_cubemapShowProbes = false; // overlay: probe positions + influence rings (UI)
	// Chrome-ball visualization of the LOADED cubemap probes (see
	// Rendergraph/probeball.h): one lit sphere per baked capture, sampling
	// its own cubemap - a renderer-owned debug pass (zero scene footprint),
	// toggled from the App's panel (off by default: the clean look).
	bool m_cubemapShowBalls = false;
	smvk::probeball::ProbeBallVisual m_probeBalls{};

	// ---- GPU probe pre-mix (docs/probe_premix_plan.md) ----------------
	// The Remember Me architecture, GPU-only selection: the weights pass
	// reads the RESIDENT probe tail (the Forward+ light buffer - the GPU
	// system we already have; NO CPU classification anywhere) and blends
	// the top-K captures into ONE cubemap at the point of interest; the
	// material's single-fetch branch samples the blend with the dominant
	// probe's box parallax (read straight from the GPU-owned selection
	// buffer via the FrameUniforms BDA). The per-pixel walk stays as the
	// A/B comparison path (TOURMALINE_PROBE_PREMIX=0|1; the material
	// falls back to it whenever the pre-mix is off or the selection is
	// empty - the fallback is the zero-regression contract).
	bool m_probePremixEnabled = true;
	smvk::probepremix::ProbePremix m_probePremix{};
	// ---- JHFAA-style shading-normal validity correction ----------------
	// (shading_normal_fix.hlsli + docs/jhfaa_normal_fix.md; the
	// TOURMALINE_NORMAL_FIX / TOURMALINE_NORMAL_FIX_DEBUG pins). The
	// correction constrains the final shading normal (and therefore every
	// reflection vector derived from it) to physically valid directions
	// at silhouette pixels — the frame flags are the only consumer state.
	// Experimental: OFF by default while the A/B evaluation runs; the
	// pins exist so headless captures can compare the baseline and the
	// corrected image under identical conditions.
	bool m_normalFixEnabled = false;
	bool m_normalFixDebug = false;
	// GPU SKINNING (the Northlight compute box; Rendergraph/skinning.h):
	// the compute shader + the per-flight palette buffers. The driver
	// (the skinning block near the probe pre-mix append) runs per frame
	// over the world's skinned clusters.
	smvk::skinning::SkinVisual m_skinning{};
	// The deformed heap's PARITY bookkeeping (the double-buffered
	// deformed heap - see Cluster::EnsureSkinnedGpuData): the flip picks
	// the half the skinning pass writes (every render fetch follows the
	// cur BDA), and the last-run heap identity gates the PREV half's
	// validity for the motion vector leg (consecutive runs on the same
	// heap - a rebuild or a swap leaves the prev stale and the motion
	// leg falls back to zero mesh motion).
	uint32_t m_skinHeapParity = 0;
	uint64_t m_skinLastHeapBda = 0;
	uint32_t m_skinLastDeformedBase = UINT32_MAX;
	// TMP-DIAG (TOURMALINE_SKINBOUNDS_DUMP=<frame>): the last ok skin push's
	// deformed half (handle + parity offset + bytes) for the post-Execute
	// sanity dump in DrawFrame — settles garbage-vs-legit for the far-out
	// cone axes + the knight's missing chunks (in the unlit view too ->
	// shared legs). Remove when the investigation closes.
	smvk::BufferHandle m_skinDumpBuf{};
	uint64_t m_skinDumpOff = 0;
	uint64_t m_skinDumpBytes = 0;
	float m_cubemapOverlayMaxDist = 80.0f; // overlay distance cutoff, meters (UI)
	uint32_t m_cubemapSampler = 0;  // clamp sampler for TextureCube (created once)
	struct CubemapProbeGpu {
		glm::vec4 posInfluence{ 0.0f }; // xyz = center (world), w = influence radius
		glm::vec4 fadeCubemap{ 0.0f };  // x = fade, y = cubemap idx bits, z = mipLevels, w = pad
		glm::vec4 boxMinPad{ 0.0f };    // xyz = depth-fitted box min (world), w = pad
		glm::vec4 boxMaxPad{ 0.0f };    // xyz = depth-fitted box max (world), w = pad
	};
	static_assert(sizeof(CubemapProbeGpu) == 64);
	std::vector<CubemapProbeGpu> m_cubemapProbes; // cached probe rows (<= 32 appended)
	void LoadCubemaps();
	// ---- Cubemap probe streaming (distance residency) -----------------
	// LoadCubemaps used to upload EVERY manifest row up front (KTX decode
	// + GPU upload per probe at scene load). Grid manifests with dozens
	// of probes made that a load hitch + resident VRAM for probes the
	// camera may never approach. Rows are now metadata until needed (see
	// CubemapRow): pinned rows (artist captures) stay resident always;
	// other rows stream by camera distance - nearest-K plus any probe
	// whose influence+fade covers the eye (mandatory: the shader breaks
	// at the first full-weight entry, so a covering probe MUST be
	// resident or lighting pops to the global fallback). Loads throttle
	// to a couple per update (KTX decode is synchronous); evictions
	// carry hysteresis (rank slack + range margin) so boundary orbits
	// don't thrash. Probe rows rebuild from resident entries (fresh
	// bindless indices) whenever residency changes. The global /
	// single-capture path (one probe, no rows) never streams.
	struct CubemapRow {
		std::string entryName;  // library key (manifest basename)
		std::string vfsPath;    // KTX payload source (reloads)
		glm::vec3 position{ 0.0f };
		float influenceRadius = 10.0f;
		float fadeRadius = 5.0f;
		// Depth-fitted proxy box (manifest "box" group; symmetric
		// position+-influence for legacy rows). Rides the probe entry
		// into the lit shader's box-parallax correction.
		glm::vec3 boxMin{ 0.0f };
		glm::vec3 boxMax{ 0.0f };
		bool pinned = false;
		bool failed = false;    // payload missing/undecodable: skip forever
	};
	std::vector<CubemapRow> m_cubemapRows; // manifest metadata (all rows)
	uint32_t m_cubemapStreamBudget = 48;   // resident non-pinned rows (UI)
	uint32_t m_cubemapStreamLoadsPerUpdate = 2;
	float m_cubemapStreamEvictRankSlack = 8.0f;
	float m_cubemapStreamEvictRangeMargin = 4.0f; // meters past fade
	bool m_cubemapStreamPrimed = false;    // first update fills synchronously
	void UpdateCubemapStreaming(const glm::vec3& eye);
	void RebuildCubemapProbeRows();
	// Probe entries appended to the FP light upload this frame (0 when
	// disabled or nothing loaded). The dispatch gate + the freshness bit
	// share this so shader and GPU lists can never disagree.
	uint32_t ProbeEntryCount() const {
		if (!m_cubemapEnabled || m_cubemapProbes.empty())
			return 0;
		return static_cast<uint32_t>(
			std::min<size_t>(m_cubemapProbes.size(), smvk::kForwardPlusMaxProbes));
	}
	// Single gate for the tile-bucketed probe path: the forward+ tile cull
	// files probe entries into the shared light tile lists, and
	// FrameUniforms.probeTileFresh publishes 1/0 so the lit shader and the
	// read hints can never disagree with the dispatch below.
	bool ProbeTileListsWanted() const {
		return ProbeEntryCount() > 0
			&& m_passes.forwardPlusCull
			&& m_viewportResources.forwardPlus.Valid();
	}
	uint32_t m_spotShadowCount = 0; // shadowed spots this frame (tiles used)
	int32_t m_spotShadowMapIndex = -1; // bindless spot atlas, -1 = none
	std::vector<smvk::GpuCascadeDescription> m_spotShadowTiles; // spot tiles this frame (feed the spot RenderViews)
	// Point-light shadows (six cube-face tiles per light, same atlas):
	// frame numbering is spots first, faces after, so the combined tile
	// index of face f of light k is m_spotShadowCount + k*6 + f.
	uint32_t m_pointShadowLightCount = 0; // shadowed points this frame
	uint32_t m_pointShadowTileCount = 0;  // 6 * light count (face tiles used)
	std::vector<smvk::GpuCascadeDescription> m_pointShadowTiles; // face tiles this frame (feed the face RenderViews, CubemapFace order per light)
	// Total local-light shadow tiles this frame (spots + point faces): the
	// pc.spotShadowCount / DsPhasePush.spotShadowCount the shaders guard
	// tile indexing against (the array bound is kMaxLocalShadowTiles).
	uint32_t LocalShadowTileCount() const {
		return m_spotShadowCount + m_pointShadowTileCount;
	}
	// Shadow contribution culling: during shadow map rendering, cull casters
	// that can never shadow the MAIN camera's view. CASCADE views test
	// against m_casterContributionView - the main frustum swept toward the
	// sun (MakeCasterContributionView): the RAW frustum dropped casters
	// outside the camera that still cast INTO it (oblique sun - shadow pop
	// at the screen edges while moving the camera). SPOT views skip the
	// test entirely: the spot cone (their own view planes) is the exact
	// bound, and a sun-shaped sweep is the wrong geometry for a spot origin.
	bool m_shadowContributionCulling = true;
	// Per-frame caster contribution volume (built where the frame's views
	// are allocated, from mainRenderView + the sun's direction; see
	// MakeCasterContributionView). Consumed by BOTH caster paths: the
	// direct item-level cull (RecordShadowCastersDirect) and the indirect
	// mesh-bound discard (gfx::BeginShadowView's contributionView).
	smvk::RenderView m_casterContributionView{};
	bool m_casterContributionViewValid = false;
	// LiSPSM (Light Space Perspective Shadow Maps) for spotlights: warp the
	// spot projection to match the camera's perspective frustum, maximizing
	// texel density in camera/view space. When enabled, the spot projection is
	// warped to match the main camera's perspective frustum for better texel
	// density.
	bool m_spotLightLiSPSM = true;
	// ---- EVSM (Exponential Variance Shadow Maps, prefiltered async) ----
	// Opt-IN (default OFF; the ImGui checkbox or TOURMALINE_EVSM=1). When
	// enabled (and the device supports RG32F 2xMSAA moments), the
	// main view's cascade/spot depth passes are replaced by EVSM moment
	// passes (RG32F 2xMSAA + resolve) followed by separable Gaussian blur
	// passes marked AsyncCompute. The blur runs on the dedicated compute
	// queue (G1/C/G2 split submit, SameFrame default — no shadow lag); the
	// OneFrameLag pipeline stays available in the UI. shadow_map_index
	// fields then address the blurred atlas; FrameUniforms.evsmMode tells
	// shaders. Falls back to PCF automatically when unsupported/invalid.
	// MAIN-view only: the multiview secondary views always keep their PCF
	// depth atlases (the moment atlases are per-view memory giants).
	bool m_evsmEnabled = false; // user toggle (ImGui); effective only if supported
	// SameFrame default: moments are blurred on the true-async compute
	// queue and consumed by this frame's lighting (no shadow lag/wobble).
	// OneFrameLag remains in the ImGui combo for overlap comparison.
	smvk::EvsmPipelineMode m_evsmPipeline = smvk::EvsmPipelineMode::SameFrame;
	// Directional sampling descriptors for OneFrameLag: the blurred atlas
	// read by lighting trails one flight slot, so it was baked with LAST
	// frame's cascade fit — the sampling copy of
	// GpuDirectionalLightDescription must trail one frame too or the
	// receiver projection disagrees with the stale map content (EVSM-only
	// acne under camera motion). SameFrame uploads the current fit.
	smvk::GpuDirectionalLightDescription m_dirSamplePrev{};
	bool m_dirSamplePrevValid = false;
	// Soft shadows: artistic scalar over the physical penumbrae —
	// directional uses the sun disc angular radius (environment
	// sunDiscSizeDeg) projected per cascade; spots use the emitting-sphere
	// diameter (m_evsmSpotLightSize) with PCSS similar-triangles. 0 = hard.
	float m_evsmSoftness = 1.0f;
	float m_evsmSpotLightSize = 0.35f; // m, matches the demo PCSS light size
	// ---- Directional CSM tuning (UI, Quality tab) ----
	// Tile resolution recreates the depth + EVSM moment atlases on change
	// (one-time hitch via the idempotent per-frame Ensure).
	uint32_t m_cascadeResolution = 1024; // per-cascade tile (512..4096; canon = PS5-quality 1024)
	uint32_t m_spotShadowResolution = 512; // per-spot tile (256..1024; canon = PS5-quality 512)
	// Split blend for ComputeCascadeSplits: 0 = linear, 1 = logarithmic.
	float m_cascadeLambda = 0.8f;
	// Shadow range cap (m). 0 = scene farClip; otherwise clamped to
	// [near + 1, scene farClip] and used as the split far plane, so the
	// cascades concentrate where the slider says.
	float m_cascadeMaxDistance = 0.0f;
	// Multipliers over the engine's per-cascade auto bias (texel-scaled:
	// normalBias ~1.25 texels world, depth bias ~0.5 texels / depthRange).
	float m_cascadeDepthBiasScale = 1.0f;
	float m_cascadeNormalBiasScale = 1.0f;
	// Effective split far plane (see m_cascadeMaxDistance).
	float CascadeFar() const {
		if (m_cascadeMaxDistance > 0.0f)
			return std::clamp(m_cascadeMaxDistance,
				default_near_clip + 1.0f, m_sceneFarClip);
		return m_sceneFarClip;
	}
	// Live tile extent (atlas after Ensure; falls back to the selected
	// resolution before the first Ensure).
	smvk::Extent2D CascadeTileExtent() const {
		const auto& atlas = m_viewportResources.directionalShadows;
		if (atlas.tileExtent.width != 0 && atlas.tileExtent.height != 0)
			return atlas.tileExtent;
		return { m_cascadeResolution, m_cascadeResolution };
	}
	// Last built per-cascade splits + applied biases (filled by
	// UpdateLights every frame; read by the shadow UI sections).
	float m_cascadeSplitsDbg[5] = {};
	float m_cascadeBiasDbg[4] = {};
	float m_cascadeNormalBiasDbg[4] = {};
	// ---- SDSM (Sample Distribution Shadow Maps) ----
	// GPU-driven, distribution-based cascade fitting:
	//  - sdsm_stats.cs.hlsl reduces the CURRENT frame's render depth every
	//    frame (decimated min/max + a 64-bin log2 view-depth histogram per
	//    group; a dedicated one-pass reduce because the occlusion HZB is
	//    min-only + sky-poisoned and the MAX pyramid is SSR-conditional).
	//  - The stats ride an OWN async readback ticket submitted every
	//    frame (not the shared every-4th batch) -> ONE-FRAME latency:
	//    current-frame measurement, next-frame fit.
	//  - UpdateLights merges the 256 partial histograms on the CPU and
	//    fits the split range from the 1%/99% PERCENTILE range (the
	//    histogram point: one tiny distant object must not dictate the
	//    whole range), padded and clamped to [nearClip, CascadeFar()].
	//  - Temporal stabilization: the applied range follows the measured
	//    range with asymmetric hysteresis (instant expand — never clip a
	//    receiver; slow contract ~2%/frame — projections don't swim) and
	//    is quantized to 0.25 m steps (tiny distribution changes don't
	//    move the light projections at all).
	//  - CUT/discards: a camera teleport (per-frame position jump), stale
	//    stats, or a measured range escaping the applied one by >30%
	//    discards the hysteresis history. A teleport additionally cools
	//    down on the fixed heuristic range for 2 frames — the 1-frame-old
	//    stats describe the OLD view, so they are wrong for the new one;
	//    the fresh post-cut stats snap in after the cooldown.
	static constexpr uint32_t kSdsmGroups = 256; // must match SDSM_GROUPS (sdsm_stats.cs.hlsl)
	static constexpr uint32_t kSdsmBins = 64;    // must match SDSM_BINS (sdsm_stats.cs.hlsl)
	static constexpr uint32_t kSdsmGroupBytes =
		2 * sizeof(float) + kSdsmBins * sizeof(uint32_t); // 264: must match HLSL SdsmGroupStats
	static constexpr uint32_t kSdsmStatsBytes = kSdsmGroups * kSdsmGroupBytes;
	struct SdsmStatsPush { // must mirror the HLSL SdsmStatsPush (40 bytes)
		uint64_t stats = 0;   // BDA of the per-flight SdsmStatsOut buffer
		uint32_t depthIndex = UINT32_MAX;
		float zNear = 0.01f;
		float zFar = 100.0f;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t stride = 1;
		uint32_t pad0 = 0;
	};
	static_assert(sizeof(SdsmStatsPush) == 40, "SDSM stats push must mirror the HLSL SdsmStatsPush (40 bytes)");
	// Must mirror the HLSL SdsmGroupStats (264 bytes).
	struct SdsmGroupStats {
		float minV = 0.0f;
		float maxV = 0.0f;
		uint32_t bins[kSdsmBins] = {};
	};
	static_assert(sizeof(SdsmGroupStats) == kSdsmGroupBytes,
		"SDSM group stats must mirror the HLSL SdsmGroupStats (264 bytes)");
	// Raw measured range (from the last completed stats readback).
	bool m_sdsmEnabled = true;   // TOURMALINE_SDSM=0/1 override at init; UI toggle
	bool m_sdsmStatsValid = false; // a completed readback exists
	float m_sdsmMin = 0.0f;      // measured true min/max view depth
	float m_sdsmMax = 0.0f;
	float m_sdsmPctMin = 0.0f;   // measured 1%/99% percentile range
	float m_sdsmPctMax = 0.0f;
	uint64_t m_sdsmMeasureFrame = 0; // frame whose depth the consumed stats measured
	bool m_sdsmLogged = false;   // one-shot fit log
	// Hysteresis state (the APPLIED range; see the block comment above).
	bool m_sdsmAppliedValid = false;
	float m_sdsmAppliedMin = 0.0f;
	float m_sdsmAppliedMax = 0.0f;
	uint32_t m_sdsmCutCooldown = 0; // teleport cooldown: heuristic range frames
	glm::vec3 m_sdsmLastCamPos{ 0.0f };
	bool m_sdsmLastCamPosValid = false;
	// Per-flight stats buffers + the readback plumbing (own ticket).
	smvk::Holder<smvk::BufferHandle> m_sdsmStatsBuffers[2]{};
	std::vector<uint8_t> m_pendingSdsmStats; // kSdsmStatsBytes; readback destination
	uint64_t m_sdsmReadbackTicket = 0;       // own 1-in-flight async ticket
	uint32_t m_sdsmReadbackSlot = 0;         // flight slot the ticket copies
	bool m_sdsmStatsArmed = false; // the stats pass ran this frame
	bool EvsmActive() const {
		return m_evsmEnabled
			&& m_viewportResources.evsmDir.Valid()
			&& m_viewportResources.evsmDir.ShadersValid()
			&& m_viewportResources.evsmSpot.Valid()
			&& m_viewportResources.evsmSpot.ShadersValid();
	}
	// Direct-path EVSM moment recording: depth-only 2x MSAA through the
	// PCF shadow programs (RecordShadowCastersDirect with Count2 MSAA) —
	// the per-sample resolve (evsm_resolve.cs.hlsl) reconstructs the
	// moments from the MSAA depth right after the caster passes.
	smvk::RenderViewSet m_renderViews{}; // per-frame virtual cameras: main view + shadow cascade/spot views
	// Sync owner of a frame: fb.swapchain when the host supplied one
	// (the element contract — the host acquires), else the borrowed
	// host swapchain (init-time sizing). RenderFrame must resolve EVERY
	// per-frame sync property (extent, formats, flights) through these —
	// so the renderer stays oblivious to who acquired the frame.
	smvk::Swapchain* FrameSync(const smvk::Framebuffer& fb) const {
		return fb.swapchain ? fb.swapchain : m_swapchain;
	}
	smvk::Extent2D FrameSwapExtent(const smvk::Framebuffer& fb) const {
		if (const smvk::Swapchain* s = FrameSync(fb))
			return s->GetExtent();
		return smvk::Extent2D{ 0, 0 };
	}
	smvk::Format FrameSurfaceFormat(const smvk::Framebuffer& fb) const {
		if (const smvk::Swapchain* s = FrameSync(fb))
			return s->GetSurfaceFormat();
		return smvk::Format::Undefined;
	}
	smvk::Format FrameDepthFormat(const smvk::Framebuffer& fb) const {
		if (const smvk::Swapchain* s = FrameSync(fb))
			return s->GetDepthFormat();
		return smvk::Format::Undefined;
	}
	uint32_t FrameFlights(const smvk::Framebuffer& fb) const {
		if (const smvk::Swapchain* s = FrameSync(fb))
			return s->framesInFlight();
		return 0;
	}
	// This frame's layer rect clamped into the sync-owner extent (full swap
	// when unset). Swap writes (tonemap / SMAA resolve / UI composite)
	// confine to it via pass SetOffset/SetExtent with Load semantics; the
	// camera aspect + render extent derive from it.
	LayerRect ClampedLayer(const smvk::Framebuffer& fb) const {
		return ClampLayerRect(m_layerRect, FrameSwapExtent(fb));
	}
	LayerRect m_layerRect = LayerRect::Full();
	// The renderer's content overlay (loose content/ + the project root
	// below it), mounted into the CALLER'S
	// vfs (OverlayVfs is non-copyable).
	void MountContentVfs(smvk::OverlayVfs& vfs) const;
	// ---- GeometryFX-style per-view filter (clustered, see App/renderer.h) ----
	engine::GfxRenderer m_gfxRenderer{};
	// The filtered main view is ALWAYS ON - the runtime A/B toggle (false =
	// legacy direct draws) is gone. This latch is setup-time availability
	// ONLY (onAttach): Ensure failure or an empty cluster heap drops the
	// filter leg for the session (gfxSlot stays invalid; everything draws
	// direct - graceful skip, never fatal). Never user-toggled.
	bool m_gfxFilterAvailable = true;
	// Task-less mesh-shader main leg (opaque ranges, mesh
	// FS twins for base/sg/gi variants). False or unsupported device =
	// indexed multi-draws (CPU fallback).
	bool m_useGfxMesh = true;
	// ---- Cooperative shadow draw lists (cascade + spot tiles) ----
	// Candidates are prebuilt once per frame on the render thread (LOD
	// decisions mutate the World halfrate cache — not thread-safe), then
	// each shadow view's command list is built on a worker while the main
	// thread records the rest of the frame. Futures join before Execute
	// (the tile-pass lambdas read the built ranges) and the ring uploads
	// run on the main thread right after (see UploadShadowList).
	void BuildShadowDrawCandidates();
	// Direct caster recording, both phases (opaque VS-only + masked
	// alpha-test): the full legacy loop when the filter is off, and the
	// unfiltered-caster fallback inside the indirect tile passes
	// (skipFiltered=true skips registered meshes — they are covered by
	// the per-view command list).
	void RecordShadowCastersDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& opaqueProgram, const IRenderer::PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered,
		const smvk::MSAASettings& msaa = smvk::MSAASettings{});
	smvk::mem::LinearPtr<smvk::ThreadPool> m_shadowWorkers;
	std::vector<std::future<void>> m_shadowFutures;
	std::vector<smvk::gfx::DrawRequest> m_shadowCandidates;
	// ---- Option A: occluder-only HZB depth ----
	// (The m_hizOccluderDepth toggle is GONE: the occluder-depth fill +
	// the HZB pyramid are a FIXED COST of the renderer - the fill is the
	// pyramid's only source, hizDepth is not used for anything else. The
	// lever for HZB CULLING is the filter's occlusion cull flag
	// (CullFlagBit_HzbOcclusion - TOURMALINE_HIZ=0, TOURMALINE_CULLFLAGS,
	// or the UI checkbox), never the fill/build.)
	float m_hizOccluderMinPixels = 24.0f; // projected bound-diameter threshold (px)
	float m_hizOccluderMinRadius = 0.5f;  // world-space bound-radius threshold (m)
	bool m_useFilteredMotion = true; // false = motion stays fully direct
	// ---- Meshlet/cone debug views (see App/renderer.h debug programs) ----
	// m_debugView selects the Main opaque program (F1..F6); anything but Lit
	// bypasses the GPU filter with direct draws so culled data stays visible.
	ViewMode m_debugView = ViewMode::Lit;
	// Text-only stats HUD (top-left overlay, Ctrl+T): framerate, GPU,
	// camera, filter culls, LODs, exposure, streaming. Defaults OFF (a
	// clean frame; the toggle is the light way back). It carries the live
	// numbers removed from the panel header + Stats tab (which keep
	// controls, explanations and detail breakdowns).
	bool m_showHud = false;
	// The debug PANEL (the right-hand "Tourmaline - Debug" sidebar): TAB
	// toggles it (keybind::DEBUG_PANEL_TOGGLE; the window's close X flips
	// the same flag). Defaults OFF — hosts render a clean scene by
	// default; the stats HUD above is the light readout when wanted.
	bool m_debugPanelOpen = false;
	int m_debugMeshletMode = 0;   // 0 = random per meshlet, 1 = cone heat
	bool m_hideTransparent = false; // debug: skip all transparent draws (bisect blending/overdraw out)
	// Wireframe (P): raster override on every scene-triangle bind in the
	// Main pass — PolygonMode::Line + Cull None (see backfaces). Composes
	// with any debug view (lit/unlit/meshlets/LODs) and the transparent
	// leg; skydome, cone-line overlays and the depth prepass stay Fill so
	// lines keep hidden-line removal against the prepass depth.
	bool m_wireframe = false;
	smvk::TransparentMode m_transparentMode = smvk::TransparentMode::Blended;
	bool m_debugCones = false;    // cone-axis line overlay (the GPU leg:
	                               // SetConeDebug arms GfxConeDebug, Main
	                               // Draws the GPU-written candidate lines)
	bool m_dumpIndirectAudit = false; // one-shot: dump GPU indirect cmds (stderr)
	// Opt-in EVSM atlas debug dump (TOURMALINE_EVSM_DUMP=<frame>): one-shot
	// after Execute — per-stage stats (moments/temp/blurred) + a log-encoded
	// PNG of the cascade-0 tile in exports/ (edge-band width = prefilter
	// width). Same pattern as TOURMALINE_DS_DUMP (stalls; debug only).
	int m_evsmDumpAtFrame = -1;
	void EvsmDumpAtlas(uint32_t frameIndex);
	// Opt-in SSR/volumetric stage dump (TOURMALINE_SSR_DUMP=<frame>): the
	// HDR chain inputs + every SSR/volumetric intermediate at frame N and
	// N+1, with NaN/Inf/garbage stats on stderr and tone-mapped PNGs
	// (NaN/Inf hot-pink) in exports/. Pinpoints WHICH stage turns wrong.
	int m_ssrDumpAtFrame = -1;
	void DumpSsrStages(uint32_t frameIndex);
	// SSR contribution debug attachment (TOURMALINE_SSR_DEBUG=1): arms
	// SsrDesc.debugContrib - the composite binds its .dbg twin and
	// writes the EXACT add term (what the reflections contribute to the
	// final HDR) to the ssrContrib second attachment; the ssrdump's
	// ssr_contrib stage exports it. Wasteful by design (a full-res
	// RGBA16F target + write), debug only.
	bool m_debugSsr = false;
	// Ground-truth depth-view pass (runs when TOURMALINE_SSR_DUMP is armed):
	// samples the depth through the SSR trace's exact bindless path and
	// writes it to the (post-blur dead) raw target for the dump.
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrDepthViewVs;
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrDepthViewPs;
	smvk::ShaderProgram m_ssrDepthViewProgram{};
	// Ground-truth stencil-view twin (TOURMALINE_SSR_DUMP +
	// TOURMALINE_SSR_STENCILVIEW): loads the receiver-material stencil
	// through the trace's exact uintTextures[] path and writes it (raw
	// byte in R, decoded STENCIL_SSR / STENCIL_SSR_METAL flags in G/B)
	// to the (post-composite dead) blurred target for the dump.
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrStencilViewVs;
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrStencilViewPs;
	smvk::ShaderProgram m_ssrStencilViewProgram{};
	// ---- SSSR trace Hi-Z (MAX-reduce over the COMPLETE renderDepth —
	// see hzb_max.cs.hlsl): the tracing twin of the gfx filter's culling
	// HZB. The culling pyramid is MIN-reduced (farthest-surface, the
	// occlusion-cull conservatism) and built over the seed's partial set
	// / Option A crudest-LOD occluders BEFORE the depth passes complete —
	// unusable for ray marching. This one is MAX-reduced (NEAREST surface
	// per block — the ray-trace direction) over the final renderDepth,
	// emitted AFTER all depth passes. The BUFFER + mip layout moved into
	// smvk::Ssgi::hzb (Engine/Rendergraph/ssgi.h) — it became a SHARED
	// screen-space primitive (the SSR trace's mip walk, the SSR
	// blur/compose block reps and the SSGI AO chain all read it); this
	// renderer keeps only the module/program that runs the build passes.
	// The trace's empty-space skip proves "every surface in the covered
	// block is behind the ray's next position" from the block's nearest
	// value — only provably-empty blocks are skipped (a missed reflection
	// at worst, never a see-through).
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrHzbModule;
	smvk::ShaderProgram m_ssrHzbProgram{};
	// ---- SPD HZB build (hzb_spd.cs.hlsl — Phase 1 of the HZB upgrade,
	// docs in the shader header): the single-dispatch twin of the
	// per-mip build above. A/B + validation switches, read ONCE at
	// startup like TOURMALINE_SSR_BLUR so the headless bench matrix can
	// pin them:
	//   TOURMALINE_HZB_SPD=1        — SPD feeds the consumers (default
	//                                  OFF: the multi-mip path stays live
	//                                  until the two produce identical
	//                                  pyramids).
	//   TOURMALINE_HZB_SPD_VERIFY=1 — multipass feeds the consumers; SPD
	//                                  builds a SHADOW pyramid
	//                                  (Ssgi::hzbVerify) from the same
	//                                  depth; post-Execute byte-compare
	//                                  of every mip (stalls — debug
	//                                  only). Forces the proven path for
	//                                  the frame's actual consumers, so a
	//                                  broken SPD cannot corrupt output
	//                                  while it is being validated.
	// Extents beyond 4096 (the tested 64x64-tile upper-level capacity,
	// AMD SPD's documented limit) always fall back to the per-mip path.
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrHzbSpdModule;
	smvk::ShaderProgram m_ssrHzbSpdProgram{};
	bool m_hzbSpd = false;
	bool m_hzbSpdVerify = false;
	bool m_hzbSpdVerifyArmed = false;  // this frame built both pyramids
	uint64_t m_hzbSpdVerifyFrames = 0; // running compare stats (report)
	uint64_t m_hzbSpdVerifyMismatches = 0;
	void CompareHzbSpdVerify();        // post-Execute one-shot (stalls)
	bool m_debugDumpHzb = false;    // one-shot: dump HZB buffer contents (stderr)
	bool m_dumpFirstFrameHzb = false;      // opt-in: dump HZB once on the next frame (stalls)
	int m_dumpAtFrame = -1;                // opt-in (TOURMALINE_DS_DUMP=<frame>): one-shot deferred-shadow dump (stalls)
	void DumpDeferredShadows(uint32_t frameIndex); // post-Execute one-shot (stalls)
	// Headless A/B helpers: auto-capture at a fixed frame (env
	// TOURMALINE_CAPTURE=<n>; -1 = off) keyed on the render-frame counter.
	int m_captureAtFrame = -1;
	uint64_t m_frameCounter = 0;
	// ---- Debug culling freeze: hold the filter's (view, eye) + HZB while
	// the render camera moves freely, so culling decisions stay put and
	// their effect (missing survivors, holes) is inspectable. Captured on
	// the rising edge; auto-released on extent drift (a resized HZB would
	// rebuild empty while held -> garbage reads).
	bool m_freezeCullingView = false;
	bool m_frozenViewValid = false;
	glm::mat4 m_frozenViewProj{ 1.0f };
	glm::vec3 m_frozenEye{ 0.0f };
	smvk::Extent2D m_frozenExtent{ 0, 0 };
	engine::ForwardPlusRenderer m_fpRenderer{};
	// ---- Deferred shadows (tiled/zBin pipeline; H toggles) ----
	bool m_dsEnabled = true;
	// ---- SG-GI baked probe volume (tools/gi_baker output, see
	// smvk::gi + Engine/GI). Loaded once at scene init from the
	// CLUSTER bake-payload channel (ReadBakeBytes: gi/<scene>.sggi,
	// pak-embedded or editor-injected). N cycles the
	// ambient source (All -> Lightmaps -> Probes -> SkyIbl -> Off ->
	// Baked — see AmbientMode); R / F8 cycle the baked-GI debug
	// views. ----
	AmbientMode m_ambientMode = AmbientMode::All; // N cycles (production default)
	int m_giDebug = 0;             // R / F8 cycle: 0 off, 1 baked irradiance,
	                                // 2 GI source palette (the octree's
	                                // level-isolation/argmax debug modes died
	                                // with the walker; 3/4 read as 1 here)
	int m_giDebugLevel = 0;        // (retired with the octree level debugs —
	                                // feeds the frame's legacy giDebug byte,
	                                // always 0)
	float m_giIntensity = 1.0f;   // ambient scale (FrameUniforms.giIntensity)
	bool m_giShowProbes = false;  // probe-point 3D overlay (the coplanarity check:
	                              // red = blocked, yellow = near-surface, green = open)
	float m_giProbePointMaxDist = 40.0f; // overlay: draw probes within N m
	bool m_giShowVolumeBounds = false; // GI volume AABB/OBB wireframe overlay
	                              // (ImGui foreground, on top of the current
	                              // view: regular OBBs + convex grid AABBs,
	                              // no depth test; the legacy octree bounds
	                              // box + the probe-point overlay died with
	                              // the .sggi loader)
	// ---- SG 2D lightmaps (STATIC geometry, cook --lightmap output).
	// Loaded once at scene init from the CLUSTER bake-payload channel
	// (ReadBakeBytes: gi/<scene>.sglm): uploads ONLY the atlas textures
	// (bindless). Chart uvs + chart ids + per-chart atlas transforms +
	// per-mesh lobe counts are COOKED into the pak (texCoord2/chartId,
	// lmChartXforms, StaticMesh::lmChunk riding the draw ring) — no
	// chunk buffer exists at runtime. Default OFF (m_lmEnabled): the
	// lit shader ignores loaded lightmaps until opted in here or via
	// TOURMALINE_LIGHTMAP=1. Missing file = not baked yet (non-fatal).
	bool m_lmLoaded = false;
	bool m_lmEnabled = false;
	smvk::gi::SgLightmapAtlas m_lmAtlas; // CPU mirror (chunk table + debug views)
	// Atlas texture mirrors: 12 slots — the uncompressed path fills 9
	// (kLightmapLayerCount RGBA16F stream layers, kind 0), the BC6H
	// path fills scene-lobe-count one-per-lobe textures (kind 1).
	smvk::Holder<smvk::TextureHandle> m_lmLayers[12];
	uint32_t m_lmLayerSlots[12] = {}; // bindless slots (FrameUniforms.lmLayers)
	uint32_t m_lmLayerKind = 0;       // FrameUniforms.lmLayerKind mirror
	bool LmActive() const {
		return m_lmLoaded && m_lmEnabled && LmWanted();
	}
	// Bake-channel load (ReadBakeBytes: pak-embedded or editor-injected;
	// no VFS needed — the channel already resolved it).
	void LoadGiLightmaps();
	// The legacy .sggi octree loader (LoadGiOctree + the grid/dense-layer
	// members) is DELETED — stage 3 of the octree retirement. The
	// FrameUniforms gi* mirror fields stay as the zeroed legacy ABI
	// (the C++/HLSL layout static_asserts never renumber); the giVariant
	// ID 1 (GI_OCTREE) stays reserved. The probe-visibility helpers and
	// the probe-point overlay died with the loader.
	// Ambient-source gating (see AmbientMode): the per-source "wanted"
	// helpers drive the FrameUniforms enable flags; the *Active() gates
	// additionally require the content to be loaded. Sky IBL rides the
	// env flag (All + SkyIbl) — Baked deliberately EXCLUDES it (the
	// isolation: the sky flag also gates the GI-volume composite's sky
	// base in the lit shader).
	bool IblWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::SkyIbl;
	}
	bool ProbesWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::Baked
			|| m_ambientMode == AmbientMode::Probes;
	}
	bool LmWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::Baked
			|| m_ambientMode == AmbientMode::Lightmaps;
	}
	bool RegWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::Baked
			|| m_ambientMode == AmbientMode::Probes;
	}
	// GiActive() — the legacy .sggi octree gate — is DELETED with the
	// loader (stage 3 of the octree retirement): frame.giEnabled is
	// unconditionally 0 and the gi* FrameUniforms block is the zeroed
	// legacy ABI.
	// GI bake names resolved from the loaded clusters' registry (first
	// binding per kind wins + warn). The registry names: legacy paks
	// carry bake STEMS (gi/<stem>.sggi + friends — the loader adds the
	// gi/ prefix + extension); scene-bound paks carry FULL VFS paths
	// (gi/<scene>.sggi — the tour giAsset hard binding, no stem
	// guessing, pak name never involved). Resolved once per scene load,
	// before the GI loaders run. m_giVolumePaths: the per-volume GiVolume
	// bindings (Kind::GiVolume — one FULL vfs path per authored GI
	// volume, the scene's hard identification; manifest-free).
	// (The Probe-kind binding is acknowledged-and-ignored at load: the
	// octree loader is gone; old paks still carry the records.)
	std::string m_giLightmapName, m_giRegName, m_giVolumeManifestName,
		m_giCubemapName;
	std::vector<std::string> m_giVolumePaths;
	void ResolveGiBakes();
	// Bake-payload channel read (see World::ReadBakeBytes): the bytes at
	// a GI/cubemap VFS path from the world's clusters — dynamic cluster
	// first, so runtime quick-bakes override pak payloads. Empty = miss.
	std::span<const uint8_t> ReadBakeBytes(const std::string& vfsPath) const {
		return m_world ? m_world->ReadBakeBytes(vfsPath)
			: std::span<const uint8_t>{};
	}
	void CycleAmbientMode() {
		m_ambientMode = static_cast<AmbientMode>(
			(static_cast<int>(m_ambientMode) + 1) % 6);
	}
	// (LoadGiOctree — the legacy .sggi loader — is DELETED; see the
	// members' retirement note above the ambient gating.)
	// (LoadCubemaps declared with the cubemap members above.)
	// ---- SG 2D lightmaps: DEPRECATED and removed (were STATIC-geometry
	// atlas textures plus a runtime branch). The pak-side chart payloads
	// (texCoord2/chartId, lmChartXforms) stay cooked — the GPU filter's VS
	// still indexes the chart-transform table (see EnsureChartXforms).
	// ---- SG regular 3D probe volumes (DYNAMIC objects, tools/gi_baker
	// --regular-out output; artist OBBs + density, hardware trilinear).
	// Loaded from the CLUSTER bake-payload channel (ReadBakeBytes:
	// gi/<scene>.regmanifest + its rows): up to 8 volumes, each 7
	// RGBA16F 3D textures + one OBB descriptor. Inside a volume's OBB the
	// grid REPLACES the sky-IBL ambient for unflagged (dynamic) draws. ----
	bool m_regLoaded = false;
	bool m_regEnabled = false;         // UI toggle (dynamic-object GI)
	struct RegVolume {
		smvk::gi::SgRegularVolume volume; // CPU mirror (OBB; coeffs freed)
		smvk::Holder<smvk::TextureHandle> layers[7]; // RGBA16F 3D
		uint32_t layerSlots[7] = {};    // bindless slots
	};
	std::vector<RegVolume> m_regVolumes;
	smvk::Holder<smvk::BufferHandle> m_regObbsBuffer;
	uint64_t m_regObbsAddress = 0;
	bool RegActive() const { return m_regLoaded && m_regEnabled; }
	void LoadGiRegularVolumes();
	// ---- GI volumes (Engine/GI/cubemap_volume.h; the probes-only
	// pivot's confined-space path). Loaded from the CLUSTER bake-payload
	// channel (ReadBakeBytes — the scene's hard per-volume bindings, the
	// tour giAsset records): up to 8 volumes, each 7 RGBA16F 3D textures
	// (dense probe grid, HW trilinear) + one descriptor (convex planes +
	// per-plane fades + priority). The lit shader composites them ordered
	// over the sparse octree world field (volumes BLEND by priority —
	// never cross-interpolated with the octree's neighbors). ----
	bool m_giVolumesLoaded = false;
	bool m_giVolumesEnabled = false;       // UI toggle (confined-volume GI)
	struct GiVolumeResources {
		smvk::gi::GiVolume volume;       // CPU mirror (def; coeffs freed)
		smvk::Holder<smvk::TextureHandle> layers[7]; // RGBA16F 3D
		uint32_t layerSlots[7] = {};    // bindless slots
		// Isotropic-mean twin (the volumetric fog's one-fetch path —
		// PackGpuMeanLayer): created under the same failure discipline
		// as the 7 layers (a volume publishes only with all 8 up).
		smvk::Holder<smvk::TextureHandle> meanTex;
		uint32_t meanSlot = UINT32_MAX; // bindless slot
		// Visibility twins (PackGpuVisLayers — the through-wall
		// rejection): 2 RGBA8_UNORM 3D textures (the 6 axis distances +
		// the clean-cell flag). SOFT against the failure discipline:
		// failed vis layers publish with UINT32_MAX slots — the shader
		// treats absent vis as all-clean (the plain hardware trilerp,
		// the pre-visibility behavior) instead of dropping the volume's
		// GI entirely.
		smvk::Holder<smvk::TextureHandle> visLayers[2];
		uint32_t visSlots[2] = { UINT32_MAX, UINT32_MAX };
	};
	std::vector<GiVolumeResources> m_giVolumes;
	smvk::Holder<smvk::BufferHandle> m_giVolumesBuffer;
	uint64_t m_giVolumesAddress = 0;
	bool GiVolumesActive() const {
		return m_giVolumesLoaded && m_giVolumesEnabled && ProbesWanted();
	}
	// Forward+ tile-cull DISPATCH gate — the frame's single source of
	// truth, consumed by the dispatch site AND the FrameUniforms
	// GI-volume-tile-mask publish (the mask address may only go out when the
	// dispatch actually refreshes the masks this frame; a skipped
	// dispatch would leave stale bits masquerading as fresh). Classic
	// forward+ always runs it; under deferred shadows it runs only when
	// a tile-list consumer still needs the lists: decals, probes, or
	// GI-volume masks (the DS pipeline owns the lights then; the
	// volume leg re-runs as live work, like the decal leg).
	bool TileCullWillRun() const {
		if (!m_passes.forwardPlusCull)
			return false;
		if (!DsActive())
			return true;
		return (DecalsActive() && m_decalFrameCount > 0)
			|| ProbeTileListsWanted() || (m_giVolumesLoaded && m_giVolumesEnabled);
	}
	void LoadGiVolumes();
	// Degrades single-source ambient modes whose source did not load
	// (called once after all three GI loaders at scene init): keeps the
	// isolation states meaningful instead of silently empty.
	void DegradeAmbientForMissingSources();
#if defined(DS_DEBUG_VIZ)
	// ---- Deferred-shadow debug visualizers (paper 5.3/5.4; debug builds
	// only). Mode/light/overlay ride the per-flight DsVizParams ring
	// (FrameUniforms::dsVizParams); the light index refers to the SORTED
	// light buffer (phase 1 sorts by near distance AFTER the spot-shadow
	// selection, so it differs from the ImGui overlay's m_debugLights
	// order). ----
	// near distance AFTER the spot-shadow selection, so it differs from
	// the ImGui overlay's m_debugLights order). ----
	DsDebugViz m_dsDebugMode = DsDebugViz::Off;
	int m_dsDebugLight = 0;             // selected light (sorted index)
	bool m_dsDebugOverlay = true;       // blend over lit (vs flat replace)
	int m_dsDebugCountCeil = 32;         // count-heat ceiling (lights)
	uint32_t m_dsLightCount = 0;        // sorted lights this frame (UI clamp)
	// Selected light captured post-sort for the ImGui marker (sorted order).
	glm::vec3 m_dsDebugLightPos{ 0.0f };
	glm::vec3 m_dsDebugLightColor{ 1.0f };
	int m_dsDebugLightType = 0;         // 0=point, 1=spot
	float m_dsDebugLightRange = 0.0f;
	bool m_dsDebugLightValid = false;
#endif
	// Retired-slice counter readback for the debug overlay (list/vis slot
	// peaks, ~2 frames old — same lag as the exposure meter). Includes
	// the phase 3/4 classification histogram (telemetry).
	struct DsStats {
		uint32_t listSlots = 0;
		uint32_t visSlots = 0;
		uint32_t clsAllLit = 0;
		uint32_t clsAllShadow = 0;
		uint32_t clsPenumbra = 0;
		uint32_t sumEarly = 0;
		uint32_t sumFinal = 0;
		uint32_t dbgSpotCone = 0;
		uint32_t dbgSpotClip = 0;
	};
	DsStats m_dsStats{};
	std::vector<uint32_t> m_dsZBinStaging;  // 4 * kDsZBinCount
	std::vector<uint32_t> m_dsMaskStaging;  // per-TileGroup mask entries
	// Light count captured at phase-1 upload time (telemetry reference
	// for the async counter readback; see UpdateReadbacks).
	uint32_t m_dsReadbackLightCount = 0;
	// ---- Motion vector pass: previous-frame state + debug view ----
	glm::mat4 m_prevViewProj{ 1.0f }; // last frame's viewProj (post Y-flip)
	// This frame's viewProj WITHOUT the SMAA T2x subpixel jitter (post
	// Y-flip). 2D ImGui overlays (light markers/labels) must project with
	// this: the UI layer is not jittered or temporally resolved, so the
	// jittered matrix would make every marker buzz +/-0.25 px per frame.
	glm::mat4 m_unjitteredViewProj{ 1.0f };
	// This frame's projection WITHOUT jitter (post Y-flip): DLSS wants the
	// clip transform unjittered (cameraViewToClip) — jitter travels as the
	// separate pixel-space offset, never inside the matrices.
	glm::mat4 m_unjitteredProj{ 1.0f };
	// Last frame's m_unjitteredViewProj (same validity as m_prevViewProj):
	// DLSS clipToPrevClip must reproduce the motion-vector pass' camera
	// history without the per-frame jitter.
	glm::mat4 m_prevUnjitteredViewProj{ 1.0f };
	bool m_prevViewProjValid = false; // false until the first frame completes
	// ---- Temporal jitter bookkeeping (SMAA T2x / DLSS) ----
	// The motion vectors MUST carry the jitter (prevViewProj is jittered)
	// so SMAA T2x / DLSS reproject exactly. Non-temporal consumers (motion
	// blur, the MV debug view, and SMAA's history-attenuation metric) get
	// the known jitter component of the vectors to subtract:
	//   m_currJitterUV  - this record's sample-position offset (UV)
	//   m_prevJitterUV  - the previous record's (snapshotted with prevViewProj)
	//   m_jitterDeltaUV - (prev - curr): the jitter's contribution to the
	//                     MV texture (prevUV - currUV); zero when still
	//                     warming up (prevViewProj == viewProj) or no jitter.
	glm::vec2 m_currJitterUV{ 0.0f };
	glm::vec2 m_prevJitterUV{ 0.0f };
	glm::vec2 m_jitterDeltaUV{ 0.0f };
	std::vector<glm::mat4> m_prevGlobalTransforms; // last frame's node transforms
	bool m_showMotionDebug = false; // tonemap debug view: motion vectors
	float m_motionDebugScale = 16.0f; // debug view velocity amplification
	// ---- Motion blur pass (HDR velocity gather, see motionblur.h) ----
	bool m_motionBlurEnabled = true; // runs after Main, before Bloom/Tonemap (M)
	int m_motionBlurSamples = 12; // gather taps 1..32
	float m_motionBlurMaxRadius = 24.0f; // blur clamp (px)
	float m_motionBlurStrength = 1.0f; // shutter scale
	bool m_motionBlurTileDilation = true; // CoD:AW tile max + 5x5 dilate prepasses
	// ---- Depth-of-field pass (HDR, post-motion-blur, pre-bloom) ----
	bool m_dofEnabled = false; // opt-in: off keeps the reference sharp look
	bool m_dofHqBokeh = false; // false = low single-pass Poisson, true = CoC + 48-tap hex bokeh
	float m_dofFocusDistance = 8.0f; // in-focus plane (m, view distance)
	float m_dofFocusRange = 2.0f; // sharp half-depth around focus (m)
	float m_dofMaxRadius = 12.0f; // low-pass clamp (px)
	float m_dofBokehMaxRadius = 24.0f; // HQ bokeh clamp (px)
	float m_dofHighlightBoost = 1.5f; // HQ bright-tap weight multiplier
	float m_dofHighlightThreshold = 1.0f; // HQ pre-exposure luminance knee
	// ---- Screen-space reflections (HDR composite, post-DOF, pre-bloom) ----
	// Receiver materials come from the THIN G-BUFFER (full-res RGBA8 oct
	// normal + roughness + metal, written by the Forward+ prepass when
	// SSR OR AO wants it — see fc.ssrGbufferWanted). Every depth read in
	// the blur/composite goes through the SHARED final-depth MAX-HZB
	// (smvk::Ssgi::hzb, built by the SSGI block after Main + particles:
	// mip0 = the exact 1:1 depth copy, mip1 = nearest-of-2x2 block reps);
	// the pyramid is a hard requirement now — frames without a built
	// pyramid skip SSR entirely (the old linear-march fallback died with
	// the depth-load blur). The pane/glass A/B layer is gone (the pane
	// pass was removed long before the SSGI rework; its knob + dead
	// program went with it).
	// Bistro shopfront glass at street grazing angles is the calibration
	// case (default Bistro view looks down the road at it). Presented in
	// the UI as SSGI reflections (it rides the SSGI block's shared HZB);
	// the builder/code tokens stay SSR. Default ON.
	bool m_ssrEnabled = true;
	int m_ssrSteps = 32; // raymarch steps 1..128 (half-res trace)
	int m_ssrRefineSteps = 4; // binary refine iterations 0..8
	float m_ssrMaxDistance = 50.0f; // ray length (m)
	float m_ssrThickness = 0.3f; // depth acceptance band (m)
	float m_ssrIntensity = 1.0f; // reflection strength
	float m_ssrF0 = 0.04f; // dielectric base reflectance (glass)
	float m_ssrBlurSharpness = 10.0f; // bilateral depth edge weight (higher = tighter)
	// Blur filter (see smvk::SsrBlurMode): 0 = the original 2D bilateral
	// (quality baseline), 1 = the separable H+V candidate (same bilateral
	// weights as two 1D passes through the ssrBlurTemp intermediate).
	// Per-recording — no target rebuild on a flip; the temporal history
	// invalidates instead (m_ssrLastBlurMode below).
	int m_ssrBlurMode = 0;
	int m_ssrLastBlurMode = 0; // live-switch detector -> SSR TAA history reset
	// ---- Debug material override (the SSR blur-bucket test rig) ----
	// Overwrites EVERY uploaded material's metallic/roughness (UpdateMaterials:
	// params.y/z + the metallicRoughness texture slot disabled so the
	// slider IS the final value). One knob re-skins the whole scene — the
	// lit path, the thin G-buffer and the SSR blur radius buckets
	// (metal + 0.35/0.7 roughness thresholds) all read the same table.
	bool m_debugMaterialOverride = false;
	float m_debugMaterialRoughness = 0.5f;
	float m_debugMaterialMetallic = 0.0f;
	// ---- SSR temporal filter (TAA of the reflection layer only) ----
	// Motion-vector-reprojected accumulation of the half-res reflection
	// layer (see SsrTaaState in ssr.h). The base HDR is never filtered.
	// Needs the motion-vector target; without it (or on invalid history)
	// the record falls back to the spatial-only path.
	bool m_ssrTaaEnabled = true; // temporal accumulation of the SSR layer
	float m_ssrTaaBlend = 0.9f; // history weight 0..0.98 (attenuated by motion)
	// Record-sequence state (same discipline as m_smaaT2x): parity picks
	// the history slot the next temporal record writes; invalidated on
	// target rebuild, SSR/motion loss and AA-mode changes (jitter shift).
	smvk::SsrTaaState m_ssrTaa;
	// True when this record emitted the temporal chain (end-step input).
	bool m_ssrFrameRanTemporal = false;
	// Requested = UI toggle only; the emission gate additionally needs
	// valid motion vectors (checked at the SSR site, like SmaaActive).
	bool SsrTaaRequested() const { return m_ssrTaaEnabled; }
	// ---- Raymarched volumetric fog lit by the SG-GI probes (see
	// volumetric.h). Single-scatter HDR composite after SSR: extinction +
	// probe-ambient in-scatter + HG sun scatter. Off by default (opt-in;
	// zero density/intensity is a copy-through). Quality preset drives
	// march steps (full-res composite; hand edits flip to Custom). ----
	bool m_volumetricEnabled = false;
	smvk::VolumetricQuality m_volumetricQuality = smvk::VolumetricQuality::Medium;
	int m_volumetricSteps = 16; // march steps 4..128
	float m_volumetricMaxDistance = 80.0f; // ray clamp (m)
	float m_volumetricDensity = 0.015f; // extinction (1/m)
	float m_volumetricIntensity = 1.0f; // scattering multiplier
	float m_volumetricAnisotropy = 0.4f; // HG g -0.9..0.9
	float m_volumetricSun = 1.0f; // sun scatter strength
	float m_volumetricGi = 1.0f; // probe ambient strength
	bool m_volumetricUseGi = true; // sample SG-GI probes (else sky-IBL fallback)
	bool m_volumetricUseSun = true; // analytic sun single-scatter
	// Fog-GI quality switch: GI volumes stack into the fog ambient
	// (default, matches the lit path). Off = octree/sky only — the fog
	// bypasses the per-refresh volume walk entirely (A/B knob for heavy
	// fog configs; UI checkbox + TOURMALINE_VOLUMETRIC_GI_VOLUMES=0 headless).
	bool m_volumetricUseGiVolumes = true;
	float m_volumetricJitter = 1.0f; // IGN dither 0..1 (banding guard)
	float m_volumetricHeightFalloff = 0.0f; // extra height falloff (1/m, 0=off)
	float m_volumetricHeightBase = 0.0f; // reference height (m)
	// ---- World voxel fields (Engine/WorldFields — the atlas-backed
	// virtual world volume system). World-space (NOT viewport-scoped):
	// follows the frame's camera (PrepareFrame's interest point),
	// publishes its field/page-table BDA into FrameUniforms::worldFields,
	// and any pass samples it through the shared world_field.hlsli. One
	// infrastructure, independent fields (fog density, wind, ...): the
	// field set registers ONCE in onAttach (InitWorldFields — ids are
	// registration order, fog first per WORLD_FIELD_ID_*). v1 is gated
	// by TOURMALINE_WORLD_FIELDS=1 (the checkerboard shakedown field +
	// budgeted pools; fog/wind register with their consumers).
	// ---- The WorldPartition (Engine/WorldPartition) drives it: the
	// frame builds a StreamingInterest from the camera (position +
	// velocity), the partition runs the provider sequence, and the
	// WorldFieldProvider maps CommitFrame onto the fields' publication.
	// Brick generation runs on a small DEDICATED work pool (async — the
	// pool's ring is ours alone per the enqueue/backpressure contract). ----
	engine::worldfields::WorldFieldSystem m_worldFields;
	engine::worldfields::WorldFieldProvider m_worldFieldProvider{ m_worldFields };
	engine::worldpartition::WorldPartition m_worldPartition;
	std::unique_ptr<smvk::ThreadPool> m_worldFieldWorkPool;
	glm::vec3 m_worldFieldPrevEye{ 0.0f };
	bool m_worldFieldPrevEyeValid = false;
	// ---- Artistic LUT grading (TPP-style 32^3 strip, see tonemap.h) ----
	// content/luts/*.png: 1024x32 sRGB-encoded strips. F7 exports the
	// neutral strip + a screenshot reference into exports/ for Photoshop.
	smvk::Holder<smvk::TextureHandle> m_lutTexture;
	uint32_t m_lutBindless = UINT32_MAX;
	std::string m_lutFile; // "" = off
	float m_lutIntensity = 1.0f;
	std::vector<std::string> m_lutFiles;
	// Screenshot capture: the tonemap pass writes one extra RGBA8_SRGB
	// target on the capture frame; the bytes read back to PNG after Execute.
	smvk::Holder<smvk::ImageHandle> m_captureImage;
	smvk::Holder<smvk::ImageViewHandle> m_captureView;
	smvk::LogicalImageViewHandle m_captureLogical{};
	smvk::Extent2D m_captureExtent{ 0, 0 };
	// INLINE OUTPUT (see the SetInlineOutput contract): the private
	// LDR target + its world-graph logical view (the tonemap writes it)
	// + the context-global bindless registration (the HOST's composite
	// samples it). Creation/retire follows the capture pattern but with
	// the deferred-retire swap (in-flight frames keep the old objects —
	// see EnsureInlineTarget).
	bool m_inlineOutput = false;
	smvk::Extent2D m_inlineTargetExtent{ 0, 0 };
	smvk::Holder<smvk::ImageHandle> m_inlineTargetImage;
	smvk::Holder<smvk::ImageViewHandle> m_inlineTargetView;
	smvk::Holder<smvk::BindlessImageHandle> m_inlineTargetBindless;
	uint32_t m_inlineTargetBindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle m_inlineTargetLogical{};
	// The RELEASE scratch: a 1x1 color target the inline-mode tail
	// composite writes into. The pass exists for its SIDE effect —
	// SetInputImages on the inline target records the
	// COLOR_ATTACHMENT -> SHADER_READ transition, leaving the target in
	// exactly the layout the HOST's ImGui descriptor declares (the
	// bindless registration only records the expected layout; nothing
	// else transitions a per-frame-written image for off-graph
	// sampling).
	smvk::Holder<smvk::ImageHandle> m_inlineScratchImage;
	smvk::Holder<smvk::ImageViewHandle> m_inlineScratchView;
	smvk::LogicalImageViewHandle m_inlineScratchLogical{};
	bool EnsureInlineTarget(smvk::Extent2D extent);
	bool m_captureQueued = false; // emit the capture pass this frame
	bool m_captureReadbackPending = false; // read back after the submit (DrawFrame)

	void ScanLutFiles();
	bool LoadLut(const std::string& file);
	void ExportNeutralLut();
	void EnsureCaptureTarget(smvk::Extent2D extent);
	void ExportScreenshot(smvk::Extent2D extent);
	// ---- SMAA post-process antialiasing (see Engine/Rendergraph/smaa.h) ----
	// AA mode combo drives everything (MSAA 2x only runs in its own mode):
	// Off / Msaa2x / Smaa1x / SmaaT2x. T2x adds a quarter-pixel alternating
	// camera jitter (UpdateFrameUniforms) and the temporal resolve.
	AaMode m_aaMode = AaMode::SmaaT2x;
	// Texture filtering (trilinear <-> aniso 16x, UI toggle + shortcut).
	// Default trilinear (linear-between-mips); applied in-place to the
	// bindless texture samplers (see ApplyTextureFiltering).
	TextureFiltering m_textureFiltering = TextureFiltering::Trilinear;
	TextureFiltering GetTextureFiltering() const { return m_textureFiltering; }
	void SetTextureFiltering(TextureFiltering mode);
	// (Re)applies m_textureFiltering to sampler slots 0 (Repeat) + 2
	// (Clamp/cubemap). Safe to call every frame (no-op when already set);
	// call after sampler creation and whenever the UI changes the mode.
	void ApplyTextureFiltering();
	// Opt-in alpha-to-coverage for Masked cutout in MSAA 2x Main
	// (TOURMALINE_A2C=1, default off). Bench data (bistro foliage view,
	// RTX 5070 Ti): A2C moves the masked coverage/depth test after the FS
	// (coverage needs the output alpha), so overlapping foliage layers
	// can no longer early-reject — +6ms vs the demote-discard path. Keep
	// it as a quality mode: foliage silhouettes resolve AA'd in MSAA.
	bool m_msaaAlphaToCoverage = false;
	// Global T2x toggle (see SmaaT2xState in smaa.h). The phase belongs to
	// the recorded-frame sequence, not the swapchain image: every T2x
	// record writes history[parity] and blends against history[parity^1]
	// (the immediately preceding record, matching the global prevViewProj
	// the motion pass diffs against). Under VSync FIFO this displays
	// strict P0,P1,P0,P1; Mailbox/Immediate drops only skip a phase on
	// screen while each resolve stays a correct consecutive-record blend.
	// Invalidated on rebuild and on any AA-mode change (history targets
	// recreated / stale data from an older T2x run).
	smvk::SmaaT2xState m_smaaT2x;
	// Last AA mode seen by RenderFrame; any change invalidates the history
	// (covers T2x -> 1x/Off -> T2x round-trips reusing ancient data).
	AaMode m_smaaLastMode = AaMode::SmaaT2x;
	// This record's snapshot (filled at the top of RenderFrame so
	// UpdateFrameUniforms, the pass builders and the overlay all agree).
	smvk::SmaaFrameParams m_smaaFrameParams{};
	// True when this record emitted the T2x chain (end-step input).
	bool m_smaaFrameRanT2x = false;

	// ---- DLSS Super Resolution (Streamline, see Engine/Rendergraph/dlss.h) ----
	// Off by default; when active it OWNS the internal resolution (DLSS
	// optimal render size) and the upscale (HDR render-res -> HDR swap-res
	// before tonemap), and it bypasses the AA modes (its own AA replaces
	// them; MSAA would fight the reduced-resolution input).
	smvk::DlssMode m_dlssMode = smvk::DlssMode::Off;
	// One-time capability check (slIsFeatureSupported on this adapter).
	bool m_dlssSupported = false;
	// This record's snapshot: filled after the target Ensure block so the
	// jitter (UpdateFrameUniforms), the MSAA gate and the tonemap retarget
	// all agree with the pass-emission gate below.
	bool m_dlssFrameActive = false;
	// History reset request: set on deactivation/mode/extent change, held
	// until the next DLSS record consumes it (Constants::reset).
	bool m_dlssReset = true;
	// Halton jitter phase (record cadence like the T2x parity, not
	// per-image) + this record's snapshot of the jitter in render pixels.
	uint32_t m_dlssPhase = 0;
	uint32_t m_dlssPhaseCount = 8;
	smvk::DlssJitter m_dlssJitter{};
	// slDLSSSetOptions drift tracking (re-applied on mode/output change;
	// slFreeResources on deactivate — see RenderFrame's DLSS block).
	bool m_dlssOptionsApplied = false;
	smvk::DlssMode m_dlssOptionsMode = smvk::DlssMode::Off;
	uint32_t m_dlssOptionsW = 0;
	uint32_t m_dlssOptionsH = 0;
	// Reset bookkeeping: extents/mode the last DLSS record saw.
	smvk::DlssMode m_dlssLastMode = smvk::DlssMode::Off;
	smvk::Extent2D m_dlssLastRenderExtent{ 0, 0 };
	smvk::Extent2D m_dlssLastSwapExtent{ 0, 0 };

	bool DlssRequested() const { return m_dlssMode != smvk::DlssMode::Off; }
	// Hard requirements: Streamline alive + adapter support + motion
	// vectors (camera-motion input) + valid upscaler targets.
	bool DlssActive() const {
		return DlssRequested() && smvk::DlssAvailable() && m_dlssSupported
			&& m_passes.motionVectors
			&& m_viewportResources.motionVectors.Valid()
			&& m_viewportResources.dlss.Valid();
	}

	bool SmaaRequested() const {
		return m_passes.smaa && !m_dlssFrameActive
			&& (m_aaMode == AaMode::Smaa1x || m_aaMode == AaMode::SmaaT2x);
	}
	// Motion vectors are a hard requirement (the neighborhood pass packs
	// velocity into alpha even in 1x mode), and so are the LUTs + programs.
	bool SmaaActive() const {
		return SmaaRequested() && m_viewportResources.smaa.Valid()
			&& m_passes.motionVectors && m_viewportResources.motionVectors.Valid();
	}
	bool SmaaT2xActive() const {
		return SmaaActive() && m_aaMode == AaMode::SmaaT2x;
	}
	// UNORM sibling of the swapchain surface format. Used by the SMAA LDR
	// chain and the UI target: both hold gamma-encoded bytes that must be
	// stored raw (an _SRGB target would double-encode them on store); the
	// channel order matches the swapchain so the decode-on-write (smaa.h /
	// ui_composite) restores the tonemapper's values. Exotic surface
	// formats fall back to BGRA8.
	smvk::Format LdrUnormSibling() const {
		const smvk::Format surface = m_swapchain
			? m_swapchain->GetSurfaceFormat() : smvk::Format::Undefined;
		if (surface == smvk::Format::BGRA8_SRGB || surface == smvk::Format::BGRA8_UNorm)
			return smvk::Format::BGRA8_UNorm;
		if (surface == smvk::Format::RGBA8_SRGB || surface == smvk::Format::RGBA8_UNorm)
			return smvk::Format::RGBA8_UNorm;
		return smvk::Format::BGRA8_UNorm;
	}
	smvk::Format LdrUnormSibling(const smvk::Framebuffer& fb) const {
		const smvk::Format surface = FrameSurfaceFormat(fb);
		if (surface == smvk::Format::BGRA8_SRGB || surface == smvk::Format::BGRA8_UNorm)
			return smvk::Format::BGRA8_UNorm;
		if (surface == smvk::Format::RGBA8_SRGB || surface == smvk::Format::RGBA8_UNorm)
			return smvk::Format::RGBA8_UNorm;
		return smvk::Format::BGRA8_UNorm;
	}
	// True when the chosen surface format is hardware-sRGB: swap-writing
	// passes then output linear (the store encodes); manual encode/decode
	// is keyed off this for the UNORM fallbacks.
	bool SwapchainIsSrgb() const {
		if (!m_swapchain)
			return false;
		const smvk::Format surface = m_swapchain->GetSurfaceFormat();
		return surface == smvk::Format::BGRA8_SRGB || surface == smvk::Format::RGBA8_SRGB;
	}
	bool FrameSwapIsSrgb(const smvk::Framebuffer& fb) const {
		const smvk::Format surface = FrameSurfaceFormat(fb);
		return surface == smvk::Format::BGRA8_SRGB || surface == smvk::Format::RGBA8_SRGB;
	}
	// ---- Auto-exposure (histogram meter + CPU eye adaptation, see
	// exposure.h). Meters the (possibly motion-blurred) scene HDR before
	// Bloom/Tonemap; the smoothed exposure feeds the tonemap push next
	// frame (1-frame lag) and scales the bloom threshold/knee so bloom
	// strength stays constant as exposure shifts.
	bool m_autoExposureEnabled = true;
	// Bench determinism pin (survives the scene seed — applied in
	// ResolveSceneLights): --bench pins fixed manual exposure,
	// but the per-scene PushPostfxEnv seed re-seeds m_autoExposureEnabled
	// from the authored env, silently re-enabling live eye adaptation
	// mid-bench and breaking capture comparability (only
	// TOURMALINE_AUTO_EXPOSURE re-pinned before). The pin re-applies after
	// every seed; -1 = no pin. The X key / UI checkbox stay live
	// afterwards (the pin applies only at load/rebind; a bench never
	// rebinds, so "X toggles it back live" keeps working).
	int m_autoExposurePin = -1; // -1 none, 0 pinned off, 1 pinned on
	// Headless-pin snapshot (layer 2 of the authored-content layering, see
	// Engine/App/RunConfig.h): stored once at Configure, re-applied by
	// ApplyHeadless*Overrides after every authored scene seed. Never
	// re-reads the environment, so pins are stable for the whole run and
	// the UI stays live after each application.
	RunConfig m_headlessPins{};
	float m_manualExposure = 0.001f; // used when auto is off (== TONEMAP_EXPOSURE)
	float m_exposureAdapted = 0.001f; // smoothed exposure (seeded to manual)
	// Seconds accumulated since the last Adapt step. The adapt only runs
	// when an async histogram poll COMPLETES (every ~4-8 frames at the
	// every-4th-frame submit cadence), so passing a single frame's
	// delta_time_s made the effective convergence ~4-8x slower than the
	// "/s" the UI labels promise. Accumulate per frame, clamp at use, and
	// reset on every adapt — the labeled rates are now the real rates.
	float m_exposureAdaptDt = 0.0f;
	float m_exposureTarget = 0.001f; // last computed target (debug readout)
	float m_exposureEvBias = 0.0f; // user EV trim (-4..+4)
	float m_exposureSpeedToDark = 1.0f; // adapt rate when exposure rises (/s)
	float m_exposureSpeedToBright = 3.0f; // adapt rate when exposure falls (/s)
	float m_exposureMin = 0.00001f; // absolute clamp (dark interior safe)
	float m_exposureMax = 0.05f; // absolute clamp (night-sky safe)
	// CPU copy of the last histogram for the debug plot (filled by the
	// post-Execute readback; floats for ImGui::PlotHistogram).
	float m_exposureBinsUi[smvk::kExposureBinCount] = {};
	uint32_t m_exposureSamples = 0; // metered pixels last frame
	// Flight slice this frame's histogram lives in (recorded in
	// RenderFrame; the download after Execute reads the same slice).
	uint32_t m_exposureFrame = 0;
	// Effective tonemap exposure: adapted when auto is on, the pass is in
	// the set and the meter is live; manual otherwise.
	float EffectiveExposure() const {
		const bool active = m_autoExposureEnabled && m_passes.exposure &&
			m_viewportResources.exposure.Valid();
		return active ? m_exposureAdapted : m_manualExposure;
	}
	// Fixed reference exposure for the debug views (F2..F6). Debug colors
	// are authored in display space (the flat palette pre-scales below and
	// the unlit/cascade/vertex-color FS divide by the same value), and
	// metering is frozen for them anyway (ExposureMeteringActive holds
	// the last scene exposure). Rendering at a FIXED exposure instead of
	// the held one keeps debug frames deterministic and the authored
	// colors independent of the scene's adaptation state. Mirrors
	// TONEMAP_EXPOSURE (world_renderer.cpp) — keep in sync.
	static constexpr float kDebugViewExposure = 0.001f;
	// Exposure actually applied this frame: the adapted/manual value in
	// the Lit view, the fixed debug reference otherwise. Shading
	// (FrameUniforms.exposure), the flat debug gains AND the tonemap pass
	// all read this, so the 1/exposure pre-scale cancels end-to-end.
	float FrameExposure() const {
		return (m_debugView == ViewMode::Lit)
			? EffectiveExposure() : kDebugViewExposure;
	}
	// True when a debug visualization REPLACES the scene pixels this
	// record: any non-Lit ViewMode (F2..F11 flat programs), the GI
	// baked debug views (R/F8) or a DS visualizer (Y, DS_DEBUG_VIZ).
	// Scene post-fx that composites ON TOP of those pixels — SSR
	// reflections (additive), bloom (additive), volumetric fog (Beer-Lambert throughput +
	// scatter) and the SSR-only receiver-material layer (prepass
	// stencil + PaneMask) — stays off while true,
	// same discipline as the metering freeze in ExposureMeteringActive:
	// compositing over a flat readout pollutes it (reflections of the
	// palette itself, fog-faded heat colors) the way metering it would
	// poison the exposure.
	bool DebugVisualizationActive() const;
	// ---- Bloom chain (HDR down/up, see bloom.h) ----
	bool m_bloomEnabled = true; // runs before Tonemap; gated with m_passes.bloom (B)
	float m_bloomIntensity = 0.6f; // tonemap bloom-add scale (== BLOOM_INTENSITY)
	float m_bloomThreshold = 800.0f; // pre-exposure HDR at the manual-exposure reference (== BLOOM_THRESHOLD; knee tracks at 0.25x; exposure-aware scaling inverts to 1/exposure)
	// ---- Tonemap operator (see Engine/Rendergraph/tonemap.h + tonemap.frag.hlsl) ----
	// 0=Reinhard, 1=ACES (default), 2=Uncharted2, 3=GT7. ImGui combo in the
	// Exposure section; applied per-frame onto the tonemapDesc push.
	uint32_t m_tonemapMode = smvk::kTonemapGT7;
	// ---- AO (the SSGI post chain — Engine/Rendergraph/ssgi.h; GTAO is
	// deprecated and removed). Runs AFTER Main (Main exports the
	// AO-sensitive lighting through an aoTerm MRT target, the compose
	// subtracts aoTerm*(1-aoEff) from the HDR — no temporal lag, Main
	// never samples AO). O cycles No AO -> SSAO -> SAO. ----
	enum class AoMode : int { None = 0, Ssao = 1, Sao = 2 };
	AoMode m_aoMode = AoMode::Sao;
	float m_ssaoRadius = 0.5f; // view-space sample radius (m, shared SSAO/SAO)
	float m_ssaoBias = 0.025f; // self-occlusion / horizon bias
	float m_ssaoIntensity = 1.0f; // compose-side aoEff lerp (was default_lit's hot.aoIntensity)
	float m_ssaoMaxDistance = 25.0f; // fade-out distance (m) + the .g encode range
	int m_ssaoSamples = 16; // SSAO hemisphere taps 1..32
	float m_ssaoBlurSharpness = 8.0f; // blur depth edge weight
	int m_saoSamples = 11; // SAO spiral taps 1..32
	int m_saoSpiralTurns = 7; // SAO extra spiral twist 1..16
	// Compat: SSAO on/off maps onto AoMode (None vs Ssao); SAO counts as on.
	[[nodiscard]] bool SsaoEnabled() const { return m_aoMode != AoMode::None; }
	// ---- Diffuse screen-space GI (Engine/Rendergraph/ssgi_diffuse.h —
	// one cosine-hemisphere bounce at half res, the energy-conserving
	// REPLACEMENT of the diffuse ambient slice, NOT an additive stack).
	// Independent of SSR (specular stays SSR's) and of AO (the baseline
	// sourcing switches with it). Default OFF: experimental v1 — enable
	// via the render-settings checkbox or TOURMALINE_SSGI=1. Gated off
	// under MSAA (no stable single-sample G-buffer companion) and under
	// the flat debug views (the Ssgi* compose views keep it live). ----
	bool m_ssgiDiffuseEnabled = false;
	float m_ssgiDiffuseIntensity = 1.0f;  // s: the replacement strength (0 = off)
	float m_ssgiDiffuseMaxDistance = 5.0f; // ray length budget (m) — the LOCAL bounce
	float m_ssgiDiffuseThickness = 0.3f;  // depth acceptance band (m)
	float m_ssgiDiffuseTemporalBlend = 0.9f; // history weight 0..0.98
	float m_ssgiDiffuseBlurSharpness = 10.0f; // bilateral depth edge weight
	int m_ssgiDiffuseMaxIntersections = 64;   // traversal cap (exhaustion = miss)
	bool m_ssgiDiffuseTaaEnabled = true;  // temporal accumulation of the GI layer
	// Record-sequence state (the SsrTaaState discipline): parity picks
	// the history slot the next temporal record writes; invalidated on
	// target rebuild and motion-vector loss.
	smvk::SsgiDiffuseTaaState m_ssgiDiffuseTaa;
	// True when this record emitted the temporal chain (end-step input).
	bool m_ssgiDiffuseFrameRanTemporal = false;
	// ---- Forward+ light debug overlay (drawn in the ImGui UI pass) ----
	// CPU copy of the last light set (positions/colors/types), projected
	// through the frame viewProj and drawn as points + buffer indices.
	struct DebugLight {
		glm::vec3 position{};
		glm::vec3 color{}; // linear RGB
		int type = 0;      // 0=point, 1=spot (kForwardPlusPoint/Spot)
	};
	std::vector<DebugLight> m_debugLights;
	// Skydome visibility (K): a debug visibility toggle like wireframe /
	// hide-transparent — NOT environment state (the renderer owns no
	// environment; it renders the world's scene env). The pass-set flag
	// m_passes.skydome stays the pipeline-shape switch.
	bool m_skyEnabled = true;
	smvk::Skydome m_skydome{};
	// Tiled terrain heightfield (baked G-blocks from the world's TerrainSource;
	// see Rendergraph/terrain.h): 256 m blocks x 4 LODs rendered in the
	// Main pass right after the sky. m_passes.terrain is the pipeline
	// shape switch; m_terrainEnabled is the debug visibility toggle (like
	// the skydome's K). OFF by default (m_passes.terrain=false).
	bool m_terrainEnabled = true;
	smvk::terrain::TerrainField m_terrain{};
	// Heightfield shadow mode/steps for the terrain frag tail (RunConfig
	// snapshot at Configure: TOURMALINE_TERRAIN_HFSHADOW[_STEPS]).
	int m_terrainHfShadow = 1;
	int m_terrainHfSteps = 16;
	// World env tiles (Engine/WorldPartition/WorldEnvTiles.h — the runtime
	// rework of the old OBB-volume zone system): the zone tables BAKE onto
	// the 64 m tile grid whenever they change (a cheap per-frame HASH, not
	// a per-frame pack) and the terrain/volumetric shaders resolve a zone
	// by TILE ADDRESS. ALWAYS ON: the base record ([0], the scene's DEFAULT
	// environment, the old zone-zero) is the weight-1 fallback everywhere
	// (out-of-grid included). The old runtime toggle (m_envZonesEnabled)
	// and the authored Scene::envZonesEnabled master switch are retired —
	// the field stays on the tour/pak wire (forced true at every read) for
	// format compatibility.
	engine::worldpartition::WorldEnvTiles m_envTiles;
	uint64_t m_envTilesDirtyHash = 0; // the zone-table fingerprint
	bool m_envTilesPublished = false; // buffers exist + built
	uint32_t m_envTileZoneCount = UINT32_MAX; // overflow log dedupe
	size_t m_envTileCellBytes = 0; // the cells buffer size (recreate key)
	// Hashes the zone tables + transforms (the bake inputs) and rebuilds +
	// re-uploads the tiles on change (see UpdateEnvTiles — called before
	// UpdateFrameUniforms).
	void UpdateEnvTiles(uint32_t currentFrame);
	// ---- Texture inspector (aux-texture debug view) ----
	// A SEPARATE floating ImGui window (not part of the debug panel — it
	// only opens on the keybind::TEXTURE_VIEWER_TOGGLE shortcut, backtick),
	// so the panel stays lean and the list/preview never cost panel space.
	// Lists the LIVE textures (ResourceManager::ListTextureInfos — one
	// row per pool texture, no content filter: every texture is one click
	// away) and previews a clicked one through the ImGui OVERLAY's
	// user-texture bindings (AddUserTexture / AddUserTextureView — the
	// backend's descriptor registration wrapped behind RHI-opaque types;
	// NO Vulkan detail crosses into this renderer). Plain 2D previews
	// directly; CUBEMAPS get six per-face View2D bindings (+X -X +Y -Y
	// +Z -Z, shown as a 3x2 grid — the ImGui backend samples Texture2D
	// only, so the cube's own default view is unusable as a preview).
	// Staleness is a FEATURE: a selected handle dropping out of the
	// listing (destroyed — the pool generation bumped, exactly what
	// UpdateTexture's recreate path does) drops the preview and the face
	// views instead of sampling a dead view.
	bool m_texDebugWindow = false;           // backtick opens/closes
	// VRAM usage viewer (Ctrl+M): floating tool window listing the GPU
	// memory heaps — sizes + VMA budgets/usage per heap (see
	// RefreshGpuMemoryStats / GpuMemoryHeap). No owned GPU state; pure
	// read of m_gpuMemHeaps.
	bool m_vramWindow = false;               // Ctrl+M opens/closes
	// World Map window (C): floating top-down orthographic canvas over
	// the world XZ extent (north = -Z up). Background = the terrain
	// heightfield downsampled to a 256px preview when a terrain field
	// is loaded, else an empty grid. Overlays (each toggleable):
	// env-volume OBB footprints (+/-inf face arrows, priority colors),
	// GI volumes, GI + cubemap probes, lights/decals, camera wedge.
	// LATER layers slot in as one bool + one draw block each (shadow
	// cascade boxes, terrain LOD heat, env zone weights).
	bool m_worldMapWindow = false;           // C opens/closes
	bool m_worldMapEnvVolumes = true;
	bool m_worldMapGiVolumes = true;
	bool m_worldMapProbes = true;
	bool m_worldMapLights = true;
	bool m_worldMapCamera = true;
	// WorldFields layer (Engine/WorldFields): the fog field's clipmap
	// windows + resident/in-flight brick footprints per LOD, and a fog
	// DENSITY heat overlay sampled from the procedural reference at the
	// camera's height (cooked content replaces the reference function —
	// the overlay is a debug view of the SOURCE, the atlas shows
	// residency).
	bool m_worldMapFields = true;
	bool m_worldMapFogHeat = true;
	// WorldPartition layer: the streaming interest (position + velocity
	// lead) and the 256 m root-cell / 64 m subcell grid under its radius.
	bool m_worldMapCells = false;
	// Per-draw collection scratch (the WorldFields debug API, reused).
	std::vector<worldfields::WorldFieldSystem::WorldFieldDebugBrick>
		m_worldMapBrickScratch;
	glm::vec2 m_worldMapCenter{ 0.0f, 0.0f }; // world XZ, canvas center
	float m_worldMapScale = 0.0f; // px per meter; <= 0 = fit on next draw
	// Heightmap preview (built lazily from the baked terrain's CPU
	// heightfield; destroyed when the terrain goes away).
	smvk::Holder<smvk::TextureHandle> m_worldMapHeightTex{};
	ImTextureID m_worldMapHeightId = 0;
	uint32_t m_worldMapSampler = UINT32_MAX; // preview sampler (clamp/linear, created once)
	void DrawWorldMapWindow();
	void BuildWorldMapHeight();
	void DropWorldMapHeight();
	smvk::TextureHandle m_texDebugSelected{}; // empty = none selected
	ImTextureID m_texDebugPreview = 0;       // 2D: the overlay's user-texture binding (0 = none)
	// Cubemap selection: 6 per-face 2D views (Holder = auto-destroy on
	// reassign/reset) + their overlay bindings. Cube arrays (6*N layers)
	// preview cube 0 (faces 0..5).
	std::array<smvk::Holder<smvk::ImageViewHandle>, 6> m_texDebugCubeViews{};
	std::array<ImTextureID, 6> m_texDebugCubePreview{};
	uint32_t m_texDebugSampler = UINT32_MAX; // preview sampler (clamp/linear, created once)
	// Drops the preview binding(s) + selection (window closed / toggle
	// off): the overlay's retire is DEFERRED (RetireUserTexture — the
	// binding's descriptor set is referenced by the frame's ImGui draw
	// data + up to smvk::limits::MaxFramesInFlight in-flight submissions; the old
	// immediate free bound a freed set and faulted the device on the
	// X-close — the close-click frame's body still rendered the preview
	// before the drop); the face views' Holders reset (their VkImageViews
	// retire through the RMI destroy) so a dropped selection never
	// samples a dead view.
	void DropTexDebugSelection() {
		if (m_texDebugPreview != 0) {
			m_imgui->RetireUserTexture(m_texDebugPreview);
			m_texDebugPreview = 0;
		}
		for (ImTextureID& id : m_texDebugCubePreview) {
			if (id != 0) {
				m_imgui->RetireUserTexture(id);
				id = 0;
			}
		}
		for (auto& v : m_texDebugCubeViews)
			v = nullptr; // Holder reset -> DestroyImageView
		m_texDebugSelected = {};
	}
	// Selects + previews a listed texture (click OR arrow-key nav focus -
	// the shared body of both paths; see the viewer draw site). 2D gets
	// one binding; cubemaps get the six per-face views + bindings.
	void SelectTexDebugTexture(const smvk::TextureInfo& t);
	CameraPositioner_FirstPerson m_cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera m_camera = Camera(m_cameraPositioner);
	// (The virtual cursor for FPS mouselook moved to WorldRendererElement
	// with the camera drive.)
	// ---- Texture streaming (mip-tail residency, see
	// Engine/Renderer/texture_streamer.h). Owns residency for glTF-loaded
	// textures and for pak v4 digest textures (objects + tail registered
	// by the host's world source; digest-less pak blobs stay
	// full-resident, the pre-v4 behavior).
	bool m_textureStreamingEnabled = true;
	smvk::mem::LinearPtr<smvk::TextureStreamer> m_textureStreamer;
	// ---- Async readback ticket (exposure histogram + deferred-shadow
	// counters) ----
	// Submit + non-blocking poll each tick: the render thread NEVER waits
	// on the graphics-queue backlog (the old sync download locked for the
	// whole in-flight frame). One ticket in flight; while pending, new
	// ticks skip their submit (results land 1-2 frames later instead of
	// the old 2-3 — adaptation is time-damped, streaming is laggy anyway).
	uint64_t m_asyncReadbackTicket = 0;
	uint32_t m_pendingReadbackSlot = 0;
	bool m_pendingReadbackHasExposure = false;
	bool m_pendingReadbackHasDs = false;
	bool m_pendingReadbackHasGfxDemand = false;
	bool m_pendingReadbackHasGfxKept = false;
	std::array<uint32_t, smvk::kExposureBinCount> m_pendingExposureBins{};
	std::array<uint32_t, smvk::kDsCounterCount> m_pendingDsCounters{};
	// Gfx draw-level demand download destination (sized to the live slot
	// count at submit time; kDemandNoReport entries are skipped at poll).
	std::vector<uint32_t> m_pendingGfxDemand;
	// GPU-cull kept-count readback (the "0/797 culled" fix): the CPU stats
	// cannot see the GPU-mode cull (clustersCulled only counts the CPU-mode
	// path), so the filter's atomic filtered-offset counter word (zeroed by
	// the batch-cull dispatch, accumulated per KEPT entry - word
	// kCullStateCounterWord in the cullState slice) rides the same ticket:
	// 4 bytes, ~a few frames stale (retired-slice cadence), landed into
	// m_gfxKeptBatches for the HUD/panel culled computation.
	uint32_t m_pendingGfxKept = 0;
	uint32_t m_gfxKeptBatches = 0;
	bool m_gfxKeptValid = false;
	// TMP-DIAG (TOURMALINE_FILTERAUDIT=<frame>): one-shot async readback of
	// the gfx filter chain's per-frame state — the draw-cull verdict words
	// (cullState), the batch-cull kept bits (both parity regions' live
	// slot span), the filter's indirect commands and the batch headers —
	// for the LOD-sibling draw-drop hunt (distant sibling draws pushed,
	// never culled by any flag-gated stage, yet nothing rasterizes; the
	// data side is fully audited clean, so the kill is in the chain). The
	// poll prints per-draw verdict/kept/command tables to correlate with
	// the registration dump (gfx mesh -> uri). Remove later.
	uint64_t m_filterAuditTicket = 0;
	uint32_t m_filterAuditSlot = 0;
	uint32_t m_filterAuditBatchCount = 0;
	uint32_t m_filterAuditDrawCount = 0;
	std::vector<uint32_t> m_filterAuditVerdicts;   // kCullStateWords
	std::vector<uint32_t> m_filterAuditKept[2];   // per parity region, live slots
	std::vector<uint32_t> m_filterAuditIndirect;  // 5 words per slot
	std::vector<uint32_t> m_filterAuditBatches;   // 4 words per slot
	std::vector<uint32_t> m_filterAuditMeshes;    // drawConstants[].mesh snapshot
	// Streamer slot-meta epoch last uploaded into the gfx filter (0 = none
	// yet; register/unregister bumps it — retargets never do).
	uint64_t m_gfxMetaEpochUploaded = 0;
	// Last consume's reported-slot count (telemetry).
	uint32_t m_gfxDemandReported = 0;
	// CPU demand + budget tick. streamFrame must be monotonic (NOT the
	// swapchain slot); call once per RenderFrame after UpdateMaterials.
	void UpdateTextureStreaming(const FrameUniforms& frameData, smvk::Extent2D renderExtent);
	// Gfx-filter demand wiring (once per frame, before AppendPasses records
	// the filter): this frame's materials BDA slice + mip bias into the
	// gfx state, and the per-slot meta table when the streamer's epoch
	// moved (register/unregister only).
	void UpdateGfxTextureDemand(uint32_t frameIndex);
	// True when this frame may meter exposure (auto + pass + lit view).
	// Shared by the histogram download and the batched feedback pairing.
	bool ExposureMeteringActive() const;
	// Bins -> percentile target + damped adapt (tail of the async-poll
	// path; the old sync UpdateAutoExposure — a full-graphics-backlog
	// stall per frame — was replaced by UpdateReadbacks and removed).
	void AdaptExposureFromBins(const std::array<uint32_t, smvk::kExposureBinCount>& bins);
	// Combined GPU->CPU readback tick (replaces the lone UpdateAutoExposure
	// call): on GPU-feedback frames the histogram + feedback + DS-counter
	// downloads share ONE submit + wait; otherwise the existing single
	// download runs. Always clears the feedback slice afterwards
	// (download-before-clear).
	void UpdateReadbacks(uint32_t frameSlot, uint32_t streamFrame);
};

} // namespace engine
