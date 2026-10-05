// Solid-pink error texture mint (see ktx_error_texture.h). Mirrors a
// refused/stale texture's STRUCTURE and fills every texel with
// (255, 0, 255, 255) RGBA8 — an unmistakable "recook the pak" flag that
// streams cleanly (uncompressed: no Basis transcode anywhere, ever).

#include <Engine/Utils/ktx_error_texture.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ktx.h"

namespace smvk {
namespace {

// VkFormat core value (Vulkan 1.x, stable): R8G8B8A8_UNORM = 37.
// Hardcoded so this TU doesn't inherit a Vulkan header dependency
// (same convention as gltf_import's tex_transcode.cpp).
constexpr uint32_t kVkR8G8B8A8Unorm = 37;

constexpr uint8_t kPinkRgba[4] = { 255, 0, 255, 255 };

// Fill one (level, layer, faceSlice) slice with solid pink. Returns
// false when the ktx call rejects the bytes (should never happen for a
// matching size, but a mismatched mirror must not corrupt silently).
bool FillPinkSlice(ktxTexture2* tex, uint32_t level, uint32_t layer,
	uint32_t faceSlice) {
	const uint32_t w = std::max(1u, tex->baseWidth >> level);
	const uint32_t h = std::max(1u,
		tex->numDimensions < 2 ? 1u : tex->baseHeight >> level);
	const uint32_t d = std::max(1u,
		tex->numDimensions < 3 ? 1u : tex->baseDepth >> level);
	const size_t sliceBytes =
		size_t(w) * size_t(h) * size_t(d) * sizeof(kPinkRgba);
	std::vector<uint8_t> px(sliceBytes);
	for (size_t i = 0; i + 3 < px.size(); i += 4) {
		px[i + 0] = kPinkRgba[0];
		px[i + 1] = kPinkRgba[1];
		px[i + 2] = kPinkRgba[2];
		px[i + 3] = kPinkRgba[3];
	}
	return ktxTexture_SetImageFromMemory(ktxTexture(tex), level, layer,
		faceSlice, px.data(), ktx_size_t(sliceBytes)) == KTX_SUCCESS;
}

} // namespace

ktxTexture2* CreatePinkKtx2(const ktxTexture2& mirror) {
	ktxTextureCreateInfo ci{};
	ci.vkFormat = kVkR8G8B8A8Unorm;
	ci.baseWidth = std::max(1u, mirror.baseWidth);
	ci.baseHeight = std::max(1u, mirror.baseHeight);
	ci.baseDepth = std::max(1u, mirror.baseDepth);
	ci.numDimensions = mirror.numDimensions;
	ci.numLevels = std::max(1u, mirror.numLevels);
	ci.numLayers = std::max(1u, mirror.numLayers);
	ci.numFaces = std::max(1u, mirror.numFaces);
	ci.isArray = mirror.isArray;
	ci.generateMipmaps = KTX_FALSE;
	ktxTexture2* tex = nullptr;
	if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex)
		!= KTX_SUCCESS || !tex)
		return nullptr;
	// Mirror the source's transfer (an sRGB pink still reads pink; the
	// match keeps sRGB-sampled material slots sane). The query takes a
	// non-const This — const_cast for the pure read.
	ktxTexture2_SetOETF(tex, ktxTexture2_GetTransferFunction_e(
		const_cast<ktxTexture2*>(&mirror)));
	for (uint32_t level = 0; level < tex->numLevels; ++level) {
		// 3D textures address z slices through faceSlice; cube/array
		// layouts address faces/layers the same way the bakers do.
		const uint32_t depthSlices = std::max(1u, tex->baseDepth);
		const uint32_t faceSlices = (tex->numFaces > 1)
			? tex->numFaces : depthSlices;
		for (uint32_t layer = 0; layer < tex->numLayers; ++layer) {
			for (uint32_t slice = 0; slice < faceSlices; ++slice) {
				if (!FillPinkSlice(tex, level, layer, slice)) {
					ktxTexture2_Destroy(tex);
					return nullptr;
				}
			}
		}
	}
	return tex;
}

bool WritePinkKtx2Blob(const ktxTexture2& mirror, std::vector<uint8_t>& out) {
	out.clear();
	ktxTexture2* tex = CreatePinkKtx2(mirror);
	if (!tex)
		return false;
	ktx_uint8_t* mem = nullptr;
	ktx_size_t memLen = 0;
	const KTX_error_code res =
		ktxTexture_WriteToMemory(ktxTexture(tex), &mem, &memLen);
	ktxTexture2_Destroy(tex);
	if (res != KTX_SUCCESS || !mem || memLen == 0)
		return false;
	out.assign(mem, mem + memLen);
	// KTX-Software documents free() for the WriteToMemory buffer, but the
	// prebuilt KTX DLL is Release-built: a Debug executable (Debug CRT)
	// freeing a DLL-owned allocation crosses CRT heaps. Release frees
	// normally; Debug intentionally leaks this short-lived error-path
	// buffer (the gltf_import tex_transcode.cpp precedent).
#ifdef _DEBUG
	(void)mem;
#else
	std::free(mem);
#endif
	return true;
}

} // namespace smvk
