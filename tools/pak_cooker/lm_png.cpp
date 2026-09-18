#include "lm_png.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace pakcook {

namespace {

// sRGB OETF (the RGBA8_SRGB export target's store encoding in the
// editor; the cook dump mirrors it so the two PNGs are byte-comparable).
inline float SrgbEncode(float c) {
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.0031308f
		? 12.92f * c
		: 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Editor flat-view display chain (uvflat1.frag.hlsl): Reinhard on the
// lobe-0 amplitude, pow 1/2.2, then the sRGB target store.
inline uint8_t DisplayByte(float v) {
	if (std::isnan(v))
		v = 0.0f; // SanitizeHdr
	v = std::clamp(v, 0.0f, 65504.0f);
	const float tm = v / (1.0f + std::max(v, 0.0f));
	const float g = std::pow(std::max(tm, 0.0f), 1.0f / 2.2f);
	return uint8_t(std::lround(SrgbEncode(g) * 255.0f));
}

} // namespace

bool WriteLightmapAtlasPng(const smvk::gi::SgLightmapAtlas& atlas,
	const std::string& path, std::string& err) {
	using namespace smvk::gi;
	if (atlas.width == 0 || atlas.height == 0 || atlas.chunks.empty()) {
		err = "empty atlas";
		return false;
	}
	const uint32_t w = atlas.width;
	const uint32_t h = atlas.height;
	std::vector<uint8_t> px(size_t(w) * h * 4);
	// Background = the editor export's clear color (0.02, 0.02, 0.03).
	const uint8_t bg[4] = {
		DisplayByte(0.02f), DisplayByte(0.02f), DisplayByte(0.03f), 255 };
	for (size_t i = 0; i < size_t(w) * h; ++i)
		std::memcpy(&px[i * 4], bg, 4);

	size_t skipped = 0;
	for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
		const SgLightmapChunk& c = atlas.chunks[ci];
		const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
		const uint32_t n = std::min<uint32_t>(c.lobeCount, kLightmapMaxLobes);
		const int ox = int(c.rectMin[0]);
		const int oy = int(c.rectMin[1]);
		if (n == 0 || p.width == 0 || p.height == 0
			|| ox < 0 || oy < 0
			|| ox + int(p.width) > int(w) || oy + int(p.height) > int(h)
			|| p.coeffs.size() != size_t(p.width) * p.height * c.lobeCount) {
			++skipped;
			continue;
		}
		for (uint32_t y = 0; y < p.height; ++y) {
			uint8_t* row = &px[(size_t(oy + int(y)) * w + size_t(ox)) * 4];
			for (uint32_t x = 0; x < p.width; ++x) {
				const glm::vec3& a =
					p.coeffs[(size_t(y) * p.width + size_t(x)) * c.lobeCount];
				row[x * 4 + 0] = DisplayByte(a.r);
				row[x * 4 + 1] = DisplayByte(a.g);
				row[x * 4 + 2] = DisplayByte(a.b);
				row[x * 4 + 3] = 255;
			}
		}
	}
	if (!stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(),
			int(w) * 4)) {
		err = "stbi_write_png failed for " + path;
		return false;
	}
	if (skipped)
		std::cerr << "[LM-PNG] note: " << skipped << "/"
			<< atlas.chunks.size() << " chunks skipped (empty/OOB rect or "
			"payload stride mismatch)\n" << std::flush;
	return true;
}

} // namespace pakcook
