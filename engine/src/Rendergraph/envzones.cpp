// Tiled environment-zone map (see Rendergraph/envzones.h): CPU grid
// build/paint/sample + the GPU texture/bindless registration.

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
	// Default fill: zone 0 everywhere (R = 255, G/B/A = 0) — the plain
	// env look until something paints.
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
	// under a fresh bindless index — the frame-uniforms fill re-reads the
	// member every frame, so consumers follow the new slot for free.
	// 400x400 RGBA8 = 640 KB per paint cadence — cheap.
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

void EnvZoneMap::Paint(float wx, float wz, uint32_t zone, float radiusM,
	float strength) {
	if (!valid || zone >= kEnvZoneMax || !(radiusM > 0.0f))
		return;
	strength = std::clamp(strength, -1.0f, 1.0f);

	// Brush box in texel space (rounded outward), smoothstep falloff from
	// the stamp center, additive onto the zone's channel then renormalized
	// (the 8-bit channels must sum to 255 or the shader's normalization
	// rescales the mix anyway — keeping the sum exact avoids quantization
	// drift of the dominant channels).
	float cx, cz;
	WorldToTexel(wx, wz, cx, cz);
	const float rTex = radiusM / desc.texelSize;
	const int32_t x0 = int32_t(std::floor(cx - rTex));
	const int32_t x1 = int32_t(std::ceil(cx + rTex));
	const int32_t z0 = int32_t(std::floor(cz - rTex));
	const int32_t z1 = int32_t(std::ceil(cz + rTex));
	const int shift = int(zone) * 8;
	for (int32_t z = std::max(z0, 0); z <= std::min(z1, int32_t(dim) - 1); ++z) {
		for (int32_t x = std::max(x0, 0); x <= std::min(x1, int32_t(dim) - 1); ++x) {
			const float dx = (float(x) + 0.5f) - cx;
			const float dz = (float(z) + 0.5f) - cz;
			const float dist = std::sqrt(dx * dx + dz * dz);
			const float t = std::clamp(1.0f - dist / std::max(rTex, 1e-3f), 0.0f, 1.0f);
			const float falloff = t * t * (3.0f - 2.0f * t); // smoothstep
			uint32_t& w = weights[size_t(z) * dim + size_t(x)];
			float ch[4] = {
				float((w >> 0) & 0xFFu),
				float((w >> 8) & 0xFFu),
				float((w >> 16) & 0xFFu),
				float((w >> 24) & 0xFFu),
			};
			ch[zone] = std::clamp(ch[zone] + falloff * strength * 255.0f,
				0.0f, 255.0f);
			float sum = ch[0] + ch[1] + ch[2] + ch[3];
			if (sum < 1.0f) {
				ch[0] = 255.0f; // empty stamp: fall back to zone 0
				sum = 255.0f;
			}
			const float inv = 255.0f / sum;
			w = PackR8(ch[0] * inv) | (PackR8(ch[1] * inv) << 8)
				| (PackR8(ch[2] * inv) << 16) | (PackR8(ch[3] * inv) << 24);
		}
	}
}

EnvZone EnvZoneMap::SampleAt(float wx, float wz) const {
	EnvZone out = desc.zones[0];
	if (!valid)
		return out;
	// Bilinear over the grid (the GPU's linear filter), then the
	// normalized channel-weighted zone mix — the CPU mirror of the
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
