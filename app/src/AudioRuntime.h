// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// AudioRuntime: the SCENE-AWARE audio layer — the playback half of the
// AudioSource pipeline (docs/audio_source.md). An IAppElement stacked
// over the renderer that owns:
//
//   * The CLIP CACHE — sound ids (project::sounds::*, 0x4xxx) decoded
//     to mixer clips through ProjectRuntime::ReadAsset (the pak's
//     Sound blobs). Clips stay cached while ANY live scene references
//     them (the merged world's audio rows, re-checked every update) OR
//     an explicit PrefetchSound pin exists — "shared or explicitly
//     prefetched clips remain cached"; scene-owned clips die with
//     their scene (RemoveScene's rebuild drops the rows, the next
//     update evicts + unloads).
//
//   * The EMITTER STATES — one per authored audio source (keyed by the
//     STABLE (owning cluster, cluster-local node) pair, so a scene
//     rebuild never confuses emitters; positions read from the merged
//     scene's global transforms through World::clusterMergedNodes) +
//     one per SPAWNED source (keyed by its AudioSourceHandle — the
//     gameplay surface, World::SpawnAudioSource/DestroyAudioSource).
//     Every update advances each emitter: distance attenuation
//     (minDistance..maxDistance linear rolloff) + constant-power
//     stereo pan against the LISTENER pose, fed to the mixer through
//     SetVoiceSpatial. Authored playOnLoad emitters autoplay their
//     first slot once per scene lifetime.
//
//   * The VOICES — PlaySound(scene, node, sound) starts a mixer voice
//     tracked against its emitter (it stops when the emitter's scene
//     unloads or a spawned emitter is destroyed); StopSound stops it.
//     PrefetchSound warms + pins a clip.
//
// The mixer itself stays non-positional (tour::audio::AudioEngine +
// the per-voice pan this layer added); ALL spatial reasoning lives
// here, above it. The LISTENER is explicit: SetListener pins a pose,
// or the layer follows the renderer's active camera (the default).
//
// v1 scope (deliberate): linear distance rolloff + right-axis pan
// clamp (no cones/occlusion/reverb), mono-directional emitters (the
// node's orientation is ignored — bells don't face), one autoplay
// slot. The authored node model carries all of it forward.
//
// THREADING: main/render thread only (the element pump); the mixer
// commands are internally thread-safe.

#include "ProjectRuntime.h"
#include "ProjectScenes.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace tour {
	namespace audio { class AudioEngine; struct VoiceHandle; }
}
namespace tour { struct World; class App; }
namespace smvk {
	using ::tour::App;
	using ::tour::World;
	class ResourceManager;
}

namespace engine {
	class WorldRenderer;
	class IAppElement;
}

namespace app {

// The scene-aware audio element (see the header comment). Constructed
// by the host (RunPakApp's extraElement hook / a game's element stack)
// AFTER the renderer attached (the world exists); `tappPath` is the
// project manifest — the runtime façade + the scene layer are built
// LAZILY at first use, by which time the apphost's launch path has
// preloaded + MOUNTED every pak (the scene layer's adopt-or-open pass
// then shares those images instead of re-reading the archives).
[[nodiscard]] std::shared_ptr<engine::IAppElement> MakeAudioRuntime(
	smvk::App& host,
	const std::string& tappPath,
	std::shared_ptr<smvk::World*> worldHandle,
	engine::WorldRenderer* renderer);

// ---- the imperative surface (gameplay code drives these directly;
// they are safe before/without the element — no-ops then) ----
// These operate on the SINGLETON instance the element registers
// (MakeAudioRuntime installs it; one audio layer per process).

// Warms + PINS a sound clip (decode through the pak; the pin survives
// scene removal until ReleaseSound). False + err when the id is
// unknown or the pak does not serve it.
bool PrefetchSound(uint64_t soundId, std::string& err);
// Drops one explicit pin (see ProjectScenes::Release — the byte-side
// pin rides the same counter).
bool ReleaseSound(uint64_t soundId);
// Plays a sound from an EMITTER node (a merged-scene node with an
// audio-source row — authored or spawned). `gain` multiplies the
// emitter's volume (1 = authored). The voice stops with the emitter's
// scene/spawn lifetime. Returns the mixer voice handle (invalid +
// err on an unknown node/clip).
tour::audio::VoiceHandle PlaySound(smvk::World& world,
	uint32_t emitterNode, uint64_t soundId, float gain = 1.0f,
	std::string* err = nullptr);
// Stops a voice started by PlaySound (the mixer handle).
void StopSound(tour::audio::VoiceHandle voice);
// Sets an emitter's master volume (both row copies — the merged scene
// row the renderer world carries + the owning cluster's durable row —
// so a later resolve keeps the edit). False when the node carries no
// audio source.
bool SetAudioSourceVolume(smvk::World& world, uint32_t emitterNode,
	float volume);
// Pins the listener pose (world-space). An empty forward resets to
// FOLLOW-THE-CAMERA (the default when a renderer was given).
void SetListener(const glm::vec3& position, const glm::vec3& forward,
	const glm::vec3& up);

} // namespace app
