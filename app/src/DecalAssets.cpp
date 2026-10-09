// App demo decal assets: the procedural blood-splat textures + the
// sponza preset splats. APP-HOST content (was WorldRenderer demo state):
// the textures register into the world's DYNAMIC cluster texture pool,
// so decal entities reference them by URI exactly like cooked content
// (see SceneDecal + the kDecalBlood*Uri contract in Engine/Scene/scene.h).

#include "Host.h"

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/GI/bake.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Scene/scene.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace app {

bool InitDemoDecalTextures(smvk::ResourceManager& ctx, smvk::World& world)
{
	smvk::Cluster& dyn = world.EnsureDynamicCluster(&ctx);
	// Idempotent: the dynamic cluster keeps the URIs for the world's
	// lifetime; a second call (recook, re-run) is a no-op.
	if (dyn.FindTextureByUri(::kDecalBloodAlbedoUri).has_value())
		return true;
	// Procedural blood splat: metaball blob field + radial droplet
	// comet tails -> coverage alpha; color darkens with thickness
	// (pooled core) and the normal map derives from the blurred field
	// (coagulated surface bump). Deterministic Pcg32 seed so the check
	// scene renders identically every run.
	constexpr int S = 512;
	smvk::gi::Pcg32 rng(0xB100D5EEDull);
	struct Blob { float x, y, r, amp; };
	std::vector<Blob> blobs;
	const int blobCount = 9;
	blobs.reserve(blobCount + 28);
	for (int i = 0; i < blobCount; ++i) {
		Blob b{};
		b.x = (rng.Float() * 2.0f - 1.0f) * 0.16f;
		b.y = (rng.Float() * 2.0f - 1.0f) * 0.16f;
		b.r = 0.10f + rng.Float() * 0.13f;
		b.amp = 0.7f + rng.Float() * 0.6f;
		blobs.push_back(b);
	}
	// Droplets: heads at a radial distance, tails stretched outward.
	for (int i = 0; i < 26; ++i) {
		const float ang = rng.Float() * 6.2831853f;
		const float dist = 0.22f + rng.Float() * 0.55f;
		const float rad = 0.012f + rng.Float() * 0.035f;
		const float ca = std::cos(ang), sa = std::sin(ang);
		// Head + tail pair along the radial direction (comet).
		for (int k = 0; k < 2; ++k) {
			Blob b{};
			const float d = dist - k * rad * 1.6f;
			b.x = ca * d;
			b.y = sa * d;
			b.r = rad * (k == 0 ? 1.0f : 0.62f);
			b.amp = k == 0 ? 0.85f : 0.55f;
			blobs.push_back(b);
		}
	}

	std::vector<float> field(size_t(S) * S, 0.0f);
	for (int y = 0; y < S; ++y) {
		for (int x = 0; x < S; ++x) {
			const float u = (float(x) / float(S - 1)) * 2.0f - 1.0f;
			const float v = (float(y) / float(S - 1)) * 2.0f - 1.0f;
			float f = 0.0f;
			for (const Blob& b : blobs) {
				const float dx = u - b.x, dy = v - b.y;
				const float d2 = dx * dx + dy * dy;
				f += b.amp * std::exp(-d2 / (2.0f * b.r * b.r));
			}
			field[size_t(y) * S + x] = f;
		}
	}

	const auto srgb8 = [](float c) {
		c = std::min(std::max(c, 0.0f), 1.0f);
		const float lo = c * 12.92f;
		const float hi = 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		const float val = c <= 0.0031308f ? lo : hi;
		return static_cast<uint8_t>(std::lround(val * 255.0f));
	};
	const auto smooth01 = [](float e0, float e1, float x) {
		const float t = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};

	std::vector<uint8_t> albedo(size_t(S) * S * 4);
	std::vector<uint8_t> normal(size_t(S) * S * 4);
	for (int y = 0; y < S; ++y) {
		for (int x = 0; x < S; ++x) {
			const size_t i = size_t(y) * S + x;
			const float f = field[i];
			const float alpha = smooth01(0.5f, 0.62f, f);
			// Linear blood color: darker where thick (pooled core),
			// brighter at the thin rim. The low roughness target
			// supplies the wet specular read.
			const float thin = 1.0f - alpha;
			glm::vec3 c(
				0.055f + 0.105f * thin,
				0.0025f + 0.0055f * thin,
				0.002f + 0.003f * thin);
			uint8_t* o = &albedo[i * 4];
			o[0] = srgb8(c.r);
			o[1] = srgb8(c.g);
			o[2] = srgb8(c.b);
			o[3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));

			// Normal from the field height (Sobel, 2-texel reach so
			// the bump survives 0.5-scale splats). Edge-clamped.
			const auto hAt = [&](int xx, int yy) {
				return field[size_t(std::clamp(yy, 0, S - 1)) * S
					+ std::clamp(xx, 0, S - 1)];
			};
			const float bump = 0.55f;
			float nx = (hAt(x - 2, y) - hAt(x + 2, y)) * bump;
			float ny = (hAt(x, y - 2) - hAt(x, y + 2)) * bump;
			const float len = std::sqrt(nx * nx + ny * ny + 1.0f);
			nx /= len;
			ny /= len;
			const float nz = 1.0f / len;
			uint8_t* nOut = &normal[i * 4];
			nOut[0] = static_cast<uint8_t>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
			nOut[1] = static_cast<uint8_t>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
			nOut[2] = static_cast<uint8_t>(std::lround((nz * 0.5f + 0.5f) * 255.0f));
			nOut[3] = 255;
		}
	}

	smvk::ImageCreateInfo ci{};
	ci.extent = { (uint32_t)S, (uint32_t)S, 1 };
	ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
	ci.mipLevels = 1;
	ci.arrayLayers = 1;

	ci.format = smvk::Format::RGBA8_SRGB; // sampling lands in linear
	ci.name = "decalBloodAlbedo";
	auto albedoTex = ctx.CreateTexture(ci, albedo.data());
	ci.format = smvk::Format::RGBA8_UNorm; // normals stay raw
	ci.name = "decalBloodNormal";
	auto normalTex = ctx.CreateTexture(ci, normal.data());
	if (albedoTex.empty() || normalTex.empty()) {
		std::cerr << "[Decals] splat texture creation failed\n";
		return false;
	}
	// Pool entries own the Holders' lifetime; the URI map carries the RHI
	// handle decal entities resolve at use (the light-cookie pattern).
	const smvk::TextureHandle albedoH = albedoTex;
	const smvk::TextureHandle normalH = normalTex;
	dyn.textures.create(std::move(albedoTex));
	dyn.textures.create(std::move(normalTex));
	dyn.RegisterTextureUri(::kDecalBloodAlbedoUri, albedoH);
	dyn.RegisterTextureUri(::kDecalBloodNormalUri, normalH);
	ctx.FlushUploads();
	std::cerr << "[Decals] blood splat textures ready (dynamic cluster)\n";
	return true;
}

void SpawnSponzaDecalPresets(smvk::World& world)
{
	if (!world.decalLocations.empty())
		return; // already carrying content (e.g. a reload in place)

	const auto addSplat = [&world](glm::vec3 center, glm::vec3 axisZ,
		glm::vec3 upHint, glm::vec3 half, float opacity, uint32_t seed,
		const char* name) {
		::SceneDecal def{};
		def.albedoUri = std::string(::kDecalBloodAlbedoUri);
		def.normalUri = std::string(::kDecalBloodNormalUri);
		def.opacity = opacity;
		def.seed = seed;
		def.name = name;
		world.CreateDecalAt(::DecalNodeFrame(center, axisZ, upHint, half),
			std::move(def));
	};
	// Floor pools down the +X corridor arm (default camera looks +X
	// from (0,1,0); horizon-level view, so the pools sit in the lower
	// band of the frame). Floor plane is y = 0; the box hovers 0.5 m above
	// it, half depth 0.75 m -> |local.z| ~0.67 at the floor, inside
	// the depth fade, and the shallower box limits catches on
	// mid-air clutter (banners, lamp chains) inside the volume.
	addSplat(glm::vec3(4.3f, 0.5f, -0.2f), glm::vec3(0, -1, 0),
		glm::vec3(0, 1, 0), glm::vec3(0.6f, 0.6f, 0.75f), 1.0f, 11u,
		"sponza_pool_a");
	addSplat(glm::vec3(3.5f, 0.5f, -1.35f), glm::vec3(0, -1, 0),
		glm::vec3(0, 1, 0), glm::vec3(0.42f, 0.42f, 0.75f), 0.85f, 23u,
		"sponza_pool_b");
	addSplat(glm::vec3(5.4f, 0.5f, 1.30f), glm::vec3(0, -1, 0),
		glm::vec3(0, 1, 0), glm::vec3(0.55f, 0.55f, 0.75f), 0.90f, 37u,
		"sponza_pool_c");
	// Wall smear on the corridor's far end wall (screen-center at
	// horizon level: the same surface the crosshair spawner hits —
	// deep box so the end-arch band lands inside it; the depth fade
	// keeps any double projection on deeper returns faint).
	addSplat(glm::vec3(10.9f, 1.6f, 0.2f), glm::vec3(1, 0, 0),
		glm::vec3(0, 1, 0), glm::vec3(1.0f, 1.0f, 1.6f), 1.0f, 53u,
		"sponza_wall_smear");
	// Side-wall spray on the +z arcade (screen-right): the box reaches
	// the arcade/wall band; curved + occluded geometry means this one
	// reads as a fainter secondary spray.
	addSplat(glm::vec3(7.6f, 1.7f, 4.95f), glm::vec3(0, 0, 1),
		glm::vec3(0, 1, 0), glm::vec3(0.8f, 0.8f, 0.85f), 0.95f, 71u,
		"sponza_arcade_spray");
	// Facade smear on the +z front wall, centered on the entrance arch:
	// the one preset squarely in the GENERIC framing view (the load-time
	// camera parks +z/high looking at the bounds center), so exterior
	// default-view runs carry a visible splat too.
	addSplat(glm::vec3(0.5f, 6.5f, 8.6f), glm::vec3(0, 0, -1),
		glm::vec3(0, 1, 0), glm::vec3(1.6f, 1.6f, 1.2f), 1.0f, 97u,
		"sponza_facade_smear");
	std::cerr << "[Decals] sponza preset entities: "
		<< world.decalLocations.size() << "\n";
}

} // namespace app
