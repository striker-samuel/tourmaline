// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// In-process LDR image -> KTX2 (stb decode, stbir mips, libktx write).
//
// Cook/editor texture promotion: source PNG/JPG/TGA/BMP bytes become a
// KTX2 (full or clamped mip chain) entirely in-process — no toktx
// dependency. The transfer function honors the texture-asset import
// setting (sRGB for color/emissive, linear for data); the encode target
// picks the payload format (uncompressed RGBA8, or a BCn target encoded
// through DirectXTex's per-block codecs — the .texture asset's import
// settings). Callers cache the result as a `.ktx2` sidecar next to the
// source (see gltf.cpp) so the transcode runs once; recooks and editor
// reloads reuse the sidecar.
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

// KtxEncodeFormat/TextureSourceInfo (the public decls this TU defines).
#include <gltf_import/gltf.h>

// Block-compressed encode targets (see KtxEncodeFormat): DirectXTex's
// low-level per-block codecs (the same vendored block-codec layer the
// engine's BC6H GI encode uses). sRGB is a VIEW interpretation (the
// sRGB VkFormat variant) — the block bytes are identical.
#include <DirectXTex/BC.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// VkFormat core values (Vulkan 1.x, stable): R8G8B8A8_UNORM = 37,
// R8G8B8A8_SRGB = 43; block-compressed pairs below. Hardcoded so
// gltf_import doesn't inherit a Vulkan header dependency (the
// tex_transcode precedent).
namespace {

	constexpr uint32_t kVkR8G8B8A8Unorm = 37;
	constexpr uint32_t kVkR8G8B8A8Srgb = 43;
	constexpr uint32_t kVkBc1RgbUnorm = 131;
	constexpr uint32_t kVkBc1RgbSrgb = 132;
	constexpr uint32_t kVkBc3Unorm = 137;
	constexpr uint32_t kVkBc3Srgb = 138;
	constexpr uint32_t kVkBc4Unorm = 139;
	constexpr uint32_t kVkBc4Srgb = 140;
	constexpr uint32_t kVkBc5Unorm = 141;
	constexpr uint32_t kVkBc5Srgb = 142;
	constexpr uint32_t kVkBc7Unorm = 145;
	constexpr uint32_t kVkBc7Srgb = 146;

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

	// One mip's RGBA8 texels -> one BCn level (ceil(w/4)*ceil(h/4) blocks,
	// edge-replicated past the image edge — the sampler never reads the
	// padding, the encode just needs full blocks).
	bool EncodeBcnLevel(const std::vector<uint8_t>& rgba, uint32_t w,
		uint32_t h, KtxEncodeFormat fmt, std::vector<uint8_t>& out) {
		const uint32_t bw = (w + 3) / 4;
		const uint32_t bh = (h + 3) / 4;
		const size_t blockBytes =
			(fmt == KtxEncodeFormat::Bc1 || fmt == KtxEncodeFormat::Bc4)
			? 8 : 16;
		out.assign(size_t(bw) * size_t(bh) * blockBytes, 0);
		for (uint32_t by = 0; by < bh; ++by) {
			for (uint32_t bx = 0; bx < bw; ++bx) {
				DirectX::XMVECTOR block[16];
				for (uint32_t y = 0; y < 4; ++y) {
					for (uint32_t x = 0; x < 4; ++x) {
						const uint32_t sx = std::min(bx * 4 + x, w - 1);
						const uint32_t sy = std::min(by * 4 + y, h - 1);
						const size_t i = (size_t(sy) * w + sx) * 4;
						block[y * 4 + x] = DirectX::XMVectorSet(
							rgba[i + 0] / 255.0f, rgba[i + 1] / 255.0f,
							rgba[i + 2] / 255.0f, rgba[i + 3] / 255.0f);
					}
				}
				uint8_t* dst =
					out.data() + (size_t(by) * bw + bx) * blockBytes;
				switch (fmt) {
				case KtxEncodeFormat::Bc1:
					DirectX::D3DXEncodeBC1(dst, block, 0.5f, 0);
					break;
				case KtxEncodeFormat::Bc3:
					DirectX::D3DXEncodeBC3(dst, block, 0);
					break;
				case KtxEncodeFormat::Bc4:
					DirectX::D3DXEncodeBC4U(dst, block, 0);
					break;
				case KtxEncodeFormat::Bc5:
					DirectX::D3DXEncodeBC5U(dst, block, 0);
					break;
				case KtxEncodeFormat::Bc7:
					DirectX::D3DXEncodeBC7(dst, block, 0);
					break;
				default:
					return false;
				}
			}
		}
		return true;
	}

} // namespace

const char* KtxEncodeFormatName(KtxEncodeFormat f) {
	switch (f) {
	case KtxEncodeFormat::Rgba8: return "rgba8";
	case KtxEncodeFormat::Bc1: return "bc1";
	case KtxEncodeFormat::Bc3: return "bc3";
	case KtxEncodeFormat::Bc4: return "bc4";
	case KtxEncodeFormat::Bc5: return "bc5";
	case KtxEncodeFormat::Bc7: return "bc7";
	}
	return "?";
}

bool KtxEncodeFormatFromName(const std::string& name, KtxEncodeFormat& f) {
	if (name == "rgba8") f = KtxEncodeFormat::Rgba8;
	else if (name == "bc1") f = KtxEncodeFormat::Bc1;
	else if (name == "bc3") f = KtxEncodeFormat::Bc3;
	else if (name == "bc4") f = KtxEncodeFormat::Bc4;
	else if (name == "bc5") f = KtxEncodeFormat::Bc5;
	else if (name == "bc7") f = KtxEncodeFormat::Bc7;
	else
		return false;
	return true;
}

uint32_t KtxEncodeFormatVkFormat(KtxEncodeFormat f, bool srgb) {
	switch (f) {
	case KtxEncodeFormat::Rgba8:
		return srgb ? kVkR8G8B8A8Srgb : kVkR8G8B8A8Unorm;
	case KtxEncodeFormat::Bc1:
		return srgb ? kVkBc1RgbSrgb : kVkBc1RgbUnorm;
	case KtxEncodeFormat::Bc3:
		return srgb ? kVkBc3Srgb : kVkBc3Unorm;
	case KtxEncodeFormat::Bc4:
		return srgb ? kVkBc4Srgb : kVkBc4Unorm;
	case KtxEncodeFormat::Bc5:
		return srgb ? kVkBc5Srgb : kVkBc5Unorm;
	case KtxEncodeFormat::Bc7:
		return srgb ? kVkBc7Srgb : kVkBc7Unorm;
	}
	return kVkR8G8B8A8Unorm;
}

bool ProbeTextureSource(const uint8_t* data, size_t size,
	TextureSourceInfo& out) {
	out = TextureSourceInfo{};
	if (!data || size == 0)
		return false;
	// KTX2 magic: header-only parse (no LOAD_IMAGE_DATA — cheap; the DFD
	// rides the header).
	if (size >= 8 && data[0] == 0xAB && data[1] == 'K' && data[2] == 'T'
		&& data[3] == 'X' && data[4] == ' ' && data[5] == '2'
		&& data[6] == '0' && data[7] == 0xBB) {
		ktxTexture2* tex = nullptr;
		if (ktxTexture2_CreateFromMemory(
				static_cast<const ktx_uint8_t*>(data),
				static_cast<ktx_size_t>(size), 0, &tex)
			!= KTX_SUCCESS || !tex)
			return false;
		KtxGuard guard(tex);
		out.isKtx2 = true;
		out.width = std::max(1u, tex->baseWidth);
		out.height = std::max(1u, tex->baseHeight);
		out.mipLevels = std::max(1u, tex->numLevels);
		out.vkFormat = tex->vkFormat;
		out.srgb = ktxTexture2_GetTransferFunction_e(tex)
			== KHR_DF_TRANSFER_SRGB;
		// Block-compressed / Basis-family detection off the VkFormat: the
		// BCn block range is contiguous (131..146); Basis payloads carry a
		// non-GPU format (the transcode-time target picks the real one).
		out.bcn = tex->vkFormat >= kVkBc1RgbUnorm
			&& tex->vkFormat <= kVkBc7Srgb;
		out.basis = ktxTexture2_NeedsTranscoding(tex) != KTX_FALSE;
		return true;
	}
	// LDR (stb header probe; components 0-4).
	int w = 0, h = 0, comp = 0;
	if (stbi_info_from_memory(data, int(size), &w, &h, &comp) == 0
		|| w <= 0 || h <= 0)
		return false;
	out.width = uint32_t(w);
	out.height = uint32_t(h);
	out.components = uint32_t(comp < 0 ? 0 : comp);
	return true;
}

bool TranscodeImageToKtx2(const uint8_t* srcBytes, size_t srcSize, bool srgb,
	const std::string& logName, std::vector<uint8_t>& outKtx2, std::string& err,
	KtxEncodeFormat format, uint32_t maxMipLevels) {
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
	uint32_t levels = MipLevels(uw, uh);
	// Mip clamp (texture-asset import setting): 0 = the full chain,
	// N > 0 keeps only the top N levels.
	if (maxMipLevels > 0)
		levels = std::min(levels, std::max(1u, maxMipLevels));
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
	// Encode target (texture-asset import setting): the sRGB flag picks
	// the sRGB VkFormat VARIANT — the block bytes are identical, sRGB is
	// purely a view interpretation.
	ci.vkFormat = KtxEncodeFormatVkFormat(format, srgb);
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
		if (KtxEncodeFormatIsBcn(format)) {
			// BCn target: encode the level's blocks (DirectXTex per 4x4
			// block; the level data = ceil(w/4)*ceil(h/4) blocks).
			const uint32_t lw = std::max(1u, uw >> l);
			const uint32_t lh = std::max(1u, uh >> l);
			std::vector<uint8_t> level;
			if (!EncodeBcnLevel(m, lw, lh, format, level)) {
				err = "BCn encode failed at level "
					+ std::to_string(l) + " ("
					+ KtxEncodeFormatName(format) + "): " + logName;
				return false;
			}
			if (ktxTexture_SetImageFromMemory(ktxTexture(tex), l, 0, 0,
					level.data(), level.size())
				!= KTX_SUCCESS) {
				err = "ktx SetImageFromMemory failed at level "
					+ std::to_string(l) + ": " + logName;
				return false;
			}
		}
		else if (ktxTexture_SetImageFromMemory(ktxTexture(tex), l, 0, 0,
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
