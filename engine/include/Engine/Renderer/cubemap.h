// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Scene cubemaps (skybox / IBL environment captures): baked asset files +
// runtime loader.
//
// Design:
//  - Environment captures are baked offline by tools/cubemap_baker into
//    KTX2 cube textures (numFaces = 6, cube-compatible 2D image, full mip
//    chain). The baked files live in engine/resources/textures/cubemaps/
//    and are resolved through the VFS overlay like any other texture, so
//    app content or a mounted pak can override them.
//  - CubemapLibrary::Load resolves one named cubemap through the overlay
//    and keeps the bindless texture index shaders sample with
//    TextureCube. On a VFS miss (resources not deployed) it falls back to
//    procedural per-face generation with the same math the baker's
//    --test-pattern mode uses, so skybox/IBL code keeps running in dev
//    builds.
//  - FaceUvToDirection is the single source of truth for the face layout:
//    shared verbatim by the runtime fallback and the offline baker. The
//    mapping follows the Vulkan/OpenGL cubemap convention (major-axis
//    face selection, s/t in [-1,1]):
//      +X: ( 1,-sv,-su)  -X: (-1,-sv, su)
//      +Y: (su,  1, sv)  -Y: (su, -1,-sv)
//      +Z: (su,-sv,  1)  -Z: (-su,-sv,-1)
//    sampled in HLSL with TextureCube.Sample(S, dir).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/RHI/resource_manager.h>
#include <Engine/Vfs/vfs.h>

namespace smvk {

// KTX/Vulkan face order (+X, -X, +Y, -Y, +Z, -Z). Layer N of the image is
// face N; CreateCubemapTexture uploads faces in this order and the baker
// writes KTX face slots in this order.
enum class CubemapFace : uint8_t {
	Px = 0,
	Nx = 1,
	Py = 2,
	Ny = 3,
	Pz = 4,
	Nz = 5,
};

inline constexpr uint32_t kCubemapDefaultSize = 256;

// Center direction of a face (for debug / placement math).
// Defined in include to keep the baker and the fallback in sync.
struct CubemapFaceVec {
	float x, y, z;
};

[[nodiscard]] inline CubemapFaceVec CubemapFaceCenter(CubemapFace face) {
	switch (face) {
	case CubemapFace::Px: return { 1.0f, 0.0f, 0.0f };
	case CubemapFace::Nx: return { -1.0f, 0.0f, 0.0f };
	case CubemapFace::Py: return { 0.0f, 1.0f, 0.0f };
	case CubemapFace::Ny: return { 0.0f, -1.0f, 0.0f };
	case CubemapFace::Pz: return { 0.0f, 0.0f, 1.0f };
	default: return { 0.0f, 0.0f, -1.0f };
	}
}

// Texel (u,v in [0,1], row-major top-left origin) -> sampling direction.
// Matches the Vulkan cube-map texel mapping so a TextureCube.Sample(dir)
// of the baked image returns the radiance the baker traced for dir.
struct CubemapDir {
	float x, y, z;
};

[[nodiscard]] inline CubemapDir FaceUvToDirection(CubemapFace face, float u, float v) {
	const float su = u * 2.0f - 1.0f;
	const float sv = v * 2.0f - 1.0f;
	switch (face) {
	case CubemapFace::Px: return { 1.0f, -sv, -su };
	case CubemapFace::Nx: return { -1.0f, -sv, su };
	case CubemapFace::Py: return { su, 1.0f, sv };
	case CubemapFace::Ny: return { su, -1.0f, -sv };
	case CubemapFace::Pz: return { su, -sv, 1.0f };
	default: return { -su, -sv, -1.0f };
	}
}

[[nodiscard]] inline const char* CubemapFaceName(CubemapFace face) {
	switch (face) {
	case CubemapFace::Px: return "+X";
	case CubemapFace::Nx: return "-X";
	case CubemapFace::Py: return "+Y";
	case CubemapFace::Ny: return "-Y";
	case CubemapFace::Pz: return "+Z";
	default: return "-Z";
	}
}

// Base-mip pixels (RGBA8 linear) of one procedural fallback face. Shared
// by the runtime fallback and the baker's test-pattern mode: per-face
// tint + UV gradient + center cross so face order/orientation bugs are
// visible.
std::vector<uint8_t> GenerateCubemapFacePixels(CubemapFace face, uint32_t size);

// VFS path of a baked cubemap, e.g. "textures/cubemaps/sky.ktx2".
std::string CubemapVfsPath(const std::string& name);

// VFS path of a baked probe manifest, e.g.
// "textures/cubemaps/sponza.cubemanifest". The manifest is written by the
// cook (PakCooker --cubemap / CubemapBaker) alongside the KTX files when a
// capture bakes MORE than one probe (grid mode): one text line per probe,
// "<vfsPath> <x> <y> <z> <influence> <fade>", positions in the same world
// meters the trace BVH uses (cook applies the scene global transforms, so
// this matches World::ComputeSceneBounds space at runtime). Single-probe
// bakes need no manifest (the runtime centers a global probe itself).
// Bake-asset rows (see CubemapManifestEntry::guid) append
// "guid <32hex>"; the runtime ignores it.
std::string CubemapManifestVfsPath(const std::string& name);

// One manifest row: which KTX to load + where its probe lives. A row may
// carry an optional 7th "pin" token (written by --cubemap-extra-pos):
// pinned rows sort before all others at load, so an artist-placed probe
// wins weight ties against overlapping baked probes (ties keep the first
// row). Old 6-token rows parse with pinned=false. Bake-asset rows carry
// a trailing "guid <32hex>" (the capture's asset GUID — see
// Engine/Asset/bake_asset.h); the runtime ignores it (it loads by path),
// but rebakes match rows by position to reuse it. Rows without one parse
// with an empty guid.
struct CubemapManifestEntry {
	std::string vfsPath;
	glm::vec3 position{ 0.0f };
	float influenceRadius = 10.0f;
	float fadeRadius = 5.0f;
	bool pinned = false;
	std::string guid; // bake-asset GUID of the capture ("" = legacy row)
};

// Serializes entries to the manifest text format (one row per line).
std::string CubemapManifestSerialize(const std::vector<CubemapManifestEntry>& entries);

// Parses manifest text; returns false + err on any malformed line.
bool CubemapManifestParse(std::string_view text,
	std::vector<CubemapManifestEntry>& out, std::string& err);

// Sampler for cubemap sampling: clamp-to-edge on all axes (cube seams
// must not wrap), linear min/mag + linear mips for prefiltered chains.
SamplerCreateInfo CubemapSampler() noexcept;

// Uploads 6 RGBA8 faces (each size*size*4, face order = CubemapFace) into
// a cube-compatible texture with an optional box-filtered mip chain.
// Returns an empty holder on bad input (logged). Data layout per mip:
// layer = face, row-major, tightly packed.
Holder<TextureHandle> CreateCubemapTexture(ResourceManager& ctx,
	const void* const faces[kCubemapFaceCount],
	uint32_t size, bool generateMips = true,
	const char* debugName = "Cubemap");

// One loaded cubemap: GPU texture + bindless TextureCube index.
struct CubemapEntry {
	Holder<TextureHandle> texture;
	int32_t bindless = -1;
	// True when the texture came from a baked VFS file (vs the
	// procedural fallback) — diagnostics only.
	bool baked = false;
	uint32_t size = 0;
	uint32_t mipLevels = 1;
};

// Reflection probe: cubemap + position + influence radius for blending.
struct ReflectionProbe {
	glm::vec3 position{ 0.0f };
	float influenceRadius = 10.0f;        // distance where probe has full weight
	float fadeRadius = 5.0f;              // additional distance for falloff
	CubemapEntry entry;                   // the cubemap data
	bool valid = false;
};

// Named reflection probe set. Load() resolves one probe per name through VFS.
struct ReflectionProbeLibrary {
	std::unordered_map<std::string, ReflectionProbe> probes;

	// Loads `name` from `vfsPath` (or CubemapVfsPath(name) when empty)
	// through the overlay. Expects a .ktx2 cubemap file.
	// `position` = world-space probe center, `influenceRadius` = full-weight distance,
	// `fadeRadius` = falloff distance (weight goes to 0 at influence+fade).
	bool Load(ResourceManager& ctx, const OverlayVfs& vfs,
		const std::string& name, const glm::vec3& position,
		float influenceRadius = 10.0f, float fadeRadius = 5.0f,
		const std::string& vfsPath = {},		uint32_t fallbackSize = kCubemapDefaultSize);

	// Get the nearest probe to a world position (returns null if none loaded).
	const ReflectionProbe* Nearest(const glm::vec3& worldPos) const;

	// Get all probes sorted by distance to worldPos (closest first).
	std::vector<const ReflectionProbe*> SortedByDistance(const glm::vec3& worldPos) const;
};

// Named cubemap set for skybox/IBL captures. Load() is idempotent per
// name; Holders release the GPU resources on destruction.
struct CubemapLibrary {
	std::unordered_map<std::string, CubemapEntry> entries;

	// Loads `name` from KTX2 `bytes` (the caller resolves the payload —
	// cluster bake channel / pak / host file) and resolves its bindless
	// index. Empty bytes or a decode that is not a cubemap falls back to
	// procedural faces (the caller decides whether a procedural entry is
	// acceptable, e.g. probes never place one). Returns true when the
	// entry is available.
	bool Load(ResourceManager& ctx, const std::string& name,
		std::span<const uint8_t> ktxBytes,
		uint32_t fallbackSize = kCubemapDefaultSize);

	int32_t Bindless(const std::string& name) const {
		auto it = entries.find(name);
		return it != entries.end() ? it->second.bindless : -1;
	}
	const CubemapEntry* Find(const std::string& name) const {
		auto it = entries.find(name);
		return it != entries.end() ? &it->second : nullptr;
	}
	bool Valid() const {
		for (const auto& [_, e] : entries)
			if (e.bindless >= 0 && !e.texture.empty())
				return true;
		return false;
	}
};

} // namespace smvk
