// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier-Identifier: MIT

// THE mesh-source format gate (one place, every consumer): which
// source files are MODEL imports (.gltf/.glb through tinygltf, .obj
// through the tinyobjloader adapter — see LoadModelFromSourceFile)
// and which ".meta" sidecars are those sources' identity ledgers.
//
// Every extension check that used to spell `.gltf || .glb` inline (the
// CLI, the project import/build pipelines, the catalog, the recipe
// resolver, the editor's open/drop/reference/watch surfaces) funnels
// through THIS header instead — a new source format lands in one row
// and lights up everywhere at once. The header is deliberately
// self-contained (no engine, no tinygltf includes) so the pure
// classifiers (editor_watch_routes.h) can use it from any TU.
//
// NOT model sources: .geometry (the DERIVED artifact), .tour (scenes),
// images (their own import lane), .fbx/.dae/.usd (no adapter yet).

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace smvk::assets {

// `ext` INCLUDES the leading dot; the comparison is case-insensitive
// (a DCC saving "X.GLB" is the same source — callers that pre-lower
// via LowerExtension pay nothing, callers that don't stay correct).
[[nodiscard]] inline bool IsMeshSourceExtension(std::string_view ext) {
	auto eq = [ext](const char* lit) {
		const size_t n = std::char_traits<char>::length(lit);
		if (ext.size() != n)
			return false;
		for (size_t i = 0; i < n; ++i)
			if (std::tolower(static_cast<unsigned char>(ext[i]))
					!= lit[i]) // the literals are already lowercase
				return false;
		return true;
	};
	return eq(".gltf") || eq(".glb") || eq(".obj");
}

// The lowercase extension of `p` ("" when none) — the helper for
// path-based gates.
[[nodiscard]] inline std::string LowerExtension(
	const std::filesystem::path& p) {
	std::string e = p.extension().string();
	std::transform(e.begin(), e.end(), e.begin(),
		[](unsigned char c) { return char(std::tolower(c)); });
	return e;
}

// TRUE when `p` is a model source file (see the header comment).
[[nodiscard]] inline bool IsMeshSourceFile(const std::filesystem::path& p) {
	return IsMeshSourceExtension(LowerExtension(p));
}

// TRUE for a FILE NAME (not a path) that is a model source's identity
// ledger sidecar: "<source>.meta" where <source> carries a mesh-source
// extension (knight.gltf.meta, bistro.glb.meta, statue.obj.meta). The
// derivation writes/reads it beside the source; the editor's
// delete-planning and the publish/replace flows must recognize every
// source format's ledger, not just the glTF one.
[[nodiscard]] inline bool IsMeshSourceMetaSidecar(const std::string& name) {
	static constexpr std::string_view kMeta = ".meta";
	if (name.size() <= kMeta.size()
			|| name.compare(name.size() - kMeta.size(), kMeta.size(),
				kMeta) != 0)
		return false;
	// The extension of the file the sidecar belongs to: everything up
	// to the ".meta" suffix, e.g. "knight.gltf.meta" -> ".gltf".
	const size_t cut = name.size() - kMeta.size();
	const size_t dot = name.find_last_of('.', cut - 1);
	if (dot == std::string::npos)
		return false;
	std::string ext = name.substr(dot, cut - dot);
	std::transform(ext.begin(), ext.end(), ext.begin(),
		[](unsigned char c) { return char(std::tolower(c)); });
	return IsMeshSourceExtension(ext);
}

} // namespace smvk::assets
