#include <Engine/Renderer/cookies.h>

#include <Engine/Utils/texture_loader.h>

#include <algorithm>
#include <cmath>
#include <iostream>

#include <glm/glm.hpp>

namespace smvk {

std::vector<uint8_t> GenerateCookiePixels(int pattern) {
	const uint32_t S = kCookieTextureSize;
	std::vector<uint8_t> px(size_t(S) * S * 4);
	for (uint32_t y = 0; y < S; ++y) {
		for (uint32_t x = 0; x < S; ++x) {
			// [-1,1] cone UV space (V matches texture rows; patterns are
			// symmetric so the exact orientation does not matter).
			const float u = (float(x) + 0.5f) / float(S) * 2.0f - 1.0f;
			const float v = (float(y) + 0.5f) / float(S) * 2.0f - 1.0f;
			const float r = std::sqrt(u * u + v * v);

			float val = 0.0f;
			glm::vec3 tint(1.0f);
			switch (pattern) {
			case 0: { // soft disc: gentle center-weighted pool of light
				val = 1.0f - glm::smoothstep(0.45f, 0.95f, r);
				tint = { 1.0f, 0.92f, 0.80f };
				break;
			}
			case 1: { // window blinds: horizontal slats, cool daylight
				const float slat = (v + 1.0f) * 0.5f * 6.0f;
				const float f = slat - std::floor(slat);
				val = glm::smoothstep(0.30f, 0.55f, f);
				val *= 1.0f - glm::smoothstep(0.70f, 0.98f, r);
				tint = { 0.95f, 0.97f, 1.0f };
				break;
			}
			default: { // starburst: warm spokes + hot core
				const float a = std::atan2(v, u);
				val = std::pow(std::abs(std::cos(a * 4.0f)), 12.0f);
				val *= 1.0f - glm::smoothstep(0.40f, 0.95f, r);
				val += 0.25f * (1.0f - glm::smoothstep(0.0f, 0.35f, r));
				tint = { 1.0f, 0.85f, 0.60f };
				break;
			}
			}

			const glm::vec3 c = tint * std::min(val, 1.0f);
			const size_t i = (size_t(y) * S + x) * 4;
			px[i + 0] = (uint8_t)(std::clamp(c.r, 0.0f, 1.0f) * 255.0f);
			px[i + 1] = (uint8_t)(std::clamp(c.g, 0.0f, 1.0f) * 255.0f);
			px[i + 2] = (uint8_t)(std::clamp(c.b, 0.0f, 1.0f) * 255.0f);
			px[i + 3] = 255;
		}
	}
	return px;
}

std::string CookieVfsPath(int pattern) {
	switch (pattern) {
	case 0: return "textures/cookies/cookie_disc.ktx2";
	case 1: return "textures/cookies/cookie_blinds.ktx2";
	default: return "textures/cookies/cookie_starburst.ktx2";
	}
}

const char* CookieDebugName(int pattern) {
	switch (pattern) {
	case 0: return "cookie_disc";
	case 1: return "cookie_blinds";
	default: return "cookie_starburst";
	}
}

bool CookieLibrary::Valid() const {
	for (int i = 0; i < kCookiePatternCount; ++i)
		if (bindless[i] >= 0 && !textures[i].empty())
			return true;
	return false;
}

bool CookieLibrary::Ensure(ResourceManager& ctx, const OverlayVfs& vfs) {
	bool any = false;
	bool didWork = false;
	for (int i = 0; i < kCookiePatternCount; ++i) {
		if (bindless[i] >= 0) {
			any = true;
			continue;
		}
		didWork = true;
		const std::string path = CookieVfsPath(i);
		if (auto bytes = vfs.TryRead(path)) {
			auto tex = LoadKtxTextureMemory(
				ctx, bytes->data(), bytes->size(), CookieDebugName(i));
			if (!tex.empty()) {
				baked[i] = true;
				bindless[i] = static_cast<int32_t>(ctx.GetTextureBindlessIndex(tex));
				textures[i] = std::move(tex);
				any = true;
				continue;
			}
			std::cerr << "[cookies] baked file failed to decode: "
				<< path << std::endl;
		}
		// Fallback: procedural gobo (identical math to the baker's base
		// mip, single mip level like the pre-bake runtime path).
		std::vector<uint8_t> pixels = GenerateCookiePixels(i);
		ImageCreateInfo ci{};
		ci.format = Format::RGBA8_UNorm;
		ci.extent = { kCookieTextureSize, kCookieTextureSize, 1u };
		ci.mipLevels = 1u;
		ci.arrayLayers = 1u;
		ci.usage = ImageUsage::Sampled | ImageUsage::TransferDst;
		ci.name = CookieDebugName(i);
		auto tex = ctx.CreateTexture(ci, pixels.data());
		if (tex.empty()) {
			std::cerr << "[cookies] pattern " << i
				<< " unavailable (no baked file, procedural upload failed)"
				<< std::endl;
			continue;
		}
		baked[i] = false;
		bindless[i] = static_cast<int32_t>(ctx.GetTextureBindlessIndex(tex));
		textures[i] = std::move(tex);
		any = true;
	}
	if (didWork) {
		size_t bakedCount = 0;
		for (int i = 0; i < kCookiePatternCount; ++i)
			bakedCount += baked[i] ? 1 : 0;
		std::cout << "[cookies] " << bakedCount << "/" << kCookiePatternCount
			<< " patterns loaded from baked files"
			<< (bakedCount == kCookiePatternCount ? "" : " (rest procedural)")
			<< std::endl;
	}
	return any;
}

} // namespace smvk
