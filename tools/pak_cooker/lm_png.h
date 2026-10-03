#pragma once
// lm_png: cook-time raw lightmap atlas debug dump — the baked atlas
// TEXELS as a PNG, written next to the .sglm (<scene>_atlas.png). This
// is the payload itself (lobe 0 amplitude, chunk rects at their packed
// atlas positions, the editor flat-view display mapping on the editor's
// clear color), NOT the chart-unwrapped geometry draw the editor exports.
// Comparing the two localizes lightmap faults:
//   cook PNG clean + editor PNG corrupt  -> stamping (chart uvs/ids/
//                                           transforms) or the editor
//                                           draw/export path;
//   cook PNG corrupt                     -> bake / denoise / pack.
//
// Cook-time only, part of PakCooker (stb_image_write from external/stb;
// the engine and App never see it).

#include <string>

#include <Engine/GI/lightmap.h>

namespace pakcook {

// Assembles atlas.width x atlas.height RGBA8 from the chunk payloads
// (chunks out of bounds or with a payload/lobeCount stride mismatch are
// skipped, not fatal — the dump stays useful on half-broken atlases)
// and writes it as a PNG. Returns false + err on IO failure.
bool WriteLightmapAtlasPng(const smvk::gi::SgLightmapAtlas& atlas,
	const std::string& path, std::string& err);

// Chart-id map (<scene>_charts.png): every chunk rect filled with a
// stable color keyed by the ATLAS CHUNK INDEX — the same chart id the
// cook stamps into texCoord2 (chunkBase + local chart) and the runtime
// debug views carry. The inner content area (margin .. size-margin)
// draws at full brightness, the per-chart margin ring dimmed to ~45%,
// the pack separator stays black, so content vs dilation padding vs
// gutter read at a glance (padding leaks show as bright bleed across
// the black separator). margin = the cook's gutterTexels (0 = no ring).
// Same skip-not-fatal policy as the atlas dump. Returns false + err on
// IO failure.
bool WriteLightmapChartsPng(const smvk::gi::SgLightmapAtlas& atlas,
	uint32_t margin, const std::string& path, std::string& err);

// Bright-pink fault map (<scene>_pink.png, --lm-debug-pink on): the
// TRUE lightmap the runtime loads — lobe-0 amplitude decoded exactly
// as uploaded (BC6H blocks decoded via the cook codec when the atlas
// is compressed, the RGBA16F layer content when not), the same
// display mapping as the atlas dump — with fault classes stamped
// solid bright pink (255, 0, 255) at a glance:
//   * unbaked rects (zero BAKE POINTS — the chart rasterized nothing,
//     sub-texel slivers the conservative claim still missed; the
//     payload stays zero with nothing to dilate). Chunks that BAKED
//     but traced zero light (occluded at low ray counts) are
//     legitimate darkness and stay unpinked — hasBakePoints (from the
//     cook's per-chart texel counts, parallel to atlas.chunks; empty
//     falls back to the payload-zero heuristic) separates the two,
//   * corrupt texels (non-finite amplitude in the AS-LOADED image,
//     painted per texel),
//   * rect-overlap intersections (pairwise chunk-rect intersections;
//     the GPU copy has no ownership check, so these are the texels a
//     later chunk destructively overwrites).
// Compressed atlases show the real BC6H artifacts (block quantization,
// the bc6hopt separator prefill halo) — this is the data the GPU
// samples, not the pre-compression payload. Cook-time only. Same
// skip-not-fatal policy as the other dumps. Returns false + err on IO
// failure.
bool WriteLightmapPinkPng(const smvk::gi::SgLightmapAtlas& atlas,
	const std::vector<char>& hasBakePoints, const std::string& path,
	std::string& err);

} // namespace pakcook
