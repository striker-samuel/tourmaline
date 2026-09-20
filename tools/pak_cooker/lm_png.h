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
// Cook-time only, part of PakCooker (stb_image_write from app/vendor;
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

} // namespace pakcook
