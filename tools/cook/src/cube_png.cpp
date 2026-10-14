#include "cube_png.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// STB implementation owner for both cook binaries that compile this file
// (PakCooker, CubemapBaker). lm_png.cpp and all other users in those
// binaries use the header declarations only — exactly one definition per
// binary. (The engine carries its own in Utils/LutIo.cpp, the editor in
// editor_branding.cpp.)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace pakcook {
namespace {

// Display chain mirrors lm_png.cpp (Reinhard + pow 1/2.2 + sRGB store)
// so cubemap and lightmap debug PNGs read the same. exposure folds a
// display scaling in first: captures store TRUE radiance (sun-lit walls
// ~sunIntensity, sky ~skyIntensity), so the cook passes
// 1/skyIntensity to keep the strips in the old readable range (sky
// mid-gray, sun disc clipping).
inline float SrgbEncode(float c) {
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.0031308f
		? 12.92f * c
		: 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline uint8_t DisplayByte(float v, float exposure) {
	if (!std::isfinite(v))
		v = 0.0f;
	v = std::clamp(v * exposure, 0.0f, 65504.0f);
	const float tm = v / (1.0f + std::max(v, 0.0f));
	const float g = std::pow(std::max(tm, 0.0f), 1.0f / 2.2f);
	return uint8_t(std::lround(SrgbEncode(g) * 255.0f));
}

inline bool Finite3(float x, float y, float z) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

// Base-face texel fetch (RGBA32F, level 0), row-major.
inline void FetchBase(const std::vector<float>& px, uint32_t size,
	uint32_t x, uint32_t y, float& r, float& g, float& b) {
	const float* p = &px[(size_t(y) * size + x) * 4];
	r = p[0];
	g = p[1];
	b = p[2];
}

struct Rgba8 {
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	void resize(uint32_t w_, uint32_t h_) {
		w = w_;
		h = h_;
		px.assign(size_t(w) * h * 4, 0);
	}
	void set(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
		uint8_t a = 255) {
		if (x >= w || y >= h)
			return;
		uint8_t* p = &px[(size_t(y) * w + x) * 4];
		p[0] = r;
		p[1] = g;
		p[2] = b;
		p[3] = a;
	}
	void circle(uint32_t cx, uint32_t cy, float rad,
		uint8_t r, uint8_t g, uint8_t b) {
		if (rad <= 0.0f)
			return;
		const int32_t x0 = std::max<int32_t>(0, int32_t(cx - rad - 1));
		const int32_t x1 = std::min<int32_t>(int32_t(w) - 1, int32_t(cx + rad + 1));
		const int32_t y0 = std::max<int32_t>(0, int32_t(cy - rad - 1));
		const int32_t y1 = std::min<int32_t>(int32_t(h) - 1, int32_t(cy + rad + 1));
		for (int32_t y = y0; y <= y1; ++y) {
			for (int32_t x = x0; x <= x1; ++x) {
				const float dx = float(x) - float(cx);
				const float dy = float(y) - float(cy);
				if (std::abs(std::sqrt(dx * dx + dy * dy) - rad) < 1.0f)
					set(uint32_t(x), uint32_t(y), r, g, b);
			}
		}
	}
};

bool ValidChains(const smvk::CubemapMipChains& chains, uint32_t size) {
	if (chains.size() != smvk::kCubemapFaceCount || size == 0)
		return false;
	for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
		if (chains[f].empty())
			return false;
		if (chains[f][0].size() != size_t(size) * size * 4)
			return false;
	}
	return true;
}

} // namespace

glm::vec3 CubemapCaptureMean(const smvk::CubemapMipChains& chains, uint32_t size) {
	glm::vec3 acc(0.0f);
	uint64_t n = 0;
	if (!ValidChains(chains, size))
		return glm::vec3(1.0f, 0.0f, 1.0f);
	for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
		const auto& px = chains[f][0];
		for (size_t i = 0; i < size_t(size) * size; ++i) {
			const float r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
			if (!Finite3(r, g, b))
				continue;
			acc.x += r;
			acc.y += g;
			acc.z += b;
			++n;
		}
	}
	if (n == 0)
		return glm::vec3(1.0f, 0.0f, 1.0f); // fully invalid: flag magenta
	return acc / float(n);
}

float CubemapMeanLuma(const glm::vec3& mean) {
	return 0.2126f * mean.x + 0.7152f * mean.y + 0.0722f * mean.z;
}

bool WriteCubemapFacesPng(const smvk::CubemapMipChains& chains, uint32_t size,
	float exposure, const std::string& path, std::string& err) {
	if (!ValidChains(chains, size)) {
		err = "face strip: chains/size mismatch";
		return false;
	}
	constexpr uint32_t kGutter = 2;
	const uint32_t W = smvk::kCubemapFaceCount * size
		+ (smvk::kCubemapFaceCount - 1) * kGutter;
	Rgba8 img;
	img.resize(W, size);
	for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
		const uint32_t x0 = f * (size + kGutter);
		const auto& px = chains[f][0];
		for (uint32_t y = 0; y < size; ++y) {
			for (uint32_t x = 0; x < size; ++x) {
				float r, g, b;
				FetchBase(px, size, x, y, r, g, b);
				img.set(x0 + x, y,
					DisplayByte(r, exposure),
					DisplayByte(g, exposure),
					DisplayByte(b, exposure));
			}
		}
	}
	if (!stbi_write_png(path.c_str(), int(img.w), int(img.h), 4,
			img.px.data(), int(img.w) * 4)) {
		err = "stbi_write_png failed for " + path;
		return false;
	}
	return true;
}

bool WriteCubemapMipsPng(const smvk::CubemapMipChains& chains, uint32_t size,
	float exposure, const std::string& path, std::string& err) {
	if (!ValidChains(chains, size)) {
		err = "mip sheet: chains/size mismatch";
		return false;
	}
	// +X face (index 0), every level stacked sharp -> rough.
	const auto& levels = chains[0];
	constexpr uint32_t kGutter = 2;
	uint32_t H = 0;
	uint32_t w = size;
	for (size_t m = 0; m < levels.size(); ++m) {
		H += w + (m + 1 < levels.size() ? kGutter : 0);
		w = std::max(1u, w / 2);
	}
	Rgba8 img;
	img.resize(size, H);
	uint32_t y0 = 0;
	w = size;
	for (size_t m = 0; m < levels.size(); ++m) {
		const auto& px = levels[m];
		if (px.size() != size_t(w) * w * 4) {
			err = "mip sheet: level size mismatch";
			return false;
		}
		for (uint32_t y = 0; y < w; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const float* p = &px[(size_t(y) * w + x) * 4];
				img.set(x, y0 + y,
					DisplayByte(p[0], exposure),
					DisplayByte(p[1], exposure),
					DisplayByte(p[2], exposure));
			}
		}
		y0 += w + kGutter;
		w = std::max(1u, w / 2);
	}
	if (!stbi_write_png(path.c_str(), int(img.w), int(img.h), 4,
			img.px.data(), int(img.w) * 4)) {
		err = "stbi_write_png failed for " + path;
		return false;
	}
	return true;
}

bool WriteCubemapProbesPng(const std::vector<glm::vec3>& positions,
	const std::vector<glm::vec3>& means, const std::vector<float>& influences,
	const glm::vec3& bmin, const glm::vec3& bmax,
	const std::string& path, std::string& err) {
	if (positions.empty()) {
		err = "probes map: no positions";
		return false;
	}
	const float spanX = std::max(bmax.x - bmin.x, 1e-3f);
	const float spanZ = std::max(bmax.z - bmin.z, 1e-3f);
	const float longSpan = std::max(spanX, spanZ);
	const uint32_t W = (uint32_t)std::clamp(1024.0f * spanX / longSpan, 256.0f, 2048.0f);
	const uint32_t H = (uint32_t)std::clamp(1024.0f * spanZ / longSpan, 256.0f, 2048.0f);
	const float pad = 0.05f;
	auto toPx = [&](const glm::vec3& p, float& x, float& y) {
		x = (pad + (1.0f - 2.0f * pad) * (p.x - bmin.x) / spanX) * float(W);
		y = (pad + (1.0f - 2.0f * pad) * (p.z - bmin.z) / spanZ) * float(H);
	};
	const float pxPerM = (1.0f - 2.0f * pad) * float(W) / spanX;
	Rgba8 img;
	img.resize(W, H);
	// Background + bounds rect.
	for (uint32_t y = 0; y < H; ++y)
		for (uint32_t x = 0; x < W; ++x)
			img.set(x, y, 22, 22, 26);
	for (uint32_t x = 0; x < W; ++x) {
		img.set(x, 0, 90, 90, 100);
		img.set(x, H - 1, 90, 90, 100);
	}
	for (uint32_t y = 0; y < H; ++y) {
		img.set(0, y, 90, 90, 100);
		img.set(W - 1, y, 90, 90, 100);
	}
	// Rings first (influence to scale), then mean-color dots on top.
	for (size_t i = 0; i < positions.size(); ++i) {
		float x, y;
		toPx(positions[i], x, y);
		const float inf = (i < influences.size()) ? std::max(influences[i], 0.0f) : 0.0f;
		img.circle(uint32_t(x), uint32_t(y), inf * pxPerM, 110, 110, 120);
	}
	for (size_t i = 0; i < positions.size(); ++i) {
		float x, y;
		toPx(positions[i], x, y);
		const glm::vec3 m = (i < means.size()) ? means[i] : glm::vec3(1, 0, 1);
		uint8_t r, g, b;
		// Dead-black captures (mean ~0: embedded origin or fully occluded
		// at low bounce counts) flag magenta like NaN — a real capture
		// always sees some light, so black is a fault worth opening.
		if (!Finite3(m.x, m.y, m.z) || CubemapMeanLuma(m) < 1e-6f) {
			r = 255;
			g = 0;
			b = 255;
		} else {
			r = DisplayByte(m.x, 1.0f);
			g = DisplayByte(m.y, 1.0f);
			b = DisplayByte(m.z, 1.0f);
		}
		const int32_t cx = int32_t(x), cy = int32_t(y);
		for (int32_t dy = -5; dy <= 5; ++dy)
			for (int32_t dx = -5; dx <= 5; ++dx)
				if (dx * dx + dy * dy <= 25)
					img.set(uint32_t(cx + dx), uint32_t(cy + dy), r, g, b);
	}
	if (!stbi_write_png(path.c_str(), int(img.w), int(img.h), 4,
			img.px.data(), int(img.w) * 4)) {
		err = "stbi_write_png failed for " + path;
		return false;
	}
	return true;
}

} // namespace pakcook
