// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Asset identity = VFS path with an optional '#'-fragment for sub-assets:
//
//   "models/Sponza/sponza.gltf#mesh.12/prim.0"
//   "models/Sponza/sponza.gltf#mat.3"
//   "textures/brick.ktx2"                      (no fragment: whole file)
//
// The file part is a VFS path (see Vfs::Normalize); the fragment never
// touches the VFS, it only keys the owning Cluster's per-kind UriMaps.
// There are deliberately no canonical/global asset URIs: the VFS overlay
// (mount order) is the single source of truth for which bytes back a
// path, and URI values round-trip through paks unchanged.
//
// View semantics: helpers take/return StringViewT. Views either slice the
// caller's memory or point into the scratch arena argument (default
// FrameArena — reset per frame, never stored). The fast path of
// NormalizeAssetUri returns the input view unchanged; only rewrites
// (backslashes, '.'/'..' segments, duplicate separators) allocate.

#include <string>
#include <utility>

#include <Engine/Memory/arena.h>
#include <Engine/Memory/string_view.h>
#include <Engine/Vfs/vfs.h>

namespace smvk {

// Splits "file#fragment" into {file, fragment} — views into `uri`, no copy.
[[nodiscard]] inline std::pair<StringViewT, StringViewT> SplitAssetUri(StringViewT uri) {
	const size_t hash = uri.find('#');
	if (hash == StringViewT::npos)
		return { uri, StringViewT() };
	return { uri.substr(0, hash), uri.substr(hash + 1) };
}

// True when `uri` already satisfies NormalizeAssetUri's contract (used as
// the allocation-free fast path; also handy for asserts).
[[nodiscard]] inline bool UriAlreadyNormalized(StringViewT uri) {
	if (uri.empty())
		return false;
	// Drive-letter absolute (C:/..., C:\...) or posix absolute.
	if (uri.size() >= 2 && uri[1] == ':')
		return false;
	if (uri[0] == '/' || uri[0] == '\\')
		return false;
	if (uri.contains('\\'))
		return false; // normalized paths use '/' only
	size_t segStart = 0;
	for (size_t i = 0; i <= uri.size(); ++i) {
		if (i == uri.size() || uri[i] == '/') {
			if (i == segStart)
				return false; // empty segment ("//" or trailing '/')
			const size_t n = i - segStart;
			if (n == 1 && uri[segStart] == '.')
				return false;
			if (n == 2 && uri[segStart] == '.' && uri[segStart + 1] == '.')
				return false;
			segStart = i + 1;
		}
	}
	return true;
}

// Normalizes the file part as a VFS path, preserves the fragment verbatim.
// Returns the input view when already normalized; otherwise builds the
// rewrite into `scratch`. Empty view = invalid (absolute/root-escape).
[[nodiscard]] StringViewT NormalizeAssetUri(StringViewT uri, Arena& scratch = FrameArena());

// Composes "base#fragment" into `scratch` (base should be a normalized
// VFS path; empty fragment returns base unchanged).
[[nodiscard]] inline StringViewT MakeSubUri(StringViewT baseVfsPath,
	StringViewT fragment, Arena& scratch = FrameArena()) {
	if (fragment.empty())
		return baseVfsPath;
	return ConcatInto(scratch, baseVfsPath,
		ConcatInto(scratch, StringViewT("#"), fragment));
}

// The VFS path (file part) backing an asset URI, normalized, as a view.
// Empty when invalid. May point into FrameArena on the rewrite path.
[[nodiscard]] inline StringViewT AssetFilePartView(StringViewT uri, Arena& scratch = FrameArena()) {
	return NormalizeAssetUri(SplitAssetUri(uri).first, scratch);
}

// Owned VfsPath copy (IO boundary only — VFS backends take std::string).
[[nodiscard]] inline VfsPath AssetFilePart(StringViewT uri) {
	return AssetFilePartView(uri).Str();
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
