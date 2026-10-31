// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// PakWorldLoader — the pak -> World half of the old PakApp.cpp monolith
// (the viewer HOST's renderer wiring + lifecycle stayed in PakApp.cpp;
// see Host.h's layer note). Owns: consuming the background preload,
// scene discovery + cluster load, the skinning clip attach, static-flag
// application (the cooked marks — the viewer never invents them), the
// scene-prep diagnostics (SSR wax / KD dump), and mounting the pak
// backend into the content VFS.
#include "PakPreload.h"

#include <Engine/Memory/memory.h> // mem::LinearPtr

#include <string>
#include <vector>

namespace smvk {
	class OverlayVfs;
	class ResourceManager;
	class TextureStreamer;
	struct World;
}

namespace app {

// Loads scene `sceneIndex` (0 = default) from the preloaded pak into a
// fresh smvk::World (uploads issued to ctx; everything renders by
// handles). Null on any failure (logged). Mounts the pak backend into
// contentVfs at priority 10 (below loose content/, so dev overrides
// win) before the anim-clip resolution so the pak serves its own
// embedded clips (the self-contained path).
smvk::mem::LinearPtr<smvk::World> LoadPakWorld(
	PakPreload pre,
	int sceneIndex,
	smvk::ResourceManager& ctx,
	smvk::OverlayVfs& contentVfs,
	smvk::TextureStreamer* sceneStreamer);

// The --project launch (the appmanifest flow): EVERY listed pak
// preloaded + mounted (manifest order) BEFORE the cluster load —
// cross-pak textures resolve through the mounted backends via
// DeserializeCluster's external-texture hook, GI payloads + anim clips
// the same way — then the entry scene loads BY NAME from the entry
// pak (identity, never an index). A failed preload of ANY listed pak
// fails the launch (named, loud — never an unrelated fallback scene).
// `preloads` is manifest order; `entryPakIndex` indexes into it.
smvk::mem::LinearPtr<smvk::World> LoadProjectWorld(
	std::vector<PakPreload> preloads,
	size_t entryPakIndex,
	const std::string& entrySceneName,
	smvk::ResourceManager& ctx,
	smvk::OverlayVfs& contentVfs,
	smvk::TextureStreamer* sceneStreamer);

// STATIC-FLAG APPLICATION (the loader's decision seam — public for
// PakHostTests, which drives it on hand-built value worlds). Cooked
// detection is SECTION PRESENCE (Scene::staticRootsSerialized), never
// any_of(staticRoot): an authored ALL-DYNAMIC cooked scene (serialized,
// all zeros) is cooked data and is honored verbatim; the viewer-side
// freeze is the loud LEGACY fallback only (no serialized static section
// + no skeleton). `pakName` labels the fallback log.
void ApplyCookedStaticFlags(smvk::World& world, const std::string& pakName);

} // namespace app
