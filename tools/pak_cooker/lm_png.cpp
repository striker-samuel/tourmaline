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

// Stable chart-id color: golden-ratio hue step keeps adjacent chart ids
// maximally separated (sequential ids never share a hue), fixed S/V.
inline void ChartColor(uint32_t chartId, uint8_t out[3]) {
	const float h = std::fmod(float(chartId) * 0.61803398875f, 1.0f);
	const float s = 0.85f, v = 1.0f;
	const int sector = int(h * 6.0f);
	const float frac = h * 6.0f - float(sector);
	const float p = v * (1.0f - s);
	const float q = v * (1.0f - s * frac);
	const float t = v * (1.0f - s * (1.0f - frac));
	float r, g, b;
	switch (sector % 6) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	out[0] = uint8_t(std::lround(r * 255.0f));
	out[1] = uint8_t(std::lround(g * 255.0f));
	out[2] = uint8_t(std::lround(b * 255.0f));
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

bool WriteLightmapChartsPng(const smvk::gi::SgLightmapAtlas& atlas,
	uint32_t margin, const std::string& path, std::string& err) {
	using namespace smvk::gi;
	if (atlas.width == 0 || atlas.height == 0 || atlas.chunks.empty()) {
		err = "empty atlas";
		return false;
	}
	const uint32_t w = atlas.width;
	const uint32_t h = atlas.height;
	std::vector<uint8_t> px(size_t(w) * h * 4, 0); // separator = black
	size_t skipped = 0;
	for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
		const SgLightmapChunk& c = atlas.chunks[ci];
		const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
		const int ox = int(c.rectMin[0]);
		const int oy = int(c.rectMin[1]);
		if (p.width == 0 || p.height == 0 || ox < 0 || oy < 0
			|| ox + int(p.width) > int(w) || oy + int(p.height) > int(h)) {
			++skipped;
			continue;
		}
		uint8_t col[3];
		ChartColor(uint32_t(ci), col);
		const auto fill = [&](uint32_t x0, uint32_t y0, uint32_t x1,
			uint32_t y1, float shade) {
			for (uint32_t y = y0; y < y1; ++y) {
				uint8_t* row = &px[(size_t(oy + int(y)) * w
					+ size_t(ox + int(x0))) * 4];
				for (uint32_t x = x0; x < x1; ++x) {
					row[0] = uint8_t(std::lround(col[0] * shade));
					row[1] = uint8_t(std::lround(col[1] * shade));
					row[2] = uint8_t(std::lround(col[2] * shade));
					row[3] = 255;
					row += 4;
				}
			}
		};
		// Full rect dimmed (the dilation padding ring), inner content at
		// full brightness. margin rings larger than half the rect leave
		// no content — the whole rect stays dimmed.
		fill(0, 0, p.width, p.height, 0.45f);
		const uint32_t mx = std::min(margin, p.width / 2);
		const uint32_t my = std::min(margin, p.height / 2);
		fill(mx, my, p.width - mx, p.height - my, 1.0f);
	}
	if (!stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(),
			int(w) * 4)) {
		err = "stbi_write_png failed for " + path;
		return false;
	}
	if (skipped)
		std::cerr << "[LM-PNG] note: " << skipped << "/"
			<< atlas.chunks.size() << " chunks skipped (empty/OOB rect)\n"
			<< std::flush;
	return true;
}

} // namespace pakcook
