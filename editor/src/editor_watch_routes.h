// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE WATCHED-PROJECT ROUTING TABLE (the item-16 core): the pure
// classification ConsumeWatchedProjectChanges (editor_project.cpp)
// switches on at the frame boundary. Extracted from the funnel so the
// routing contract is unit-testable without an EditorApp (the
// test_asset_ops discipline — same shape as editor_asset_ops.h).
//
// The table (per settled change; extensions case-insensitive):
//   Removed        a deleted source: the registry heals on the next
//                  Rescan (a log line only)
//   MetaSidecar    .meta (incl. the <source>.meta identity ledgers of
//                  every mesh source format): the asset-db rescan
//   GltfSource     a mesh source (.gltf/.glb/.obj — one shared gate,
//                  see source_formats.h): ImportMeshArtifactFor
//                  (stale-gated, identity-preserving) + the geometry
//                  re-open flow
//   TourDoc        .tour/.tourrecipe: LOG ONLY (an external edit never
//                  silently overwrites unsaved editor work)
//   TextureSource  an image source BESIDE its .texture asset (the
//                  CreateTextureAsset convention: x.png + x.texture in
//                  the same dir): the LIVE texture refresh (the force
//                  lane + the material-slot rewire, no scene reload)
//   Other          everything else: the log (kind-specific live
//                  refresh remains a follow-up)
//
// TextureSource probes the sibling .texture through std::filesystem
// (exists) — the ONE impure touch, kept so the editor funnel stays a
// pure switch and the tests build real scratch trees.

#include <filesystem>
#include <string>

#include <gltf_import/source_formats.h> // the mesh-source gate (.gltf/.glb/.obj)

namespace editor {

enum class WatchRoute {
	Removed,
	MetaSidecar,
	GltfSource,
	TourDoc,
	TextureSource,
	Other,
};

[[nodiscard]] inline WatchRoute ClassifyWatchedChange(
	const std::filesystem::path& path)
{
	// Extension, lowercased (the old funnel compared case-sensitively;
	// a DCC saving "X.PNG" deserves the same refresh).
	std::string ext = path.extension().generic_string();
	for (char& c : ext)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	if (ext == ".meta")
		return WatchRoute::MetaSidecar;
	if (smvk::assets::IsMeshSourceExtension(ext))
		return WatchRoute::GltfSource;
	if (ext == ".tour" || ext == ".tourrecipe")
		return WatchRoute::TourDoc;
	if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
			|| ext == ".tga" || ext == ".bmp" || ext == ".ktx2") {
		// An image is a TEXTURE SOURCE only when its .texture asset
		// sits beside it (the asset convention); a bare image (a
		// glTF-imported source image, a reference, concept art) keeps
		// the generic log — its refresh rides the glTF artifact's
		// content digest, never a pool swap.
		std::error_code ec;
		std::filesystem::path asset = path;
		asset.replace_extension(".texture");
		if (std::filesystem::exists(asset, ec))
			return WatchRoute::TextureSource;
	}
	return WatchRoute::Other;
}

} // namespace editor
