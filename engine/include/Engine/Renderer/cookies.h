#pragma once
// Light cookies (gobos) for spot lights: baked asset files + runtime loader.
//
// Design:
//  - The gobo patterns (soft disc / window blinds / starburst) are baked
//    offline by tools/cookie_baker into uncompressed RGBA8 KTX2 files with a
//    full mip chain. The baked files live in engine/resources/textures/
//    cookies/ and are resolved through the VFS overlay like any other
//    texture, so app content or a mounted pak can override them — "pakked
//    or not" is purely a mount-order decision (PakBackend serves texture
//    blobs by their VFS path).
//  - CookieLibrary::Ensure loads each pattern through the overlay and keeps
//    the bindless texture index that ForwardPlusLight::cookieIndex needs.
//    On a VFS miss (resources not deployed) it falls back to procedural
//    generation with the same math the baker uses, so the spots never lose
//    their cookies in dev builds.
//  - GenerateCookiePixels is the single source of truth for the pattern
//    math: shared verbatim by the runtime fallback and the offline baker.

#include <cstdint>
#include <string>
#include <vector>

#include <Engine/RHI/resource_manager.h>
#include <Engine/Vfs/vfs.h>

namespace smvk {

// RGBA8, linear, sampled through the spot cone UV (see light.hlsli
// SpotConeUV). Pattern ids are stable: 0 = soft disc, 1 = window blinds,
// 2 = starburst.
inline constexpr int kCookiePatternCount = 3;
inline constexpr uint32_t kCookieTextureSize = 256;

// Base-mip pixels (RGBA8 linear) of one procedural pattern. Shared by the
// runtime fallback and the offline baker.
std::vector<uint8_t> GenerateCookiePixels(int pattern);

// VFS path of the baked pattern file, e.g. "textures/cookies/cookie_disc.ktx2".
std::string CookieVfsPath(int pattern);

// Vulkan debug name for the pattern's texture ("cookie_disc" ...).
const char* CookieDebugName(int pattern);

// Per-pattern textures + bindless indices for one set of spot cookies.
// Ensure() is idempotent; Holders release the GPU resources on destruction.
struct CookieLibrary {
	Holder<TextureHandle> textures[kCookiePatternCount];
	int32_t bindless[kCookiePatternCount] = { -1, -1, -1 };
	// true when the texture came from a baked VFS file (vs the procedural
	// fallback) — diagnostics only.
	bool baked[kCookiePatternCount] = { false, false, false };

	// Loads every pattern through the overlay (first-match-wins across the
	// mounted backends) and resolves its bindless index. Returns true when
	// at least one pattern is available.
	bool Ensure(ResourceManager& ctx, const OverlayVfs& vfs);

	int32_t Bindless(int pattern) const {
		return (pattern >= 0 && pattern < kCookiePatternCount) ? bindless[pattern] : -1;
	}
	bool Valid() const;
};

} // namespace smvk
