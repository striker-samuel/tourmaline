// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// AudioRuntime — see AudioRuntime.h. The spatial reasoning lives HERE
// (the mixer stays amplitude+pan-only): emitter poses from the merged
// world's global transforms, the listener from the pinned pose or the
// renderer's active camera, linear distance rolloff + right-axis pan
// per voice per frame.

#include "AudioRuntime.h"

#include <Engine/App/app.h>          // smvk::App (m_audio)
#include <Engine/App/world.h>
#include <Engine/Audio/audio.h>
#include <Engine/Camera/camera.h>    // CameraPositioner_FirstPerson
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>
#include <Engine/Vfs/vfs.h>
#include <Engine/Renderer/WorldRenderer.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace app {

namespace {

// The stable emitter key: (owning cluster, cluster-local node). A
// scene rebuild (resolve) remaps MERGED node ids — this pair never
// moves, so emitter states + their voices survive additive loads and
// RemoveScenes of OTHER scenes (the contract: only the OWNED scene's
// voices stop).
struct EmitterKey {
	smvk::Cluster* cluster;
	uint32_t node;
	bool operator==(const EmitterKey& o) const {
		return cluster == o.cluster && node == o.node;
	}
};
struct EmitterKeyHash {
	size_t operator()(const EmitterKey& k) const {
		return std::hash<void*>()(k.cluster)
			^ (std::hash<uint32_t>()(k.node) << 1);
	}
};

struct EmitterState {
	bool autoplayed = false;          // playOnLoad fired for this scene life
	smvk::audio::VoiceHandle autoplayVoice{};
	float lastVolume = -1.0f;         // change detection (mixer commands
	float lastPan = 0.0f;             // only on real changes)
	bool warnedUnresolved = false;    // one log per dead sound id
};

struct CachedClip {
	smvk::audio::ClipHandle clip{};
	bool pinned = false;              // an explicit PrefetchSound pin
};

struct ActiveVoice {
	smvk::audio::VoiceHandle voice{};
	EmitterKey emitter{};             // stops when the emitter dies
	float gainMul = 1.0f;             // PlaySound's gain multiplier
};

class AudioRuntimeElement final : public engine::IAppElement {
public:
	AudioRuntimeElement(smvk::App& host, const std::string& tappPath,
			std::shared_ptr<smvk::World*> worldHandle,
			engine::WorldRenderer* renderer)
		: m_host(host)
		, m_tappPath(tappPath)
		, m_worldHandle(std::move(worldHandle))
		, m_renderer(renderer) {
		// Singleton install (the imperative surface below). One audio
		// layer per process — a second construction replaces the first
		// (the host owns the elements; the raw pointer the free
		// functions use is only read between the element's attach and
		// detach, where the element is alive by definition).
		std::lock_guard<std::mutex> lock(SingletonMutex());
		Instance() = this;
	}

	~AudioRuntimeElement() override {
		std::lock_guard<std::mutex> lock(SingletonMutex());
		if (Instance() == this)
			Instance() = nullptr;
	}

	const char* ElementName() const override { return "AudioRuntime"; }

	// The layer records no passes (pure CPU update; see onUpdate).
	void onRender(engine::ElementFrame&) override {}

	bool onAttach() override {
		if (!m_host.m_audio.IsInitialized())
			std::cerr << "[Audio] runtime layer over a DISABLED mixer ("
				"no audio device) — bookkeeping only, voices no-op\n";
		return true;
	}

	void onUpdate(float dt) override {
		(void)dt;
		smvk::World* world = m_worldHandle ? *m_worldHandle : nullptr;
		if (!world)
			return;
		if (!EnsureScenes())
			return;
		app::ProjectScenes& scenes = *m_scenes;
		const ListenerPose listener = ResolveListener();
		// Pass 1: the authored emitters (the pak clusters' OWN scenes —
		// the durable rows; the dynamic cluster's spawned entities ride
		// pass 2 through their handles).
		std::unordered_set<EmitterKey, EmitterKeyHash> liveEmitters;
		for (size_t ci = 0; ci < world->clusters.size(); ++ci) {
			smvk::Cluster* cluster = world->clusters[ci].get();
			if (!cluster || cluster->isDynamic)
				continue;
			const auto& merged = ci < world->clusterMergedNodes.size()
				? world->clusterMergedNodes[ci]
				: std::vector<uint32_t>{};
			for (const AudioSourceRow& row :
					cluster->scene.tables.audioSources) {
				const EmitterKey key{ cluster, row.node };
				liveEmitters.insert(key);
				const uint32_t mergedNode =
					row.node < merged.size()
						? merged[row.node] : UINT32_MAX;
				UpdateEmitter(*world, key, &row.source, mergedNode,
					listener);
			}
		}
		// Pass 2: the spawned emitters (the gameplay-handle surface).
		for (const smvk::World::AudioSourceEntity& e : world->audioEntities) {
			if (!e.alive || !e.owner)
				continue;
			const EmitterKey key{ e.owner, e.clusterNode };
			liveEmitters.insert(key);
			const AudioSourceRow* row =
				FindAudioSource(e.owner->scene, e.clusterNode);
			if (row)
				UpdateEmitter(*world, key, &row->source, e.mergedNode,
					listener);
		}
		// Drop states whose emitters vanished (a RemoveScene rebuilt
		// the world; a DestroyAudioSource retired the node): their
		// voices stop, everything else survives.
		{
			auto it = m_emitters.begin();
			while (it != m_emitters.end()) {
				if (liveEmitters.count(it->first) == 0) {
					if (it->second.autoplayVoice.valid())
						m_host.m_audio.Stop(it->second.autoplayVoice);
					it = m_emitters.erase(it);
				}
				else
					++it;
			}
		}
		// Drop voices whose emitters vanished (the same test — the
		// ACTIVE list, not the autoplay slots).
		{
			auto it = m_voices.begin();
			while (it != m_voices.end()) {
				if (liveEmitters.count(it->emitter) == 0) {
					m_host.m_audio.Stop(it->voice);
					it = m_voices.erase(it);
				}
				else
					++it;
			}
		}
		// The clip-cache eviction: keep clips while ANY live emitter
		// references them or a pin exists; unload the rest (the
		// "ownership-aware" playback half — the byte-side registry is
		// ProjectScenes'; this is the decoded side).
		{
			std::unordered_set<uint64_t> referenced;
			for (size_t ci = 0; ci < world->clusters.size(); ++ci) {
				smvk::Cluster* cluster = world->clusters[ci].get();
				if (!cluster)
					continue;
				for (const AudioSourceRow& row :
						cluster->scene.tables.audioSources)
					for (const AudioSoundRef& s : row.source.sounds)
						if (s.soundId != 0)
							referenced.insert(s.soundId);
			}
			for (const smvk::World::AudioSourceEntity& e : world->audioEntities) {
				if (!e.alive || !e.owner)
					continue;
				const AudioSourceRow* row =
					FindAudioSource(e.owner->scene, e.clusterNode);
				if (row)
					for (const AudioSoundRef& s : row->source.sounds)
						if (s.soundId != 0)
							referenced.insert(s.soundId);
			}
			for (auto& [id, cached] : m_clips) {
				if (cached.pinned || referenced.count(id) != 0)
					continue;
				if (cached.clip.valid())
					m_host.m_audio.UnloadClip(cached.clip);
				cached.clip = {};
				// (the entry stays — a later play re-decodes)
			}
		}
	}

	// ---- the imperative surface --------------------------------------

	bool Prefetch(uint64_t soundId, std::string& err) {
		CachedClip& cached = m_clips[soundId];
		if (!cached.clip.valid()
				&& !DecodeClip(soundId, cached.clip, err))
			return false;
		cached.pinned = true;
		// The byte-side pin rides ProjectScenes' counter (the pak-side
		// ownership-aware prefetch; a missing scene layer is fine —
		// the decoded pin above is the playback guarantee).
		(void)m_scenes->Prefetch(soundId, err);
		return true;
	}

	bool Release(uint64_t soundId) {
		if (!EnsureScenes())
			return false;
		auto it = m_clips.find(soundId);
		if (it == m_clips.end() || !it->second.pinned)
			return false;
		it->second.pinned = false;
		(void)m_scenes->Release(soundId);
		return true;
	}

	smvk::audio::VoiceHandle Play(smvk::World& world,
		uint32_t mergedNode, uint64_t soundId, float gain,
		std::string* err) {
		// Resolve the emitter: any live row whose merged node matches
		// (authored via clusterMergedNodes, spawned via the registry).
		const EmitterKey key = FindEmitterByMergedNode(world, mergedNode);
		if (key.cluster == nullptr) {
			if (err)
				*err = "node " + std::to_string(mergedNode)
					+ " carries no live audio source";
			return {};
		}
		const AudioSourceRow* row =
			FindAudioSource(key.cluster->scene, key.node);
		if (!row) {
			if (err)
				*err = "the emitter's durable row vanished (a rebuild"
					" raced the play — retry next frame)";
			return {};
		}
		CachedClip& cached = m_clips[soundId];
		std::string decodeErr;
		if (!cached.clip.valid()
				&& !DecodeClip(soundId, cached.clip, decodeErr)) {
			if (err)
				*err = decodeErr;
			return {};
		}
		if (err)
			err->clear();
		const float vol = std::clamp(row->source.volume * gain, 0.0f,
			1.0f);
		smvk::audio::VoiceHandle voice = m_host.m_audio.Play(
			cached.clip, vol, false, 1.0f);
		if (voice.valid())
			m_voices.push_back(
				ActiveVoice{ voice, key, std::max(gain, 0.0f) });
		return voice;
	}

	void Stop(smvk::audio::VoiceHandle voice) {
		if (!voice.valid())
			return;
		m_host.m_audio.Stop(voice);
		for (auto it = m_voices.begin(); it != m_voices.end(); ++it)
			if (it->voice.id == voice.id) {
				m_voices.erase(it);
				break;
			}
	}

	bool SetVolume(smvk::World& world, uint32_t mergedNode, float vol) {
		bool wrote = false;
		if (mergedNode < world.scene.hierarchy.size()) {
			AudioSourceRow* row =
				FindAudioSource(world.scene, mergedNode);
			if (row) {
				row->source.volume = std::max(vol, 0.0f);
				wrote = true;
			}
		}
		// The durable copy (the owning cluster's row) keeps the edit
		// across resolves.
		const EmitterKey key = FindEmitterByMergedNode(world, mergedNode);
		if (key.cluster) {
			AudioSourceRow* row =
				FindAudioSource(key.cluster->scene, key.node);
			if (row) {
				row->source.volume = std::max(vol, 0.0f);
				wrote = true;
			}
		}
		return wrote;
	}

	void SetListener(const glm::vec3& pos, const glm::vec3& fwd,
		const glm::vec3& up) {
		if (glm::length(fwd) < 1e-6f) {
			m_listenerPinned = false; // the reset form
			return;
		}
		m_listenerPinned = true;
		m_listener.position = pos;
		m_listener.forward = glm::normalize(fwd);
		m_listener.up = glm::length(up) > 1e-6f
			? glm::normalize(up) : glm::vec3(0.0f, 1.0f, 0.0f);
	}

private:
	struct ListenerPose {
		glm::vec3 position{ 0.0f };
		glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
		glm::vec3 up{ 0.0f, 1.0f, 0.0f };
	};

	ListenerPose ResolveListener() const {
		if (m_listenerPinned || !m_renderer)
			return m_listener;
		// Follow the renderer's active camera (the default; explicit
		// selection stays available through SetListener).
		ListenerPose pose;
		const CameraPositioner_FirstPerson& cam =
			m_renderer->CameraPositioner();
		pose.position = cam.cameraPosition_;
		pose.forward = cam.getForward();
		const glm::quat q = cam.getOrientation();
		pose.up = q * glm::vec3(0.0f, 1.0f, 0.0f);
		return pose;
	}

	// Sound bytes -> a mixer clip (the pak's Sound blob through the
	// runtime façade). Cached by id; failures leave the cache empty so
	// the next play retries.
	bool DecodeClip(uint64_t soundId, smvk::audio::ClipHandle& out,
		std::string& err) {
		std::string rErr;
		auto bytes = m_scenes->Runtime().ReadAsset(soundId, rErr);
		if (!bytes || bytes->empty()) {
			err = rErr.empty()
				? ("sound id 0x" + std::to_string(soundId)
					+ " is not readable through the project")
				: rErr;
			return false;
		}
		out = m_host.m_audio.LoadWavFromMemory(bytes->data(),
			bytes->size());
		if (!out.valid()) {
			err = "the sound decode failed (0x"
				+ std::to_string(soundId) + "): "
				+ m_host.m_audio.LastError();
			return false;
		}
		return true;
	}

	EmitterKey FindEmitterByMergedNode(smvk::World& world,
		uint32_t mergedNode) const {
		for (size_t ci = 0; ci < world.clusters.size(); ++ci) {
			smvk::Cluster* cluster = world.clusters[ci].get();
			if (!cluster)
				continue;
			const auto& merged = ci < world.clusterMergedNodes.size()
				? world.clusterMergedNodes[ci]
				: std::vector<uint32_t>{};
			for (const AudioSourceRow& row :
					cluster->scene.tables.audioSources) {
				const uint32_t m = row.node < merged.size()
					? merged[row.node] : UINT32_MAX;
				if (m == mergedNode)
					return EmitterKey{ cluster, row.node };
			}
		}
		for (const smvk::World::AudioSourceEntity& e : world.audioEntities)
			if (e.alive && e.mergedNode == mergedNode && e.owner)
				return EmitterKey{ e.owner, e.clusterNode };
		return EmitterKey{ nullptr, 0 };
	}

	// One emitter's per-frame advance: autoplay (once), attenuation +
	// pan for the autoplay voice, per-voice updates for PlaySound's
	// actives on the same emitter.
	void UpdateEmitter(smvk::World& world, const EmitterKey& key,
		const SceneAudioSource* def, uint32_t mergedNode,
		const ListenerPose& listener) {
		if (!def || mergedNode == UINT32_MAX
				|| mergedNode >= world.scene.globalTransform.size())
			return;
		EmitterState& state = m_emitters[key];
		const glm::vec3 pos =
			glm::vec3(world.scene.globalTransform[mergedNode][3]);
		// Autoplay: the authored playOnLoad fires ONCE per scene life
		// (a rebuild drops the state, a fresh scene re-fires — the
		// authored intent).
		if (!state.autoplayed && def->playOnLoad
				&& !def->sounds.empty()) {
			state.autoplayed = true;
			const AudioSoundRef& slot = def->sounds.front();
			if (slot.soundId != 0) {
				std::string err;
				CachedClip& cached = m_clips[slot.soundId];
				if (!cached.clip.valid()
						&& DecodeClip(slot.soundId, cached.clip, err)) {
					state.autoplayVoice = m_host.m_audio.Play(
						cached.clip,
						std::clamp(def->volume * slot.gain, 0.0f, 1.0f),
						slot.loop, 1.0f);
					if (state.autoplayVoice.valid())
						std::cerr << "[Audio] autoplay: node "
							<< mergedNode << " -> sound 0x" << std::hex
							<< slot.soundId << std::dec
							<< (slot.loop ? " (loop)" : "") << "\n";
				}
				else if (!state.warnedUnresolved) {
					state.warnedUnresolved = true;
					std::cerr << "[Audio] autoplay failed (sound 0x"
						+ std::to_string(slot.soundId) + "): " + err
						+ "\n";
				}
			}
			else if (!state.warnedUnresolved) {
				state.warnedUnresolved = true;
				std::cerr << "[Audio] autoplay skipped: the emitter's"
					" first slot carries no cooked sound id (a bare"
					" cook? rebuild with the project ids)\n";
			}
		}
		// Spatialization: linear rolloff between min/max distance, a
		// right-axis pan (the listener-relative direction clamped —
		// the v1 curve), 2D sources bypass both.
		float vol = std::clamp(def->volume, 0.0f, 1.0f);
		float pan = 0.0f;
		if (def->spatial) {
			const glm::vec3 toEmitter = pos - listener.position;
			const float dist = glm::length(toEmitter);
			const float minD = std::max(def->minDistance, 0.0f);
			const float maxD = std::max(def->maxDistance, minD + 1e-3f);
			const float atten = dist <= minD
				? 1.0f
				: dist >= maxD ? 0.0f : (maxD - dist) / (maxD - minD);
			vol = std::clamp(vol * atten, 0.0f, 1.0f);
			if (dist > 1e-4f) {
				const glm::vec3 right = glm::normalize(
					glm::cross(listener.forward, listener.up));
				pan = std::clamp(
					glm::dot(toEmitter / dist, right), -1.0f, 1.0f);
			}
		}
		// Change-detected mixer commands (no per-frame command spam
		// when nothing moved).
		auto apply = [&](smvk::audio::VoiceHandle voice, float mul) {
			if (!voice.valid())
				return;
			const float v = std::clamp(vol * mul, 0.0f, 1.0f);
			m_host.m_audio.SetVoiceSpatial(voice, v, pan);
		};
		if (state.autoplayVoice.valid()) {
			const AudioSoundRef* slot = !def->sounds.empty()
				? &def->sounds.front() : nullptr;
			apply(state.autoplayVoice, slot ? slot->gain : 1.0f);
			// Retire a finished one-shot autoplay (looping ones stay).
			if (!m_host.m_audio.IsVoicePlaying(state.autoplayVoice))
				state.autoplayVoice = {};
		}
		for (ActiveVoice& v : m_voices)
			if (v.emitter == key)
				apply(v.voice, v.gainMul);
	}

	// The lazily-built project layers (see the MakeAudioRuntime note:
	// built at FIRST USE — ProjectRuntime::Load parses the manifest,
	// ProjectScenes opens + mounts the paks into the ELEMENT'S OWN
	// overlay (the apphost's attach-time vfs is scoped to the world
	// source call, nothing persists). v1 cost note: the pak IMAGES
	// are opened per layer (the apphost's preloads are not handed
	// out) — fine at sample scale; a shared-runtime seam is the
	// follow-up.
	std::string m_tappPath;
	std::optional<ProjectRuntime> m_rt;
	std::unique_ptr<ProjectScenes> m_scenes;
	smvk::OverlayVfs m_vfs;
	// Builds both on first use; false (once, loud) when the manifest
	// or the paks fail — the layer then no-ops instead of retry-
	// spamming.
	bool EnsureScenes() {
		if (m_scenes)
			return true;
		if (m_sceneInitFailed)
			return false;
		std::string err;
		m_rt = ProjectRuntime::Load(m_tappPath, err);
		if (!m_rt) {
			m_sceneInitFailed = true;
			std::cerr << "[Audio] the project manifest failed: " << err
				<< " (" << m_tappPath << ") - the audio layer no-ops\n";
			return false;
		}
		m_scenes = std::make_unique<ProjectScenes>(*m_rt, m_vfs);
		if (!m_scenes->Valid()) {
			m_sceneInitFailed = true;
			std::cerr << "[Audio] the project's paks did not open: "
				<< m_scenes->OpenError() << " - the audio layer no-ops\n";
			return false;
		}
		return true;
	}
	bool m_sceneInitFailed = false;
	smvk::App& m_host;
	std::shared_ptr<smvk::World*> m_worldHandle;
	engine::WorldRenderer* m_renderer;
	std::unordered_map<EmitterKey, EmitterState, EmitterKeyHash>
		m_emitters;
	std::unordered_map<uint64_t, CachedClip> m_clips;
	std::vector<ActiveVoice> m_voices;
	bool m_listenerPinned = false;
	ListenerPose m_listener{};

public:
	// The singleton plumbing (the free functions below).
	static AudioRuntimeElement*& Instance() {
		static AudioRuntimeElement* inst = nullptr;
		return inst;
	}
	static std::mutex& SingletonMutex() {
		static std::mutex m;
		return m;
	}
};

} // namespace

std::shared_ptr<engine::IAppElement> MakeAudioRuntime(
	smvk::App& host, const std::string& tappPath,
	std::shared_ptr<smvk::World*> worldHandle,
	engine::WorldRenderer* renderer) {
	return std::make_shared<AudioRuntimeElement>(host, tappPath,
		std::move(worldHandle), renderer);
}

// ---- the imperative surface (the singleton forwarders) ----------------

bool PrefetchSound(uint64_t soundId, std::string& err) {
	std::lock_guard<std::mutex> lock(AudioRuntimeElement::SingletonMutex());
	AudioRuntimeElement* inst = AudioRuntimeElement::Instance();
	return inst ? inst->Prefetch(soundId, err) : false;
}

bool ReleaseSound(uint64_t soundId) {
	std::lock_guard<std::mutex> lock(AudioRuntimeElement::SingletonMutex());
	AudioRuntimeElement* inst = AudioRuntimeElement::Instance();
	return inst ? inst->Release(soundId) : false;
}

smvk::audio::VoiceHandle PlaySound(smvk::World& world,
	uint32_t emitterNode, uint64_t soundId, float gain,
	std::string* err) {
	std::lock_guard<std::mutex> lock(AudioRuntimeElement::SingletonMutex());
	AudioRuntimeElement* inst = AudioRuntimeElement::Instance();
	return inst ? inst->Play(world, emitterNode, soundId, gain, err)
		: smvk::audio::VoiceHandle{};
}

void StopSound(smvk::audio::VoiceHandle voice) {
	std::lock_guard<std::mutex> lock(AudioRuntimeElement::SingletonMutex());
	AudioRuntimeElement* inst = AudioRuntimeElement::Instance();
	if (inst)
		inst->Stop(voice);
}

bool SetAudioSourceVolume(smvk::World& world, uint32_t emitterNode,
	float volume) {
	std::lock_guard<std::mutex> lock(AudioRuntimeElement::SingletonMutex());
	AudioRuntimeElement* inst = AudioRuntimeElement::Instance();
	return inst ? inst->SetVolume(world, emitterNode, volume) : false;
}

void SetListener(const glm::vec3& position, const glm::vec3& forward,
	const glm::vec3& up) {
	std::lock_guard<std::mutex> lock(AudioRuntimeElement::SingletonMutex());
	AudioRuntimeElement* inst = AudioRuntimeElement::Instance();
	if (inst)
		inst->SetListener(position, forward, up);
}

} // namespace app
