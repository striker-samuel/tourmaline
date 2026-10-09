// Tiled environment-zone map (see Rendergraph/envzones.h): CPU grid
// build/rasterize/sample + the GPU texture/bindless registration.

#include <Engine/Rendergraph/envzones.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace smvk::envzones {

namespace {

inline uint32_t PackR8(float w) {
	const float c = std::clamp(w, 0.0f, 1.0f);
	return uint32_t(std::lround(c * 255.0f));
}

} // namespace

bool EnvZoneMap::Build(ResourceManager& ctx, const EnvZoneDesc& d) {
	Destroy();
	if (!(d.worldSize > 0.0f) || !(d.texelSize > 0.0f))
		return false;
	const float dimF = d.worldSize / d.texelSize;
	const uint32_t n = uint32_t(std::lround(dimF));
	if (n < 2 || n > 4096 || std::fabs(float(n) - dimF) > 1e-3f)
		return false;

	desc = d;
	dim = n;
	// Default fill: zone 0 everywhere (R = 255, G/B/A = 0) Ã¢â‚¬â€ the plain
	// env look until a zone entity covers the texel.
	weights.assign(size_t(n) * n, 0xFF000000u);

	// GPU texture: RGBA8_UNORM, sampled, one mip; uploaded from the grid
	// (row-major u32 == RGBA8 bytes little-endian R-first).
	smvk::ImageCreateInfo ci{};
	ci.type = smvk::ImageType::Image2D;
	ci.extent = { n, n, 1 };
	ci.format = smvk::Format::RGBA8_UNorm;
	ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.name = "env zone map";
	texture = ctx.CreateTexture(ci, weights.data());
	if (texture.empty()) {
		std::fprintf(stderr, "[envzones] texture creation failed\n");
		Destroy();
		return false;
	}
	bindlessIndex = ctx.GetTextureBindlessIndex(texture);
	if (bindlessIndex == UINT32_MAX) {
		std::fprintf(stderr, "[envzones] bindless registration failed\n");
		Destroy();
		return false;
	}
	valid = true;
	std::fprintf(stderr, "[envzones] built: %ux%u texels of %.0fm "
		"(%.0fm world, bindless slot %u)\n",
		n, n, d.texelSize, d.worldSize, bindlessIndex);
	return true;
}

void EnvZoneMap::Destroy() {
	texture = {};
	bindlessIndex = UINT32_MAX;
	valid = false;
	weights.clear();
	dim = 0;
}

void EnvZoneMap::Upload(ResourceManager& ctx) {
	// Content refresh through the RHI's recreate-based UpdateTexture: the
	// old texture's pool generation BUMPS (stale holders/captured bindless
	// indices are detectably dead) and a fresh texture reseats `texture`
	// under a fresh bindless index Ã¢â‚¬â€ the frame-uniforms fill re-reads the
	// member every frame, so consumers follow the new slot for free.
// 400x400 RGBA8 = 640 KB per rebuild cadence - cheap.
	if (!valid || dim == 0 || weights.empty())
		return;
	smvk::ImageCreateInfo ci{};
	ci.type = smvk::ImageType::Image2D;
	ci.extent = { dim, dim, 1 };
	ci.format = smvk::Format::RGBA8_UNorm;
	ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.name = "env zone map";
	const uint32_t slot = ctx.UpdateTexture(texture, ci, weights.data());
	if (slot == UINT32_MAX)
		return;
	bindlessIndex = slot;
}

void EnvZoneMap::WorldToTexel(float wx, float wz,
	float& outX, float& outZ) const {
	// World (min corner at -worldSize/2) -> continuous texel coords
	// (texel centers at i + 0.5).
	outX = (wx + desc.worldSize * 0.5f) / desc.texelSize - 0.5f;
	outZ = (wz + desc.worldSize * 0.5f) / desc.texelSize - 0.5f;
}

glm::vec4 EnvZoneMap::WeightsAtTexel(float x, float z) const {
	const int32_t xi = int32_t(std::floor(std::clamp(x, 0.0f, float(dim) - 1.0f)));
	const int32_t zi = int32_t(std::floor(std::clamp(z, 0.0f, float(dim) - 1.0f)));
	const uint32_t w = weights[size_t(zi) * dim + size_t(xi)];
	return glm::vec4(
		float((w >> 0) & 0xFFu) / 255.0f,
		float((w >> 8) & 0xFFu) / 255.0f,
		float((w >> 16) & 0xFFu) / 255.0f,
		float((w >> 24) & 0xFFu) / 255.0f);
}

EnvZone EnvZoneMap::SampleAt(float wx, float wz) const {
	EnvZone out = desc.zones[0];
	if (!valid)
		return out;
	// Bilinear over the grid (the GPU's linear filter), then the
	// normalized channel-weighted zone mix Ã¢â‚¬â€ the CPU mirror of the
	// shader blend (EnvZoneWeights + the envZone* FrameUniforms block).
	float x, z;
	WorldToTexel(wx, wz, x, z);
	x = std::clamp(x, 0.0f, float(dim) - 1.0f);
	z = std::clamp(z, 0.0f, float(dim) - 1.0f);
	const int32_t xi = int32_t(std::floor(x));
	const int32_t zi = int32_t(std::floor(z));
	const int32_t xj = std::min(xi + 1, int32_t(dim) - 1);
	const int32_t zj = std::min(zi + 1, int32_t(dim) - 1);
	const float fx = x - float(xi);
	const float fz = z - float(zi);
	const glm::vec4 w = WeightsAtTexel(float(xi), float(zi)) * (1.0f - fx) * (1.0f - fz)
		+ WeightsAtTexel(float(xj), float(zi)) * fx * (1.0f - fz)
		+ WeightsAtTexel(float(xi), float(zj)) * (1.0f - fx) * fz
		+ WeightsAtTexel(float(xj), float(zj)) * fx * fz;
	float s = w.x + w.y + w.z + w.w;
	if (s < 1e-4f)
		return out;
	const glm::vec4 wn = w / s;
	glm::vec4 sky(0.0f);
	glm::vec4 fog(0.0f);
	glm::vec4 vol(0.0f);
	for (uint32_t i = 0; i < kEnvZoneMax; ++i) {
		const EnvZone& zdef = desc.zones[i];
		const float wi = wn[i];
		sky += glm::vec4(zdef.skyTint, zdef.ambientI) * wi;
		fog += glm::vec4(zdef.fogColor, zdef.fogDensity) * wi;
		vol += glm::vec4(zdef.volumetrics, zdef.volumetricDensity,
			0.0f, 0.0f) * wi;
	}
	out.skyTint = glm::vec3(sky);
	out.ambientI = sky.w;
	out.fogColor = glm::vec3(fog);
	out.fogDensity = fog.w;
	out.volumetrics = vol.x;
	out.volumetricDensity = vol.y;
	return out;
}

} // namespace smvk::envzones
