#pragma once
// Canonical artifact-kind vocabulary (the funnel's storage classes).
//
// ONE place owns the kind strings: the per-class pak file names derive
// here (pak/<kind>.pak), the registry validates against this list
// (typo'd kinds fail closed instead of silently minting orphan paks),
// and the klass->TOC-kind mapping lives here so every PakBackend mount
// serves the same kinds. Adding a kind = one row below + its TOC rule.
//
// Vocabulary (frozen — removing a kind breaks existing class paks):
//   texture | mesh | material | animation | light | camera | scene | gi
// gi covers ALL GI byproducts (sggi/cvol/regular/cubemap/lightmap rows,
// keyed by canonical VFS uri) — no per-subtype classes; the uri already
// disambiguates and one gi.pak keeps the file count flat. There is
// deliberately no `level` klass: level blobs are pack-time assemblies
// (scene + byproduct rows), composed at pack like today — caching a pack
// output inside the pack inputs would invert the funnel.

#include <string>

#include <Engine/Asset/pak.h>

namespace smvk::cache {

// Storage classes (see table above). The literals stay inline at the
// (few) call sites that already spell them; this list is the registry
// of record for validation + mapping.
inline constexpr const char* kKindTexture = "texture";
inline constexpr const char* kKindMesh = "mesh";
inline constexpr const char* kKindMaterial = "material";
inline constexpr const char* kKindAnimation = "animation";
inline constexpr const char* kKindLight = "light";
inline constexpr const char* kKindCamera = "camera";
inline constexpr const char* kKindScene = "scene";
inline constexpr const char* kKindGi = "gi";

// Closed-vocabulary check (registry + store fail closed on unknown).
inline bool IsValidArtifactKind(const std::string& klass) {
	return klass == kKindTexture || klass == kKindMesh
		|| klass == kKindMaterial || klass == kKindAnimation
		|| klass == kKindLight || klass == kKindCamera
		|| klass == kKindScene || klass == kKindGi;
}

// klass -> the class pak's TOC kind (decides what a PakBackend mount
// serves; textures must be Texture to ride the VFS path). GI payloads
// MUST be Texture for the same reason. Animation has no dedicated TOC
// kind and no path-serving consumer yet: Texture keeps the bytes opaque
// and mountable; revisit with a real PakAssetKind when one exists.
inline bool ArtifactKindToPakKind(const std::string& klass,
	smvk::PakAssetKind& out) {
	if (klass == kKindTexture) { out = smvk::PakAssetKind::Texture; return true; }
	if (klass == kKindMesh) { out = smvk::PakAssetKind::Mesh; return true; }
	if (klass == kKindMaterial) { out = smvk::PakAssetKind::Material; return true; }
	if (klass == kKindAnimation) { out = smvk::PakAssetKind::Texture; return true; }
	if (klass == kKindLight) { out = smvk::PakAssetKind::Light; return true; }
	if (klass == kKindCamera) { out = smvk::PakAssetKind::Camera; return true; }
	if (klass == kKindScene) { out = smvk::PakAssetKind::Scene; return true; }
	if (klass == kKindGi) { out = smvk::PakAssetKind::Texture; return true; }
	return false;
}

// Per-class pak file path under a store root (the folders-for-schema:
// <root>/pak/<klass>.pak — exactly one file per frozen kind).
inline std::string ArtifactPakFileName(const std::string& klass) {
	return std::string("pak/") + klass + ".pak";
}

} // namespace smvk::cache
