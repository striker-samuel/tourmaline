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

namespace tour { struct World; class TextureStreamer; }
namespace smvk {
	class OverlayVfs;
	class ResourceManager;
	using ::tour::TextureStreamer; // (moved: engine-owned streamer, tour::)
	using ::tour::World;
}

namespace app {

class ProjectRuntime; // the content session (ProjectRuntime.h)

// Loads scene `sceneIndex` (0 = default) from the preloaded pak into a
// fresh tour::World (uploads issued to ctx; everything renders by
// handles). Null on any failure (logged). The pak's backend mounts
// into contentVfs at priority 10 (below loose content/, so dev
// overrides win) AND ADOPTS into the session's registry (the
// shared-image lane) so the anim-clip resolution below resolves
// through the SAME provider selection every other content read
// uses — the overlay keeps its protocol role (the engine hands the
// world-source callback a vfs; hosts mount their paks into it),
// the CONTENT reads ride the session.
tour::mem::LinearPtr<tour::World> LoadPakWorld(
	PakPreload pre,
	int sceneIndex,
	smvk::ResourceManager& ctx,
	smvk::OverlayVfs& contentVfs,
	tour::TextureStreamer* sceneStreamer,
	app::ProjectRuntime& session);

// The --project + multi-pak launch (the session flows): EVERY listed
// pak preloaded + mounted (manifest order) BEFORE the cluster load —
// cross-pak textures resolve through the content session's REGISTRY
// (DeserializeCluster's external-texture hook; the preloaded backends
// ADOPT into the session's registry — the shared-image lane) — GI
// payloads + anim clips the same way — then the entry scene loads BY
// NAME from the entry pak (identity, never an index). A failed
// preload of ANY listed pak fails the launch (named, loud — never an
// unrelated fallback scene). `preloads` is manifest order;
// `entryPakIndex` indexes into it; `session` is the launch's content
// session (REQUIRED — every content read on this path rides it).
tour::mem::LinearPtr<tour::World> LoadProjectWorld(
	std::vector<PakPreload> preloads,
	size_t entryPakIndex,
	const std::string& entrySceneName,
	smvk::ResourceManager& ctx,
	smvk::OverlayVfs& contentVfs,
	tour::TextureStreamer* sceneStreamer,
	app::ProjectRuntime* session);

// STATIC-FLAG APPLICATION (the loader's decision seam — public for
// PakHostTests, which drives it on hand-built value worlds). Cooked
// detection is SECTION PRESENCE (Scene::staticRootsSerialized), never
// any_of(staticRoot): an authored ALL-DYNAMIC cooked scene (serialized,
// all zeros) is cooked data and is honored verbatim; the viewer-side
// freeze is the loud LEGACY fallback only (no serialized static section
// + no skeleton). `pakName` labels the fallback log.
void ApplyCookedStaticFlags(tour::World& world, const std::string& pakName);

// The POST-LOAD TAIL shared by the world loaders and the runtime scene
// layer (ProjectScenes): anim-clip attach (the first skinned cluster),
// cooked static flags, the resolve/fixup that folds the loaded clusters
// into the merged world scene, and the env-gated scene-prep diagnostics.
// EVERY content read resolves through the SESSION (the registry's
// provider union — the host overlay is no longer a content source on
// any runtime path). Public so additive scene loads run the SAME tail
// as the entry load. `pakStem` is the anim fallback's stem (the loading
// pak's).
void PostLoadTail(tour::World& world, const std::string& pakStem,
	app::ProjectRuntime& session);

} // namespace app
