#pragma once
// lm_denoise: Intel Open Image Denoise pass over the fitted SG lightmap
// amplitudes — cook-time only, part of PakCooker. The engine and App
// never link OIDN: denoised payloads reach the runtime through the
// unchanged .sglm atlas file.
//
// The atlas assembles into one float3 image per SG lobe (chunk rects are
// disjoint atlas texel regions; PackGpuLayers maps payloads the same
// way). Each layer is denoised in place with OIDN's RTLightmap filter —
// a model trained for HDR directional (SH-style) lightmap content, which
// needs no albedo/normal aux. The Monte-Carlo noise the 1024-ray bake
// leaves in per-texel fits (and especially what low-ray bakes leave)
// smooths out; chart seams are protected by construction because
// collapses never cross charts and the images are assembled per lobe.

#include <Engine/GI/lightmap.h>

namespace pakcook {

enum class DenoiseQuality {
	Balanced = 0, // OIDN_QUALITY_BALANCED (faster)
	High = 1,     // OIDN_QUALITY_HIGH (default)
};

// True when this PakCooker build links OIDN (VULKANTOY_OIDN and the
// prebuilt release found under external/oidn at configure time).
bool DenoiseAvailable();

// Denoises every fitted atlas lobe layer in place (atlas.width x
// atlas.height images; empty atlas = no-op success). Returns false when
// OIDN is unavailable or the filter failed — partially denoised layers
// may remain in that case (the cook continues on the noisy amplitudes).
// Prints per-lobe progress + a summary to stdout/stderr.
bool DenoiseLightmapAtlas(smvk::gi::SgLightmapAtlas& atlas,
	DenoiseQuality quality);

} // namespace pakcook
