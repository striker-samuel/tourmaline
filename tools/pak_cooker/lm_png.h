#pragma once
// lm_png: cook-time raw lightmap atlas debug dump — the baked atlas
// TEXELS as a PNG, written next to the .sglm (<scene>_atlas.png). This
// is the payload itself (lobe 0 amplitude, chunk rects at their packed
// atlas positions, the editor flat-view display mapping on the editor's
// clear color), NOT the uv2-unwrapped geometry draw the editor exports.
// Comparing the two localizes lightmap faults:
//   cook PNG clean + editor PNG corrupt  -> stamping (texCoord2) or the
//                                           editor draw/export path;
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

} // namespace pakcook
