// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldFields brick codecs — the cooked/static-brick encoding path
// (milestone 4: "compression and asset cooking").
//
// BC4_UNORM is the static density format: one channel, 8 bytes per 4x4x1
// block, hardware-sampled and filtered natively by the atlas. A stored
// brick (68^3 texels) encodes PER-Z-SLICE as 17x17 blocks x 68 slices =
// 157,216 bytes — half an R8 brick, exactly the slot-upload layout the
// region uploads expect (blocks (bx, by, z) at ((z*blocksY)+by)*blocksX+bx).
//
// The SqrtUNorm density packing (attribute in [0,1] -> stored
// enc = sqrt(v)) buys precision at LOW densities — the range fog cares
// about. The trade (tested against a dense reference): hardware
// trilinear filters the ENCODED domain, then the shader decodes
// scale*enc^2+bias — filtering and decoding do NOT commute; the fidelity
// test pins the error bound.
//
// CPU-side decode exists for cook-side fidelity validation and tooling
// only — the runtime never decodes bricks on the CPU (the GPU samples
// BC4 natively; CPU-side gameplay values read the field's source, not
// the atlas).

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace engine::worldfields {

// ---------------------------------------------------------------------------
// BC4 codecs (the static density format: one channel, 8 B per 4x4x1 block)
// ---------------------------------------------------------------------------

// Encode one stored brick's UNORM texels (68^3, tight z/y/x layout)
// into BC4_UNORM blocks. outCapacity must be >= StoredBrickBlockBytes()
// (= (stored/4)^2 * stored * 8). Returns the bytes written, 0 on a
// capacity/size mismatch.
size_t Bc4EncodeBrick(const uint8_t* storedTexels, uint32_t storedSize,
	std::byte* outBlocks, size_t outCapacity);

// Software decode (the vendored DirectXTex BC4 decoder) — fidelity
// checks and tooling. Returns the bytes decoded, 0 on mismatch.
size_t Bc4DecodeBrick(const std::byte* blocks, size_t blockBytes,
	uint32_t storedSize, uint8_t* outTexels, size_t outCapacity);

// ---------------------------------------------------------------------------
// BC6H codecs (the fog-GI radiance format: RGB half-float, 16 B per
// 4x4x1 block — the same block shape the dense SG-GI layers use). A
// stored brick (68^3 texels) encodes PER-Z-SLICE as 17x17 blocks x 68
// slices = 314,368 bytes. The input is fp16 TRIPLES (halves, 3 per
// texel, tightly packed z/y/x) — the caller packs float RGB through
// PackHalfClamped (the existing bc6h.h helpers).
// ---------------------------------------------------------------------------

// Encode one stored brick's fp16 RGB texels (68^3, 3 halves per texel,
// tight z/y/x layout) into BC6H_UFloat blocks (the reference-quality
// multi-mode encoder via the vendored DirectXTex D3DXEncodeBC6HU).
size_t Bc6hEncodeBrick(const uint16_t* halves, uint32_t storedSize,
	std::byte* outBlocks, size_t outCapacity);

// Software decode (DirectTex's BC6H decoder) — fidelity checks/tooling.
size_t Bc6hDecodeBrick(const std::byte* blocks, size_t blockBytes,
	uint32_t storedSize, uint16_t* outHalves, size_t outCapacity);

// ---------------------------------------------------------------------------
// Shared block math (BC4: 4x4x1 of 8 B; BC6H: 4x4x1 of 16 B)
// ---------------------------------------------------------------------------

constexpr size_t StoredBrickBlockBytes(uint32_t storedSize,
	uint32_t blockWidth, uint32_t blockHeight, uint32_t bytesPerBlock)
{
	const uint32_t bx = storedSize / blockWidth;
	const uint32_t by = storedSize / blockHeight;
	return size_t(bx) * by * storedSize * bytesPerBlock;
}

// Attribute in [0,1] -> UNORM8 (the Linear packing half).
void QuantizeUnorm8(const float* attributes, size_t count, uint8_t* out);

// Attribute in [0,1] -> sqrt(attribute) quantized UNORM8 (the SqrtUNorm
// packing — the shader's decodeScale*enc*enc+decodeBias is the inverse).
// The sqrt happens BEFORE quantization (and before BC4): the quantizer's
// 8 bits concentrate where small densities live.
void QuantizeSqrtUnorm8(const float* attributes, size_t count, uint8_t* out);

} // namespace engine::worldfields
