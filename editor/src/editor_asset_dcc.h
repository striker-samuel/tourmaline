// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// The editor's source-of-truth -> published-asset handoff to external DCCs.
// A native .ma/.blend/.hip/... is an AUTHORING document, not an engine
// importer input. The DCC publishes a supported .obj/.gltf/.glb (or an
// image); the content server watches that published file as usual.
#include <string>

namespace editor {

struct DccAsset {
	std::string published; // in-project file the content server watches
	std::string document;  // DCC-authoring file (same as published if unlinked)
	bool linked = false;
	bool fromNative = false; // native .ma/.mb/.blend tile, first publish
};

// Resolve a clicked asset (including a manifest-derived .geometry,
// .material, .anim, or a .texture) to its published source file.
// Fails rather than opening binary derived data in the OS.
bool ResolveDccPublishedAsset(const std::string& selected,
	std::string& published, std::string& err);

// Resolve the optional <published-file>.dccsource sidecar; a broken
// authoring link is a visible error (not a silent published fallback).
bool ResolveDccAsset(const std::string& selected,
	DccAsset& asset, std::string& err);

// Sidecar is beside the PUBLISHED file; paths relative to that directory
// survive moving the whole project. Absolute external sources are also
// allowed, but must remain accessible at the recorded location.
bool LinkDccSource(const std::string& selected,
	const std::string& documentPath, std::string& err);
bool UnlinkDccSource(const std::string& selected, std::string& err);
bool IsDccPublishable(const std::string& published);
	
// Blocking export stage (invoke on the editor's worker pool): executes
// staged tools/dcc/publish_gltf.py through Python, waits for exit code,
// and leaves the previous publication intact on failure. Successful
// replacement is picked up by EditorContentServer.
bool RunDccPublish(const DccAsset& asset, const std::string& publishScript,
	std::string& err);

} // namespace editor
