#pragma once

#include <optional>
#include <string>

#include <Engine/Vfs/vfs.h>

namespace smvk {

// Asset identity = VFS path with an optional '#'-fragment for sub-assets of
// a file, e.g.:
//
//   "models/Sponza/sponza.gltf#mesh.12/prim.0"
//   "models/Sponza/sponza.gltf#mat.3"
//   "textures/brick.ktx2"                      (no fragment: whole file)
//
// The file part is a VFS path (see Vfs::Normalize); the fragment never
// touches the VFS, it only keys the owning Cluster's per-kind lookup maps.
// There are deliberately no canonical/global asset URIs: the VFS overlay
// (mount order) is the single source of truth for which bytes back a path,
// and AssetRef::uri values round-trip through paks unchanged.

// Splits "file#fragment" into {file, fragment}. No normalization applied.
inline std::pair<std::string, std::string> SplitAssetUri(const std::string& uri) {
	const size_t hash = uri.find('#');
	if (hash == std::string::npos)
		return { uri, {} };
	return { uri.substr(0, hash), uri.substr(hash + 1) };
}

// Normalizes the file part as a VFS path, preserves the fragment verbatim.
// Returns "" when the file part is not a valid VFS path.
inline std::string NormalizeAssetUri(const std::string& uri) {
	const auto [file, frag] = SplitAssetUri(uri);
	const VfsPath n = vfs::Normalize(file);
	if (n.empty())
		return {};
	if (frag.empty())
		return n;
	return n + "#" + frag;
}

// Builds "base#fragment". Base should already be a normalized VFS path.
inline std::string MakeSubUri(const std::string& baseVfsPath, const std::string& fragment) {
	if (fragment.empty())
		return baseVfsPath;
	return baseVfsPath + "#" + fragment;
}

// The VFS path (file part) backing an asset URI, normalized. "" if invalid.
inline VfsPath AssetFilePart(const std::string& uri) {
	return vfs::Normalize(SplitAssetUri(uri).first);
}

// glTF sampler parameters, stored as raw tinygltf enum ints (-1 = unset).
// Persisted per texture into paks so a future sampler-selection pass can
// reproduce authoring intent. The renderer is intentionally untouched: it
// keeps using its default sampler until that pass lands.
struct TextureSamplerDesc {
	int magFilter = -1;
	int minFilter = -1;
	int wrapS = -1;
	int wrapT = -1;
};

} // namespace smvk
