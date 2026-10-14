#pragma once
// cube_png: cook-time cubemap debug dumps — the baked captures as PNGs,
// written loose next to the outputs (<name>_N.png etc., never into the
// pak). Same role as the lightmap chart PNGs (lm_png.h): comparing these
// against the runtime view localizes faults to the bake/encode vs the
// load/sample path (black ball + clean PNGs = runtime sampling; corrupt
// PNGs = capture/prefilter/encode).
//
//   <name>_N.png       base faces left-to-right +X,-X,+Y,-Y,+Z,-Z (2px
//                      black gutters), HDR->LDR with exposure + the same
//                      Reinhard/gamma/sRGB display chain as the lightmap
//                      dumps. Face order matches CubemapFace.
//   <name>_N_mips.png  +X face, all mip levels stacked sharp->rough:
//                      the GGX prefilter read directly (rough blur must
//                      grow down the stack; level 0 identical to the strip).
//   <name>_probes.png  top-down (XZ) placement map over the scene bounds:
//                      per probe a filled dot in its capture's mean color
//                      + an influence-radius ring to scale (bounds rect
//                      outline). NaN/empty captures draw magenta. Answers
//                      "is there a cubemap near X?" without the app.
//
// Cook-time only, part of PakCooker and CubemapBaker (stb_image_write
// from external/stb; the STB implementation lives in cube_png.cpp — the one
// TU every cook binary compiles. The engine and App never see it). All
// writers are skip-on-IO-failure (false + err), never fatal.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Renderer/cubemap_prefilter.h>

namespace pakcook {

// Mean linear RGB over the 6 base faces (level 0). NaN/Inf texels are
// ignored (quarantined like the volumetric sanitize); fully-invalid
// captures return magenta (1, 0, 1) so the probes map flags them.
glm::vec3 CubemapCaptureMean(const smvk::CubemapMipChains& chains, uint32_t size);

// Mean luminance (Rec.709) of a linear RGB mean, for one-line cook logs.
float CubemapMeanLuma(const glm::vec3& mean);

bool WriteCubemapFacesPng(const smvk::CubemapMipChains& chains, uint32_t size,
	float exposure, const std::string& path, std::string& err);

bool WriteCubemapMipsPng(const smvk::CubemapMipChains& chains, uint32_t size,
	float exposure, const std::string& path, std::string& err);

bool WriteCubemapProbesPng(const std::vector<glm::vec3>& positions,
	const std::vector<glm::vec3>& means, const std::vector<float>& influences,
	const glm::vec3& bmin, const glm::vec3& bmax,
	const std::string& path, std::string& err);

} // namespace pakcook
