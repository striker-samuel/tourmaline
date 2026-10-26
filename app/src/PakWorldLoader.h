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

} // namespace app
