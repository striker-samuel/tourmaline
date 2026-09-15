#include "lm_denoise.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#if VULKANTOY_OIDN
#include <OpenImageDenoise/OIDN.h>
#endif

namespace pakcook {

#if !VULKANTOY_OIDN

bool DenoiseAvailable() {
	return false;
}

bool DenoiseLightmapAtlas(smvk::gi::SgLightmapAtlas&, DenoiseQuality) {
	return false;
}

#else

namespace {

void OidnError(void*, OIDNError code, const char* message) {
	std::cerr << "[LM-DN] oidn error " << int(code) << ": "
		<< (message ? message : "") << "\n" << std::flush;
}

// Island-context dilation: chart rects sit in ~4-texel pack gutters, so
// the assembled image is otherwise surrounded by black void — OIDN's
// receptive field would darken island borders. Grow covered values into
// the empty space before filtering (scatter only writes covered texels
// back, so the dilated halo is discarded). One forward+backward sweep
// pair chains ~1 texel per direction; 8 pairs reach 8 texels.
constexpr int kDilatePasses = 8;

void DilateIntoGaps(std::vector<float>& px, std::vector<uint8_t>& cover,
	uint32_t w, uint32_t h) {
	auto fillFrom = [&](size_t dst, size_t src) {
		px[dst * 3 + 0] = px[src * 3 + 0];
		px[dst * 3 + 1] = px[src * 3 + 1];
		px[dst * 3 + 2] = px[src * 3 + 2];
		cover[dst] = 2; // 2 = dilated halo, never scattered back
	};
	for (int pass = 0; pass < kDilatePasses; ++pass) {
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t i = size_t(y) * w + x;
				if (cover[i])
					continue;
				if (x > 0 && cover[i - 1])
					fillFrom(i, i - 1);
				else if (y > 0 && cover[i - w])
					fillFrom(i, i - w);
			}
		}
		for (int64_t yy = int64_t(h) - 1; yy >= 0; --yy) {
			for (int64_t xx = int64_t(w) - 1; xx >= 0; --xx) {
				const size_t i = size_t(yy) * w + size_t(xx);
				if (cover[i])
					continue;
				if (xx + 1 < int64_t(w) && cover[i + 1])
					fillFrom(i, i + 1);
				else if (yy + 1 < int64_t(h) && cover[i + w])
					fillFrom(i, i + w);
			}
		}
	}
}

} // namespace

bool DenoiseAvailable() {
	return true;
}

bool DenoiseLightmapAtlas(smvk::gi::SgLightmapAtlas& atlas,
	DenoiseQuality quality) {
	using namespace smvk::gi;
	if (atlas.width == 0 || atlas.height == 0 || atlas.chunks.empty())
		return true;

	const auto t0 = std::chrono::steady_clock::now();
	const uint32_t w = atlas.width;
	const uint32_t h = atlas.height;
	const size_t texels = size_t(w) * h;

	uint32_t maxLobes = 0;
	for (const SgLightmapChunk& c : atlas.chunks)
		maxLobes = std::max(maxLobes, c.lobeCount);
	if (maxLobes == 0)
		return true;
	maxLobes = std::min<uint32_t>(maxLobes, kLightmapMaxLobes);

	// Atlas coverage from the chunk rects (packer guarantees disjoint
	// rects; a double cover is a packer bug, refuse to denoise over it).
	std::vector<uint8_t> cover(texels, 0);
	for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
		const SgLightmapChunk& c = atlas.chunks[ci];
		const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
		const int ox = int(c.rectMin[0]);
		const int oy = int(c.rectMin[1]);
		if (ox < 0 || oy < 0 || ox + int(p.width) > int(w)
			|| oy + int(p.height) > int(h)) {
			std::cerr << "[LM-DN] chunk rect out of atlas bounds\n";
			return false;
		}
		for (uint32_t y = 0; y < p.height; ++y) {
			uint8_t* row = cover.data() + size_t(oy + int(y)) * w + ox;
			for (uint32_t x = 0; x < p.width; ++x) {
				if (row[x]) {
					std::cerr << "[LM-DN] overlapping chunk rects - "
						"refusing to denoise\n";
					return false;
				}
				row[x] = 1;
			}
		}
	}

	OIDNDevice device = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
	if (!device) {
		std::cerr << "[LM-DN] oidnNewDevice failed\n";
		return false;
	}
	oidnSetDeviceErrorFunction(device, &OidnError, nullptr);
	oidnCommitDevice(device);

	std::vector<float> px(texels * 3);
	std::vector<uint8_t> dil;
	const int oidnQuality = quality == DenoiseQuality::High
		? OIDN_QUALITY_HIGH : OIDN_QUALITY_BALANCED;
	uint32_t lobesDone = 0;
	bool ok = true;
	for (uint32_t lobe = 0; lobe < maxLobes && ok; ++lobe) {
		const auto lobeT0 = std::chrono::steady_clock::now();

		// Assemble this lobe's amplitude layer (chunks with fewer lobes
		// stay empty; their texels scatter back untouched).
		std::fill(px.begin(), px.end(), 0.0f);
		for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
			const SgLightmapChunk& c = atlas.chunks[ci];
			if (lobe >= c.lobeCount)
				continue;
			const SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
			const int ox = int(c.rectMin[0]);
			const int oy = int(c.rectMin[1]);
			for (uint32_t y = 0; y < p.height; ++y) {
				const glm::vec3* src = &p.coeffs[size_t(y) * p.width * c.lobeCount + lobe];
				float* dst = &px[(size_t(oy + int(y)) * w + size_t(ox)) * 3];
				for (uint32_t x = 0; x < p.width; ++x) {
					const glm::vec3& a = src[size_t(x) * c.lobeCount];
					dst[x * 3 + 0] = a.x;
					dst[x * 3 + 1] = a.y;
					dst[x * 3 + 2] = a.z;
				}
			}
		}

		// Filter: RTLightmap, in-place float3, auto inputScale (NaN
		// default scales HDR per image), quality from the flag.
		dil = cover;
		DilateIntoGaps(px, dil, w, h);
		OIDNFilter filter = oidnNewFilter(device, "RTLightmap");
		if (!filter) {
			std::cerr << "[LM-DN] oidnNewFilter(RTLightmap) failed\n";
			ok = false;
			break;
		}
		oidnSetSharedFilterImage(filter, "color", px.data(),
			OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
		oidnSetSharedFilterImage(filter, "output", px.data(),
			OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
		oidnSetFilterInt(filter, "quality", oidnQuality);
		oidnCommitFilter(filter);
		oidnExecuteFilter(filter);
		const OIDNError err = oidnGetDeviceError(device, nullptr);
		oidnReleaseFilter(filter);
		if (err != OIDN_ERROR_NONE) {
			std::cerr << "[LM-DN] lobe " << lobe << " filter failed\n";
			ok = false;
			break;
		}

		// Scatter covered texels back into the chunk payloads.
		for (size_t ci = 0; ci < atlas.chunks.size(); ++ci) {
			const SgLightmapChunk& c = atlas.chunks[ci];
			if (lobe >= c.lobeCount)
				continue;
			SgLightmapAtlas::ChunkPayload& p = atlas.payloads[ci];
			const int ox = int(c.rectMin[0]);
			const int oy = int(c.rectMin[1]);
			for (uint32_t y = 0; y < p.height; ++y) {
				glm::vec3* dst = &p.coeffs[size_t(y) * p.width * c.lobeCount + lobe];
				const float* src = &px[(size_t(oy + int(y)) * w + size_t(ox)) * 3];
				for (uint32_t x = 0; x < p.width; ++x)
					dst[size_t(x) * c.lobeCount] =
						glm::vec3(src[x * 3 + 0], src[x * 3 + 1], src[x * 3 + 2]);
			}
		}
		++lobesDone;
		const double secs = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - lobeT0).count();
		std::cout << "  [LM-DN] lobe " << (lobe + 1) << "/" << maxLobes
			<< " (" << w << "x" << h << ", "
			<< (quality == DenoiseQuality::High ? "high" : "balanced")
			<< ") done in " << secs << "s\n" << std::flush;
	}
	oidnReleaseDevice(device);

	if (!ok)
		return false;
	const double secs = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();
	std::cout << "  [LM-DN] atlas denoised: " << lobesDone
		<< " lobe layer(s) in " << secs << "s\n" << std::flush;
	return true;
}

#endif // VULKANTOY_OIDN

} // namespace pakcook
