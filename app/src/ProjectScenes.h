// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// ProjectScenes: the SCENE-INSTANCE layer over ProjectRuntime — the
// reviewer's "next layer" on the content-as-code foundation. Three
// responsibilities, all NUMERIC-ID-ADDRESSED (game code passes
// project::scenes::* constants; no pak filenames, no cooked scene
// names, no VFS paths enter from the outside):
//
//   1. SCENE INSTANCES — LoadScene/LoadSceneAdditive/RemoveScene over
//      a live smvk::World. A load deserializes the scene's pak blob
//      into a NEW registered Cluster (World::LoadClusterFromPak's
//      path) and runs the same post-load tail the apphost's entry
//      load runs (anim attach, cooked static flags, the resolve that
//      folds the cluster into the merged world scene). RemoveScene
//      unloads the cluster the load created (World::UnloadCluster) —
//      the merged scene rebuild drops the scene's nodes. LoadScene is
//      a REPLACE (every previously loaded project scene unloads
//      first); LoadSceneAdditive stacks on top.
//
//   2. OWNERSHIP-AWARE PREFETCH — Prefetch(id) warms the asset's pak
//      (the background preload + CRC open is the expensive half) and
//      PINS the id; Release(id) drops the pin. A loaded scene OWNS the
//      assets registered against it (SceneAcquireAsset — the audio
//      layer registers each scene's sound ids at load); RemoveScene
//      releases them. An asset stays "held" while ANY scene owns it OR
//      an explicit pin exists; RemoveScene reports the ids whose LAST
//      owner went away and that are not pinned (the releasedAssets out
//      param) so the layer that CACHES decoded forms (audio clips) can
//      evict exactly those.
//
//   3. THE ID->SCENE JOIN — ResolveSceneTarget maps a scene id to
//      (pak, cooked scene name, pak scene index) through the
//      appmanifest's recorded name (the [ids] row's 5th token) or, for
//      rows that predate it, single-scene inference (a multi-scene pak
//      without a recorded name refuses loudly — recook).
//
// SceneHandle is the same 64-bit generational shape the gameplay
// handles (MeshEntityHandle / AnimatorHandle / AudioSourceHandle) use:
// high 32 bits generation (starts at 1), low 32 bits the slot index;
// the all-zero value is "no scene". RemoveScene bumps the slot's
// generation so stale copies of a removed scene's handle fail
// validation — never resolve a replacement scene by accident.
//
// THREADING: main/render thread only (the world loading it builds on
// is load-time-only there). The constructor's pak opens run their
// preloads in parallel but join before returning.
//
// v1 notes (deliberate):
//   - Pak backends stay open for the runtime's lifetime (the archive
//     is the IO channel + the VFS mount surface; refcount-evicting
//     images is a resource-policy decision that needs a measured need
//     — the ownership registry above is the machinery that would drive
//     it). The prefetched/pinned state is tracked; eviction is not.
//   - The pak->cluster step is a replaceable seam (clusterLoader) so
//     the GPU-less tests drive the instance/ownership machinery with
//     hand-built offline clusters — the default runs the real
//     deserialize path.

#include "ProjectRuntime.h"

#include <Engine/Asset/cluster_serializer.h> // ExternalTextureResolver

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tour { struct World; }
namespace smvk {
	class OverlayVfs;
	class ResourceManager;
	class TextureStreamer;
	struct Cluster;
	using ::tour::World;
}

namespace app {

// A loaded scene instance (see the header comment). Default-zero =
// "no scene" — gameplay structs compose these exactly like
// MeshEntityHandle/AnimatorHandle.
struct SceneHandle {
	uint64_t value = 0;

	[[nodiscard]] constexpr uint32_t Index() const {
		return static_cast<uint32_t>(value);
	}
	[[nodiscard]] constexpr uint32_t Generation() const {
		return static_cast<uint32_t>(value >> 32);
	}
	[[nodiscard]] constexpr explicit operator bool() const {
		return value != 0;
	}
	friend bool operator==(SceneHandle, SceneHandle) = default;
};

class ProjectScenes {
public:
	// Where a scene id resolves to (the id->scene join's product).
	struct SceneTarget {
		std::string pak;        // manifest pak FILE (also the VFS mount name)
		std::string sceneName;  // the cooked scene's name (may be empty on old paks)
		uint32_t sceneIndex = 0; // the pak's scene index (what the load consumes)
	};

	// The pak -> cluster step. The default mirrors World::
	// LoadClusterFromPak (DeserializeCluster + RegisterCluster) and
	// needs a live ResourceManager (a null ctx is the GPU-less test
	// seam and refuses); the GPU-less tests replace it with a
	// hand-built offline cluster. Returns the registered cluster
	// (the WORLD owns it) or null + err.
	using ClusterLoader = std::function<smvk::Cluster*(
		smvk::World& world, smvk::ResourceManager* ctx,
		const smvk::PakReader& reader, uint32_t sceneIndex,
		smvk::TextureStreamer* streamer,
		std::shared_ptr<const std::vector<uint8_t>> imagePin,
		const smvk::ExternalTextureResolver* externalTexture,
		std::string& err)>;

	explicit ProjectScenes(ProjectRuntime& runtime,
		smvk::OverlayVfs& contentVfs);

	// The constructor's adopt-or-open pass: every manifest pak adopted
	// from the host's mounts or preloaded + opened in parallel, then
	// mounted (priority 10, manifest order) if not already mounted.
	// One pak failing leaves the layer unusable (the same every-pak-
	// must-open contract LoadProjectWorld applies) — check Valid().
	[[nodiscard]] bool Valid() const { return m_openError.empty(); }
	[[nodiscard]] const std::string& OpenError() const {
		return m_openError;
	}

	// The runtime façade this layer resolves ids through (the
	// ProjectScenes ctor argument).
	[[nodiscard]] ProjectRuntime& Runtime() const { return m_runtime; }

	// ---- the id->scene join ----
	// Resolves (and caches) a scene id to its load target. Unknown ids,
	// non-scene kinds, missing/corrupt paks, and multi-scene paks
	// without a recorded name all refuse loudly (err filled, nullopt).
	[[nodiscard]] std::optional<SceneTarget> ResolveSceneTarget(
		uint64_t sceneId, std::string& err);

	// ---- scene instances ----
	// REPLACE semantics: unloads every currently loaded project scene,
	// then loads sceneId into `world`. Empty handle + err on failure
	// (the world is untouched when the replace's own load fails — the
	// previously loaded scenes are already gone by then; a failed
	// LoadScene is a loud dead end, never a silent fallback scene).
	// ctx is the host's upload context — REQUIRED for the default
	// cluster loader (null = the GPU-less test seam, which must pair
	// with a replaced clusterLoader).
	SceneHandle LoadScene(smvk::World& world, smvk::ResourceManager* ctx,
		smvk::TextureStreamer* streamer, uint64_t sceneId,
		std::string& err);
	// ADDITIVE: stacks sceneId on top of whatever the world already
	// carries (the entry scene the apphost loaded, other additive
	// scenes, editor content).
	SceneHandle LoadSceneAdditive(smvk::World& world,
		smvk::ResourceManager* ctx, smvk::TextureStreamer* streamer,
		uint64_t sceneId, std::string& err);
	// Unloads the scene's cluster from `world` and releases its owned
	// assets. releasedAssets (optional out) receives the asset ids
	// whose last owner went away and that carry no explicit prefetch
	// pin — the decoded-form caches (audio) evict exactly those.
	// False + err on a stale/unknown handle (generation mismatch =
	// the scene was already removed).
	bool RemoveScene(SceneHandle scene, smvk::World& world,
		std::string& err,
		std::vector<uint64_t>* releasedAssets = nullptr);

	[[nodiscard]] bool IsLoaded(SceneHandle scene) const;
	[[nodiscard]] uint32_t LoadedSceneCount() const;
	// The first loaded instance of a content id (empty handle = none).
	[[nodiscard]] SceneHandle FindScene(uint64_t sceneId) const;
	// The content id a loaded instance carries (0 on a bad handle).
	[[nodiscard]] uint64_t SceneContentId(SceneHandle scene) const;
	// The cluster a loaded instance created (nullptr on a bad handle;
	// DEAD after RemoveScene — the world owned it, the unload erased
	// it). Lets scene-aware layers (audio) reach the owning cluster.
	[[nodiscard]] smvk::Cluster* SceneCluster(SceneHandle scene) const;

	// ---- ownership-aware prefetch ----
	// Warms the asset's pak (the background preload + open — the
	// expensive half of a first read) and PINS the id. Works for every
	// kind: scenes additionally resolve + cache their SceneTarget.
	// Pair every Prefetch with a Release; pins survive scene removal.
	bool Prefetch(uint64_t assetId, std::string& err);
	// Drops one explicit pin. False (no-op) when the id carries none.
	bool Release(uint64_t assetId);
	[[nodiscard]] bool IsPinned(uint64_t assetId) const;
	// True while the id is held by ANY owner: an explicit pin or any
	// loaded scene's ownership registry (what decoded-form caches gate
	// eviction on).
	[[nodiscard]] bool IsHeld(uint64_t assetId) const;

	// ---- the ownership registry ----
	// Registers that `scene` owns assetId (the scene-aware layers call
	// this at their own load/discover points — e.g. audio registering
	// each scene's sound ids). Duplicate acquires per scene are
	// idempotent. No-op on a stale handle.
	void SceneAcquireAsset(SceneHandle scene, uint64_t assetId);
	// Releases the scene's whole registry (RemoveScene calls this
	// internally; returns the dropped ids — see RemoveScene). Public
	// so a layer that tears its own state down first can drive the
	// same bookkeeping.
	[[nodiscard]] std::vector<uint64_t> SceneReleaseAssets(
		SceneHandle scene);

	// The replaceable pak->cluster seam (see ClusterLoader). Public by
	// design: the GPU-less tests substitute a hand-built cluster.
	ClusterLoader clusterLoader;

private:
	struct SceneInstance {
		uint32_t generation = 1;
		bool loaded = false;
		uint64_t contentId = 0;
		std::string pak;
		std::string sceneName;
		uint32_t sceneIndex = 0;
		smvk::Cluster* cluster = nullptr; // dead after RemoveScene
		std::vector<uint64_t> ownedAssets;
	};

	[[nodiscard]] SceneInstance* Instance(SceneHandle scene);
	[[nodiscard]] const SceneInstance* Instance(SceneHandle scene) const;
	[[nodiscard]] SceneHandle AllocHandle();
	void FreeHandle(SceneHandle scene);

	SceneHandle LoadInternal(smvk::World& world,
		smvk::ResourceManager* ctx, smvk::TextureStreamer* streamer,
		uint64_t sceneId, std::string& err);

	ProjectRuntime& m_runtime;
	smvk::OverlayVfs& m_vfs;
	std::string m_openError; // non-empty = the ctor's pak pass failed
	std::vector<SceneInstance> m_slots;
	std::vector<uint32_t> m_free;
	// Explicit prefetch pins (id -> count).
	std::map<uint64_t, uint32_t> m_pins;
	// Resolved scene targets (id -> join product), warmed by Prefetch.
	std::map<uint64_t, SceneTarget> m_targets;
	// The asset ownership registry (id -> live owning scene count).
	std::map<uint64_t, uint32_t> m_assetRefs;
};

} // namespace app
