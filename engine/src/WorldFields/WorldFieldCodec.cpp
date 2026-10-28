// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <cassert>
#include <cmath>
#include <cstring>

#include <DirectXTex/BC.h>

#include <Engine/GI/sg.h> // PackHalf / UnpackHalf (global scope)
#include <Engine/WorldFields/WorldFieldCodec.h>

namespace engine::worldfields {

// Same clamped fp16 pack the dense SG bakes use (BC6H-UF is unsigned —
// clamp to [0, 65504]; the convention outlived the .sggi dense layer
// that first introduced it).
static inline uint16_t PackHalfClamped(float v)
{
	return smvk::gi::PackHalf(std::min(std::max(v, 0.0f), 65504.0f));
}

size_t Bc4EncodeBrick(const uint8_t* storedTexels, uint32_t storedSize,
	std::byte* outBlocks, size_t outCapacity)
{
	assert(storedTexels != nullptr && outBlocks != nullptr);
	// 4x4x1 blocks: X/Y tile in 4s, Z stays per-texel.
	if (storedSize % 4u != 0u)
		return 0;
	const uint32_t blocksX = storedSize / 4u;
	const uint32_t blocksY = storedSize / 4u;
	const size_t expected = size_t(blocksX) * blocksY * storedSize * 8u;
	if (outCapacity < expected)
		return 0;

	// Per-z-slice 2D encode (the same loop shape as Bc6hEncodeImage —
	// DirectXTex block codecs are 2D; a 3D brick is a stack of slices).
	uint8_t* out = reinterpret_cast<uint8_t*>(outBlocks);
	DirectX::XMVECTOR block[16];
	for (uint32_t z = 0; z < storedSize; ++z) {
		const uint8_t* slice = storedTexels + size_t(z) * storedSize * storedSize;
		uint8_t* sliceOut = out + size_t(z) * blocksX * blocksY * 8u;
		for (uint32_t by = 0; by < blocksY; ++by)
			for (uint32_t bx = 0; bx < blocksX; ++bx) {
				for (uint32_t y = 0; y < 4; ++y)
					for (uint32_t x = 0; x < 4; ++x) {
						// BC4 encodes the R channel; hardware UNORM
						// decode returns [0,1] in R.
						const float v = float(slice[(size_t(by * 4 + y) * storedSize)
							+ (bx * 4 + x)]) / 255.0f;
						block[y * 4 + x] = DirectX::XMVectorSet(v, v, v, v);
					}
				DirectX::D3DXEncodeBC4U(
					sliceOut + (size_t(by) * blocksX + bx) * 8u, block, 0);
			}
	}
	return expected;
}

size_t Bc4DecodeBrick(const std::byte* blocks, size_t blockBytes,
	uint32_t storedSize, uint8_t* outTexels, size_t outCapacity)
{
	assert(blocks != nullptr && outTexels != nullptr);
	if (storedSize % 4u != 0u)
		return 0;
	const uint32_t blocksX = storedSize / 4u;
	const uint32_t blocksY = storedSize / 4u;
	const size_t expected = size_t(blocksX) * blocksY * storedSize * 8u;
	if (blockBytes != expected || outCapacity < size_t(storedSize) * storedSize * storedSize)
		return 0;

	const uint8_t* in = reinterpret_cast<const uint8_t*>(blocks);
	DirectX::XMVECTOR block[16];
	for (uint32_t z = 0; z < storedSize; ++z) {
		uint8_t* slice = outTexels + size_t(z) * storedSize * storedSize;
		const uint8_t* sliceIn = in + size_t(z) * blocksX * blocksY * 8u;
		for (uint32_t by = 0; by < blocksY; ++by)
			for (uint32_t bx = 0; bx < blocksX; ++bx) {
				DirectX::D3DXDecodeBC4U(block,
					sliceIn + (size_t(by) * blocksX + bx) * 8u);
				for (uint32_t y = 0; y < 4; ++y)
					for (uint32_t x = 0; x < 4; ++x) {
						const float v = DirectX::XMVectorGetX(block[y * 4 + x]);
						slice[(size_t(by * 4 + y) * storedSize) + (bx * 4 + x)] =
							uint8_t(v * 255.0f + 0.5f);
					}
			}
	}
	return expected;
}

void QuantizeUnorm8(const float* attributes, size_t count, uint8_t* out)
{
	for (size_t i = 0; i < count; ++i) {
		const float v = std::min(std::max(attributes[i], 0.0f), 1.0f);
		out[i] = uint8_t(v * 255.0f + 0.5f);
	}
}

void QuantizeSqrtUnorm8(const float* attributes, size_t count, uint8_t* out)
{
	for (size_t i = 0; i < count; ++i) {
		const float v = std::min(std::max(attributes[i], 0.0f), 1.0f);
		out[i] = uint8_t(std::sqrt(v) * 255.0f + 0.5f);
	}
}

// ---------------------------------------------------------------------------
// BC6H (the fog-GI radiance codec — the same per-z-slice encode shape the
// dense SG-GI layers use, wrapped for the stored-brick layout)
// ---------------------------------------------------------------------------

size_t Bc6hEncodeBrick(const uint16_t* halves, uint32_t storedSize,
	std::byte* outBlocks, size_t outCapacity)
{
	assert(halves != nullptr && outBlocks != nullptr);
	// 4x4x1 blocks: X/Y tile in 4s, Z stays per-texel. 16 B per block.
	if (storedSize % 4u != 0u)
		return 0;
	const uint32_t blocksX = storedSize / 4u;
	const uint32_t blocksY = storedSize / 4u;
	const size_t expected = size_t(blocksX) * blocksY * storedSize * 16u;
	if (outCapacity < expected)
		return 0;

	uint8_t* out = reinterpret_cast<uint8_t*>(outBlocks);
	DirectX::XMVECTOR block[16];
	for (uint32_t z = 0; z < storedSize; ++z) {
		// Input: 3 halves per texel (RGB), tight z/y/x.
		const uint16_t* slice = halves
			+ size_t(z) * storedSize * storedSize * 3;
		uint8_t* sliceOut = out + size_t(z) * blocksX * blocksY * 16u;
		for (uint32_t by = 0; by < blocksY; ++by)
			for (uint32_t bx = 0; bx < blocksX; ++bx) {
				for (uint32_t y = 0; y < 4; ++y)
					for (uint32_t x = 0; x < 4; ++x) {
						// Unpack fp16 -> float (the caller pre-clamped to
						// [0, 65504] — BC6H-UF is unsigned).
						const uint16_t* texel = &slice[
							(size_t(by * 4 + y) * storedSize
								+ (bx * 4 + x)) * 3];
						const float r = smvk::gi::UnpackHalf(texel[0]);
						const float g = smvk::gi::UnpackHalf(texel[1]);
						const float b = smvk::gi::UnpackHalf(texel[2]);
						block[y * 4 + x] =
							DirectX::XMVectorSet(r, g, b, 1.0f);
					}
				DirectX::D3DXEncodeBC6HU(
					sliceOut + (size_t(by) * blocksX + bx) * 16u, block, 0);
			}
	}
	return expected;
}

size_t Bc6hDecodeBrick(const std::byte* blocks, size_t blockBytes,
	uint32_t storedSize, uint16_t* outHalves, size_t outCapacity)
{
	assert(blocks != nullptr && outHalves != nullptr);
	if (storedSize % 4u != 0u)
		return 0;
	const uint32_t blocksX = storedSize / 4u;
	const uint32_t blocksY = storedSize / 4u;
	const size_t expected = size_t(blocksX) * blocksY * storedSize * 16u;
	const size_t outHalfCount = size_t(storedSize) * storedSize * storedSize * 3;
	if (blockBytes != expected || outCapacity < outHalfCount)
		return 0;

	const uint8_t* in = reinterpret_cast<const uint8_t*>(blocks);
	DirectX::XMVECTOR block[16];
	for (uint32_t z = 0; z < storedSize; ++z) {
		uint16_t* slice = outHalves
			+ size_t(z) * storedSize * storedSize * 3;
		const uint8_t* sliceIn = in + size_t(z) * blocksX * blocksY * 16u;
		for (uint32_t by = 0; by < blocksY; ++by)
			for (uint32_t bx = 0; bx < blocksX; ++bx) {
				DirectX::D3DXDecodeBC6HU(block,
					sliceIn + (size_t(by) * blocksX + bx) * 16u);
				for (uint32_t y = 0; y < 4; ++y)
					for (uint32_t x = 0; x < 4; ++x) {
						uint16_t* texel = &slice[
							(size_t(by * 4 + y) * storedSize
								+ (bx * 4 + x)) * 3];
						texel[0] = PackHalfClamped(
							DirectX::XMVectorGetX(block[y * 4 + x]));
						texel[1] = PackHalfClamped(
							DirectX::XMVectorGetY(block[y * 4 + x]));
						texel[2] = PackHalfClamped(
							DirectX::XMVectorGetZ(block[y * 4 + x]));
					}
			}
	}
	return expected;
}

} // namespace engine::worldfields
