// In-process LDR image -> KTX2 (stb decode, stbir mips, libktx write).
//
// Cook/editor texture promotion: source PNG/JPG/TGA/BMP bytes become an
// uncompressed RGBA8 KTX2 (full mip chain) entirely in-process — no toktx
// dependency. The transfer function honors the file-node colorspace
// (sRGB for color/emissive, linear for data). Callers cache the result
// as a `.ktx2` sidecar next to the source (see gltf.cpp) so the transcode
// runs once; recooks and editor reloads reuse the sidecar.
//
// stb symbols are static (no ODR risk with the editor/app TUs that compile
// their own stb_image); STB_IMAGE_RESIZE_IMPLEMENTATION lives only here.

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4267 4456 4459)
#endif

// Static linkage: host binaries (Editor, App) compile their own stb
// elsewhere — static keeps this TU self-contained with no LNK2005 risk.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <ktx.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// VkFormat core values (Vulkan 1.x, stable): R8G8B8A8_UNORM = 37,
// R8G8B8A8_SRGB = 43. Hardcoded so gltf_import doesn't inherit a Vulkan
// header dependency for two constants.
namespace {

	constexpr uint32_t kVkR8G8B8A8Unorm = 37;
	constexpr uint32_t kVkR8G8B8A8Srgb = 43;

	struct KtxGuard {
		ktxTexture2* tex = nullptr;
		explicit KtxGuard(ktxTexture2* t) : tex(t) {}
		~KtxGuard() {
			if (tex)
				ktxTexture2_Destroy(tex);
		}
		KtxGuard(const KtxGuard&) = delete;
		KtxGuard& operator=(const KtxGuard&) = delete;
	};

	uint32_t MipLevels(uint32_t w, uint32_t h) {
		uint32_t levels = 1;
		while (w > 1 || h > 1) {
			if (w > 1)
				w /= 2;
			if (h > 1)
				h /= 2;
			++levels;
		}
		return levels;
	}

} // namespace

bool TranscodeImageToKtx2(const uint8_t* srcBytes, size_t srcSize, bool srgb,
	const std::string& logName, std::vector<uint8_t>& outKtx2, std::string& err) {
	outKtx2.clear();
	if (!srcBytes || srcSize == 0) {
		err = "empty source bytes: " + logName;
		return false;
	}
	int w = 0, h = 0, comp = 0;
	if (stbi_info_from_memory(srcBytes, int(srcSize), &w, &h, &comp) == 0) {
		err = std::string("undecodable image (not PNG/JPG/TGA/BMP/GIF/PSD/PIC?): ")
			+ logName;
		return false;
	}
	int iw = 0, ih = 0;
	stbi_uc* rgba = stbi_load_from_memory(srcBytes, int(srcSize), &iw, &ih,
		nullptr, 4);
	if (!rgba || iw <= 0 || ih <= 0) {
		err = std::string("stb decode failed: ") + logName + " ("
			+ (stbi_failure_reason() ? stbi_failure_reason() : "?") + ")";
		if (rgba)
			stbi_image_free(rgba);
		return false;
	}
	const uint32_t uw = uint32_t(iw), uh = uint32_t(ih);
	const uint32_t levels = MipLevels(uw, uh);
	// Full mip chain (box downsample; gamma-correct for sRGB so mips
	// match what toktx --genmipmap produces closely enough for a cache).
	std::vector<std::vector<uint8_t>> mips;
	mips.reserve(levels);
	mips.emplace_back(rgba, rgba + size_t(uw) * uh * 4);
	stbi_image_free(rgba);
	uint32_t mw = uw, mh = uh;
	for (uint32_t l = 1; l < levels; ++l) {
		const uint32_t nw = mw > 1 ? mw / 2 : 1;
		const uint32_t nh = mh > 1 ? mh / 2 : 1;
		std::vector<uint8_t> dst(size_t(nw) * nh * 4);
		// sRGB gets gamma-correct resampling; data maps take the linear
		// path (this stbir vintage spells it `generic`).
		const int ok = srgb
			? stbir_resize_uint8_srgb(mips[l - 1].data(), int(mw), int(mh), 0,
				dst.data(), int(nw), int(nh), 0, 4, 3, 0)
			: stbir_resize_uint8_generic(mips[l - 1].data(), int(mw), int(mh),
				0, dst.data(), int(nw), int(nh), 0, 4, 3, 0,
				STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT,
				STBIR_COLORSPACE_LINEAR, nullptr);
		if (!ok) {
			err = "mip resample failed at level " + std::to_string(l)
				+ ": " + logName;
			return false;
		}
		mips.push_back(std::move(dst));
		mw = nw;
		mh = nh;
	}
	ktxTextureCreateInfo ci{};
	ci.vkFormat = srgb ? kVkR8G8B8A8Srgb : kVkR8G8B8A8Unorm;
	ci.baseWidth = uw;
	ci.baseHeight = uh;
	ci.baseDepth = 1;
	ci.numDimensions = 2;
	ci.numLevels = levels;
	ci.numLayers = 1;
	ci.numFaces = 1;
	ci.isArray = KTX_FALSE;
	ci.generateMipmaps = KTX_FALSE;
	ktxTexture2* tex = nullptr;
	if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex)
		!= KTX_SUCCESS
		|| !tex) {
		err = "ktxTexture2_Create failed: " + logName;
		return false;
	}
	KtxGuard guard(tex);
	for (uint32_t l = 0; l < levels; ++l) {
		const std::vector<uint8_t>& m = mips[l];
		if (ktxTexture_SetImageFromMemory(ktxTexture(tex), l, 0, 0,
			m.data(), m.size())
			!= KTX_SUCCESS) {
			err = "ktx SetImageFromMemory failed at level "
				+ std::to_string(l) + ": " + logName;
			return false;
		}
	}
	// Zstd supercompression (transparent to the runtime reader, which
	// inflates on load): best-effort — uncompressed KTX2 is equally valid.
	if (ktxTexture2_DeflateZstd(tex, 3) != KTX_SUCCESS)
		std::fprintf(stderr, "KTX convert: zstd unavailable, storing '%s' uncompressed\n",
			logName.c_str());
	// Stamp the transfer for sidecar validation: a later colorspace
	// change must invalidate the cache (see SidecarTransferMismatch).
	{
		const char* fer = srgb ? "srgb" : "linear";
		ktxHashList_AddKVPair(&tex->kvDataHead, "VTXfer",
			(unsigned int)std::strlen(fer), fer);
	}
	ktx_uint8_t* dstBytes = nullptr;
	ktx_size_t dstSize = 0;
	if (ktxTexture_WriteToMemory(ktxTexture(tex), &dstBytes, &dstSize)
		!= KTX_SUCCESS
		|| !dstBytes || dstSize == 0) {
		err = "ktx WriteToMemory failed: " + logName;
		return false;
	}
	outKtx2.assign(dstBytes, dstBytes + dstSize);

	// KTX-Software documents that the buffer returned by
	// ktxTexture_WriteToMemory() must be released with free().
	//
	// On Windows, however, the prebuilt KTX DLL is Release-built and allocates
	// this buffer using the Release CRT. A Debug executable normally uses the
	// Debug CRT (/MDd), so freeing the DLL-owned allocation here can cross CRT
	// heaps and crash.
	//
	// Release builds use the matching CRT and may free normally. In Debug we
	// intentionally leak this temporary buffer rather than free it through an
	// incompatible CRT. This is acceptable for the current test/editor workflow
	// where this path is short-lived; if Debug transcodes become long-running or
	// frequent, replace this workaround with matching KTX Debug binaries or a
	// DLL-side deallocator.
#ifdef _DEBUG
	(void)dstBytes; // Intentionally leaked: avoid Release-DLL / Debug-CRT free.
#else
	free(dstBytes);
#endif
	return true;
}
