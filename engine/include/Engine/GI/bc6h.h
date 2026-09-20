#pragma once
// Cook-time BC6H-U (unsigned half-float) block codec for the SG lightmap
// atlas. The runtime samples BC6H_UFloat natively (the atlas uploads as
// 12 one-per-lobe BC6H images — see SgLightmapAtlas::CompressToBc6h);
// this codec runs at COOK time only, plus a decode pass when a
// compressed .sglm is inspected (load path reconstructs approximate
// chunk payloads from the blocks for the CPU views/PNG dumps).
//
// Unsigned only: the SG fit is NNLS (non-negative amplitudes), so every
// stored half is >= 0 and BC6H-UF covers [0, 65504].
//
// The block codec itself is the vendored DirectXTex BC6H encoder/
// decoder (external/DirectXTex, D3DXEncodeBC6HU/D3DXDecodeBC6HU —
// reference-quality multi-mode). This wrapper only marshals lobe-image
// half patterns <-> the float RGBA block layout. The quality work is
// the caller's prefill (Frostbite: replace invalid texels with the
// block's valid-texel average so zeros do not expand the endpoint box).

#include <cstdint>
#include <vector>

namespace smvk::gi {

// One image: halves = w*h RGB half patterns (row-major), w and h must
// be multiples of 4. out receives (w/4)*(h/4)*16 block bytes.
void Bc6hEncodeImage(const uint16_t* halves, uint32_t w, uint32_t h,
	std::vector<uint8_t>& out);

// Decode one BC6H image (every mode; out = w*h*3 half patterns).
void Bc6hDecodeImage(const uint8_t* blocks, uint32_t w, uint32_t h,
	std::vector<uint16_t>& out);

} // namespace smvk::gi
