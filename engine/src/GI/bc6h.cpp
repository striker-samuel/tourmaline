#include <Engine/GI/bc6h.h>

#include <cstring>

#include <DirectXTex/BC.h>

#include <Engine/GI/sg.h> // UnpackHalf

namespace smvk::gi {
namespace {

constexpr size_t kBlockBytes = 16;

} // namespace

// ---------------------------------------------------------------------------
// Encode — thin wrapper over the vendored DirectXTex BC6H-U block codec
// (D3DXEncodeBC6HU: reference-quality multi-mode encoder; we only marshal
// the lobe image's half patterns into the float RGBA block layout).
// ---------------------------------------------------------------------------

void Bc6hEncodeImage(const uint16_t* halves, uint32_t w, uint32_t h,
	std::vector<uint8_t>& out) {
	const uint32_t blocksX = w / 4;
	const uint32_t blocksY = h / 4;
	const size_t base = out.size();
	out.resize(base + size_t(blocksX) * blocksY * kBlockBytes);

	DirectX::XMVECTOR block[16];
	for (uint32_t by = 0; by < blocksY; ++by) {
		for (uint32_t bx = 0; bx < blocksX; ++bx) {
			for (uint32_t y = 0; y < 4; ++y)
				for (uint32_t x = 0; x < 4; ++x) {
					const uint16_t* texel = &halves[(size_t(by * 4 + y) * w
						+ (bx * 4 + x)) * 3];
					block[y * 4 + x] = DirectX::XMVectorSet(
						UnpackHalf(texel[0]), UnpackHalf(texel[1]),
						UnpackHalf(texel[2]), 1.0f);
				}
			DirectX::D3DXEncodeBC6HU(
				&out[base + (size_t(by) * blocksX + bx) * kBlockBytes],
				block, 0);
		}
	}
}

// ---------------------------------------------------------------------------
// Decode (block -> half patterns; DirectXTex's software decoder handles
// every BC6H mode, so arbitrary blocks decode on the load/inspect path)
// ---------------------------------------------------------------------------

void Bc6hDecodeImage(const uint8_t* blocks, uint32_t w, uint32_t h,
	std::vector<uint16_t>& out) {
	const uint32_t blocksX = w / 4;
	const uint32_t blocksY = h / 4;
	out.assign(size_t(w) * h * 3, 0);

	DirectX::XMVECTOR block[16];
	for (uint32_t by = 0; by < blocksY; ++by) {
		for (uint32_t bx = 0; bx < blocksX; ++bx) {
			DirectX::D3DXDecodeBC6HU(block,
				blocks + (size_t(by) * blocksX + bx) * kBlockBytes);
			for (uint32_t y = 0; y < 4; ++y)
				for (uint32_t x = 0; x < 4; ++x) {
					const DirectX::XMVECTOR c = block[y * 4 + x];
					uint16_t* dst = &out[(size_t(by * 4 + y) * w
						+ (bx * 4 + x)) * 3];
					dst[0] = PackHalf(DirectX::XMVectorGetX(c));
					dst[1] = PackHalf(DirectX::XMVectorGetY(c));
					dst[2] = PackHalf(DirectX::XMVectorGetZ(c));
				}
		}
	}
}

} // namespace smvk::gi
