// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Cook-asset definitions: GI cooks + lightmap cooks as first-class project
// assets (Unity Lighting-Settings style).
//
// The problem this solves: GI bakes (GiBaker .sggi/.sgrv/.givolume) and lightmap
// cooks (PakCooker .sglm) were anonymous bake OUTPUTS — keyed by pak stem,
// configured by the transient cook dialog (m_cook/TourCook), with no
// project file to select, version, or rebind. The asset browser could list
// the baked payloads (the "GI bakes" collection over the bakes' .meta
// identity files)
// but the user could never CREATE the thing that OWNS the bake: its
// settings + its result pointer.
//
// The model (mirrors the .material source asset):
//   - Each cook asset is ONE text manifest file carrying BOTH halves the
//     user asked for: the settings (bake parameters) AND the binary blob
//     result (the bake GUID + its VFS path — the payload itself keeps
//     living at its guid-derived bake path, gi/<guid>.sggi for GI world
//     fields, gi/<guid>.sglm for lightmap atlases; the asset REFERENCES
//     it, never duplicates it).
//   - <name>.gicook  — the GI cook (convex volumes + world-field octree
//     flags; result = the probe bake guid/path).
//   - <name>.lmcook  — the lightmap cook (TourCook's lm*/sun/sky block;
//     result = the atlas bake guid/path).
//   - Identity is the standard source-asset sidecar (<file>.meta guid +
//     assetdb row, kind "gicook"/"lmcook"); the manifest ALSO carries its
//     guid (like .material) so a detached read still knows WHO it is.
//   - Unbaked assets carry an empty blob (blobGuid/blobPath "") — creation
//     writes exactly that; the bake execution (a follow-up: running
//     GiBaker/PakCooker FROM the asset and recording blobGuid/blobPath +
//     bakedHash) fills it in. Baked assets whose settings changed after
//     the bake read as stale (bakedHash != settings hash).
//
// Defaults mirror TourCook (engine/include/Engine/Asset/tour.h) so a fresh
// asset bakes exactly what the cook dialog would. The runtime never reads
// these files (cook-time/editor-time only, like .material).

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace smvk::cook {

// File extensions (browser icons, filters, creation).
inline constexpr const char* kGiCookExt = ".gicook";
inline constexpr const char* kLightmapCookExt = ".lmcook";
// Typed GI product assets: first-class manifests that OWN their bake
// payload (no loose bake files — the payload path rides blobPath inside
// the typed asset folder, typically content/gi/). The registry
// (assetdb kind) is the single index of who owns what.
inline constexpr const char* kCubemapExt = ".cubemap";
inline constexpr const char* kGivolumeExt = ".givolume";
// Assetdb/browser kinds (the sidecar importer + db row; opaque strings).
inline constexpr const char* kCookAssetKindGiCook = "gicook";
inline constexpr const char* kCookAssetKindLightmapCook = "lmcook";
inline constexpr const char* kCookAssetKindCubemap = "cubemap";
inline constexpr const char* kCookAssetKindGivolume = "givolume";

// 32 lowercase hex chars (the asset-identity rule, shared with the
// source-side GUIDs and the bake-asset GUIDs).
[[nodiscard]] bool IsCookAssetGuid(std::string_view s);

// ---- GI cook -----------------------------------------------------------
// Owns the GiBaker GI-volume bake settings for one scene: the on-demand
// flag from the cook dialog (TourCook::giVolumes) plus the result
// pointer (the bake guid/path; "" = never baked). (The world-field
// flag + the .sggi blob it pointed at are RETIRED with the octree —
// stage 3 of the octree retirement; old sidecars' world=/blobPath=
// lines load through the unknown-key tolerance.)
struct GiCookAsset {
	std::string guid;     // the asset's own GUID (manifest + .meta agree)
	std::string name;     // display name (file stem by default)
	std::string scene;    // scene stem this cook bakes ("" = unbound)
	bool giVolumes = true; // bake authored GI volumes (giVolumes)
	std::string blobGuid; // result bake GUID ("" = unbaked)
	std::string blobPath; // result VFS path (historically the .sggi; the
	                      // world bake is gone, volume-only cooks keep a
	                      // previous record alive)
	std::string bakedHash; // settings fingerprint at bake ("" = unbaked)
};

// ---- Lightmap cook ------------------------------------------------------
// Owns the PakCooker lightmap bake for one scene: TourCook's lm*/sun/sky
// block plus the result pointer (the .sglm atlas guid/path; "" = unbaked).
struct LightmapCookAsset {
	std::string guid;
	std::string name;
	std::string scene;    // scene stem this cook bakes ("" = unbound)
	// Chart density request, METERS PER TEXEL (mirrors TourCook).
	float lmMetersPerTexel = 0.25f;
	uint32_t lmLobes = 12;
	uint32_t lmAtlasWidth = 1024; // fixed width; height grows with content
	uint32_t lmRays = 256;
	int lmProxyRes = 256;  // voxel-remesh proxy grid (0 = legacy path)
	float lmExcludeSmall = 0.0f;
	std::string lmExcludeKw;
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f }; // linear RGB world sun
	std::string blobGuid; // result bake GUID ("" = unbaked)
	std::string blobPath; // result VFS path, e.g. "gi/<guid>.sglm"
	std::string bakedHash; // settings fingerprint at bake ("" = unbaked)
};

// Fresh-asset defaults (name = file stem suggestion, scene = the open
// scene's stem or "" when nothing is open). GUIDs are NOT minted here —
// the creator (editor AdoptProjectAsset) issues them.
[[nodiscard]] GiCookAsset DefaultGiCookAsset(const std::string& name,
	const std::string& scene);
[[nodiscard]] LightmapCookAsset DefaultLightmapCookAsset(
	const std::string& name, const std::string& scene);

// ---- Cubemap capture asset -------------------------------------------------
// Owns one cubemap capture set for a scene: capture parameters + the baked
// payload pointer (the .ktx2 set / manifest rows now live under the typed
// asset folder, e.g. "gi/<guid>/cube_*.ktx2"; "" = never baked).
struct CubemapAsset {
	std::string guid;
	std::string name;
	std::string scene;
	int size = 128;       // capture resolution per face
	int rays = 256;       // rays per texel
	std::string capture;  // capture-set base name (blank = scene stem)
	std::string blobGuid; // result bake GUID ("" = unbaked)
	std::string blobPath; // result VFS path, e.g. "gi/<guid>.cubedata"
	std::string bakedHash;
};

// ---- GI-volume asset -------------------------------------------------------
// Owns one authored GI volume's bake: the bake inputs snapshot +
// result pointer (payload e.g. "gi/<guid>.givolume"; "" = unbaked).
struct GivolumeAsset {
	std::string guid;
	std::string name;
	std::string scene;
	uint32_t node = UINT32_MAX; // source scene node (UINT32_MAX = unbound)
	uint32_t priority = 0;
	std::string blobGuid;
	std::string blobPath; // e.g. "gi/<guid>.givolume"
	std::string bakedHash;
};

[[nodiscard]] CubemapAsset DefaultCubemapAsset(const std::string& name,
	const std::string& scene);
[[nodiscard]] GivolumeAsset DefaultGivolumeAsset(const std::string& name,
	const std::string& scene, uint32_t node = UINT32_MAX);

// Settings fingerprint (the staleness token: bakedHash == SettingsHash
// means the blob matches the current settings). Covers every bake input
// above (identity + blob fields excluded — they ARE the result).
[[nodiscard]] std::string GiCookSettingsHash(const GiCookAsset& a);
[[nodiscard]] std::string LightmapCookSettingsHash(
	const LightmapCookAsset& a);
[[nodiscard]] inline bool GiCookBlobStale(const GiCookAsset& a) {
	return !a.blobGuid.empty() && a.bakedHash != GiCookSettingsHash(a);
}
[[nodiscard]] inline bool LightmapCookBlobStale(
	const LightmapCookAsset& a) {
	return !a.blobGuid.empty()
		&& a.bakedHash != LightmapCookSettingsHash(a);
}
[[nodiscard]] std::string CubemapSettingsHash(const CubemapAsset& a);
[[nodiscard]] std::string GivolumeSettingsHash(const GivolumeAsset& a);
[[nodiscard]] inline bool CubemapBlobStale(const CubemapAsset& a) {
	return !a.blobGuid.empty() && a.bakedHash != CubemapSettingsHash(a);
}
[[nodiscard]] inline bool GivolumeBlobStale(const GivolumeAsset& a) {
	return !a.blobGuid.empty()
		&& a.bakedHash != GivolumeSettingsHash(a);
}

// Manifest IO (key=value ASCII like .material; tolerant: unknown keys
// skip, a missing/invalid guid fails). Host forms take paths; text forms
// take the file content (VFS reads cook through the overlay).
bool SaveGiCookAsset(const std::string& path, const GiCookAsset& a,
	std::string& err);
bool LoadGiCookAsset(const std::string& path, GiCookAsset& out,
	std::string& err);
bool SaveGiCookAssetText(std::string& out, const GiCookAsset& a);
bool LoadGiCookAssetText(const std::string& text, GiCookAsset& out,
	std::string& err);
bool SaveLightmapCookAsset(const std::string& path,
	const LightmapCookAsset& a, std::string& err);
bool LoadLightmapCookAsset(const std::string& path, LightmapCookAsset& out,
	std::string& err);
bool SaveLightmapCookAssetText(std::string& out,
	const LightmapCookAsset& a);
bool LoadLightmapCookAssetText(const std::string& text,
	LightmapCookAsset& out, std::string& err);
bool SaveCubemapAsset(const std::string& path, const CubemapAsset& a,
	std::string& err);
bool LoadCubemapAsset(const std::string& path, CubemapAsset& out,
	std::string& err);
bool SaveCubemapAssetText(std::string& out, const CubemapAsset& a);
bool LoadCubemapAssetText(const std::string& text, CubemapAsset& out,
	std::string& err);
bool SaveGivolumeAsset(const std::string& path, const GivolumeAsset& a,
	std::string& err);
bool LoadGivolumeAsset(const std::string& path, GivolumeAsset& out,
	std::string& err);
bool SaveGivolumeAssetText(std::string& out, const GivolumeAsset& a);
bool LoadGivolumeAssetText(const std::string& text, GivolumeAsset& out,
	std::string& err);

} // namespace smvk::cook
