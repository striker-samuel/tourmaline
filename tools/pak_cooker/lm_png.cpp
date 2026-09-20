#include "lm_png.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <Engine/GI/bc6h.h>

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

// IEEE 754 half -> float (BC6H decode path: the blocks hold half
// patterns; UnpackHalf lives in the engine's anonymous namespace, so
// this is a local mirror).
inline float HalfToFloat(uint16_t h) {
	const uint32_t sign = uint32_t(h >> 15) << 31;
	const uint32_t exp = (h >> 10) & 0x1Fu;
	const uint32_t man = h & 0x3FFu;
	uint32_t bits;
	if (exp == 0) {
		if (man == 0)
			bits = sign; // zero
		else {
			// Subnormal: normalize into the float exponent.
			uint32_t e = 0, m = man;
			while ((m & 0x400u) == 0) {
				m <<= 1;
				++e;
			}
			bits = sign | ((113u - e) << 23) | ((m & 0x3FFu) << 13);
		}
	} else if (exp == 31) {
		bits = sign | 0x7F800000u | (man << 13); // inf / nan
	} else {
		bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
	}
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

bool WriteLightmapPinkPng(const smvk::gi::SgLightmapAtlas& atlas,
	const std::vector<char>& hasBakePoints, const std::string& path,
	std::string& err) {
	using namespace smvk::gi;
	if (atlas.width == 0 || atlas.height == 0 || atlas.chunks.empty()) {
		err = "empty atlas";
		return false;
	}
	const uint32_t w = atlas.width;
	const uint32_t h = atlas.height;

	// As-loaded lobe-0 image (the texture the runtime uploads):
	// compressed -> decode lobe image 0 from the BC6H blocks (what the
	// GPU samples, artifacts and all); uncompressed -> the RGBA16F
	// layer content (rect texels from the payloads, zeros elsewhere).
	std::vector<float> lm(size_t(w) * h * 3, 0.0f);
	if (atlas.compression != SgLightmapCompression::Uncompressed) {
		const size_t blockBytes = size_t(w / 4) * (h / 4) * 16;
		if (atlas.bc6hLobes < 1 || atlas.bc6hBlocks.size() < blockBytes) {
			err = "compressed atlas has no lobe-0 blocks";
			return false;
		}
		std::vector<uint16_t> halves;
		Bc6hDecodeImage(atlas.bc6hBlocks.data(), w, h, halves);
		for (size_t i = 0; i < size_t(w) * h; ++i)
			for (int ch = 0; ch < 3; ++ch)
				lm[i * 3 + ch] = HalfToFloat(halves[i * 3 + ch]);
	} else {
		for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
			const SgLightmapChunk& c = atlas.chunks[ci];
			const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
			const int ox = int(c.rectMin[0]);
			const int oy = int(c.rectMin[1]);
			if (c.lobeCount == 0 || p.width == 0 || p.height == 0
				|| ox < 0 || oy < 0
				|| ox + int(p.width) > int(w)
				|| oy + int(p.height) > int(h)
				|| p.coeffs.size()
					!= size_t(p.width) * p.height * c.lobeCount)
				continue;
			for (uint32_t y = 0; y < p.height; ++y)
				for (uint32_t x = 0; x < p.width; ++x) {
					const glm::vec3& a =
						p.coeffs[(size_t(y) * p.width + size_t(x))
							* c.lobeCount];
					float* dst = &lm[(size_t(oy + int(y)) * w
						+ size_t(ox + int(x))) * 3];
					dst[0] = a.r;
					dst[1] = a.g;
					dst[2] = a.b;
				}
		}
	}

	// Chunk classification: bounds + unbaked (zero BAKE POINTS — the
	// chart rasterized nothing; the payload stays zero with nothing
	// to dilate). Chunks that baked but traced zero light are
	// legitimate darkness, not faults: hasBakePoints (cook's per-chart
	// texel counts) separates them; a size-mismatched mask falls back
	// to the payload-zero heuristic.
	const bool haveMask = hasBakePoints.size() == atlas.chunks.size();
	size_t skipped = 0, unbakedChunks = 0;
	std::vector<char> inBounds(atlas.chunks.size(), 0);
	std::vector<char> isUnbaked(atlas.chunks.size(), 0);
	for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
		const SgLightmapChunk& c = atlas.chunks[ci];
		const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
		const int ox = int(c.rectMin[0]);
		const int oy = int(c.rectMin[1]);
		if (c.lobeCount == 0 || p.width == 0 || p.height == 0
			|| ox < 0 || oy < 0
			|| ox + int(p.width) > int(w)
			|| oy + int(p.height) > int(h)
			|| p.coeffs.size()
				!= size_t(p.width) * p.height * c.lobeCount) {
			++skipped;
			continue;
		}
		inBounds[ci] = 1;
		if (haveMask && hasBakePoints[ci] == 0) {
			isUnbaked[ci] = 1;
			++unbakedChunks;
			continue;
		}
		bool anyNonZero = false;
		for (const glm::vec3& a : p.coeffs)
			if (a.x != 0.0f || a.y != 0.0f || a.z != 0.0f) {
				anyNonZero = true;
				break;
			}
		if (!haveMask && !anyNonZero) {
			isUnbaked[ci] = 1;
			++unbakedChunks;
		}
	}

	// Paint the true loaded lightmap; corrupt (non-finite) texels in
	// the AS-LOADED image turn pink per texel. Compressed atlases
	// paint the full decoded image (every texel is loaded, prefill
	// halo included); uncompressed atlases paint the editor clear
	// color outside the chunk rects — the atlas dump's background, so
	// the two PNGs stay byte-comparable.
	std::vector<uint8_t> px(size_t(w) * h * 4);
	static const uint8_t kPink[4] = { 255, 0, 255, 255 };
	uint32_t corruptTexels = 0;
	const auto paintTexel = [&](size_t i) {
		const float r = lm[i * 3 + 0], g = lm[i * 3 + 1],
			b = lm[i * 3 + 2];
		if (!std::isfinite(r) || !std::isfinite(g)
			|| !std::isfinite(b)) {
			std::memcpy(&px[i * 4], kPink, 4);
			++corruptTexels;
			return;
		}
		px[i * 4 + 0] = DisplayByte(r);
		px[i * 4 + 1] = DisplayByte(g);
		px[i * 4 + 2] = DisplayByte(b);
		px[i * 4 + 3] = 255;
	};
	if (atlas.compression != SgLightmapCompression::Uncompressed) {
		for (size_t i = 0; i < size_t(w) * h; ++i)
			paintTexel(i);
	} else {
		const uint8_t bg[4] = { DisplayByte(0.02f), DisplayByte(0.02f),
			DisplayByte(0.03f), 255 };
		for (size_t i = 0; i < size_t(w) * h; ++i)
			std::memcpy(&px[i * 4], bg, 4);
		for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
			if (!inBounds[ci])
				continue;
			const SgLightmapChunk& c = atlas.chunks[ci];
			const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
			const int ox = int(c.rectMin[0]);
			const int oy = int(c.rectMin[1]);
			for (uint32_t y = 0; y < p.height; ++y)
				for (uint32_t x = 0; x < p.width; ++x)
					paintTexel(size_t(oy + int(y)) * w
						+ size_t(ox + int(x)));
		}
	}

	// Unbaked rects paint solid pink over the loaded content.
	for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
		if (!inBounds[ci] || !isUnbaked[ci])
			continue;
		const SgLightmapChunk& c = atlas.chunks[ci];
		const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
		const int ox = int(c.rectMin[0]);
		const int oy = int(c.rectMin[1]);
		for (uint32_t y = 0; y < p.height; ++y) {
			uint8_t* row = &px[(size_t(oy + int(y)) * w
				+ size_t(ox)) * 4];
			for (uint32_t x = 0; x < p.width; ++x) {
				std::memcpy(row, kPink, 4);
				row += 4;
			}
		}
	}

	// Pairwise rect-overlap intersections paint pink: these are the
	// exact texels the sequential GPU copy overwrites. Strict
	// intersection (edge-touching rects share no pixels and stay
	// clean), same predicate as the cook gates.
	size_t overlapPairs = 0;
	for (size_t i = 0; i < atlas.chunks.size(); ++i) {
		if (!inBounds[i])
			continue;
		const SgLightmapChunk& a = atlas.chunks[i];
		for (size_t j = i + 1; j < atlas.chunks.size(); ++j) {
			if (!inBounds[j])
				continue;
			const SgLightmapChunk& b = atlas.chunks[j];
			if (!(a.rectMin[0] < b.rectMax[0]
					&& b.rectMin[0] < a.rectMax[0]
					&& a.rectMin[1] < b.rectMax[1]
					&& b.rectMin[1] < a.rectMax[1]))
				continue;
			++overlapPairs;
			const int ix0 = int(std::max(a.rectMin[0], b.rectMin[0]));
			const int iy0 = int(std::max(a.rectMin[1], b.rectMin[1]));
			const int ix1 = int(std::min(a.rectMax[0], b.rectMax[0]));
			const int iy1 = int(std::min(a.rectMax[1], b.rectMax[1]));
			for (int y = iy0; y < iy1; ++y) {
				uint8_t* row = &px[(size_t(y) * w + size_t(ix0)) * 4];
				for (int x = ix0; x < ix1; ++x) {
					std::memcpy(row, kPink, 4);
					row += 4;
				}
			}
		}
	}
	if (!stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(),
			int(w) * 4)) {
		err = "stbi_write_png failed for " + path;
		return false;
	}
	std::cerr << "[LM-PNG] pink: " << unbakedChunks << " unbaked, "
		<< corruptTexels << " corrupt texels, " << overlapPairs
		<< " overlapping pairs, " << skipped << " skipped\n" << std::flush;
	return true;
}

} // namespace pakcook
