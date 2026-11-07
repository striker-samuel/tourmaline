// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
//
// Audio subsystem: clip loading + voice mixing + AUTHORED AUDIO GRAPH
// INSTANCES (docs/audio_graph.md) on a dedicated, CPU-pinned mixer
// thread.
//
// THE RENDERING CORE is miniaudio's node graph (vendored 0.11.26,
// external/miniaudio — the device-less configuration of engine/src/
// Audio/miniaudio_config.h): ONE ma_engine hosts every voice, bus and
// effect node; the engine renders each chunk through
// ma_engine_read_pcm_frames and pushes it into the SAME SDL3 audio
// stream as always. SDL3 keeps the DEVICE; paks + the content
// registry keep the CONTENT; miniaudio owns the DSP: per-voice
// source/pan/pitch/resample machinery, the biquad + delay effect
// nodes, and the sound-group submix buses.
//
// THE MODEL (the animator parallel — a graph asset is a skeleton, a
// cue is an anim clip, an instance is an animator):
//   - Game thread calls Play/Stop/SetVoiceSpatial or the INSTANCE
//     facade (lock-protected command queue; the legacy voice API is
//     unchanged and what the scene-aware layer drives).
//   - One mixer thread owns ALL miniaudio state: it drains commands,
//     advances instance cue timelines (automation + source events at
//     the CHUNK cadence — immune to frame stalls), renders the graph,
//     and pushes to the SDL stream. Graph mutations and parameter
//     scheduling happen ONLY at chunk boundaries; steady-state cue
//     starts/crossfades/param sets are pure gain/seek/reconfig ops
//     with ZERO allocation (node/sound objects are pre-built at
//     instance create — the one sanctioned allocation burst).
//   - The mixer thread is pinned to a dedicated core (default: last
//     logical core) and raised to time-critical priority so asset/job
//     threads in ThreadPool can't starve it.
//
// Instances (engine-owned registries, generational handles — see
// smvk::AudioInstanceHandle):
//   tour::audio::AudioEngine& ae = app.m_audio;
//   auto h = ae.CreateInstance(doc, ResolveClip, &ctx, &err);
//   tour::audio::AudioInstance inst(ae, h);   // the facade
//   inst.play();                              // the default cue
//   inst.set("wind", 0.75f);                  // an exposed parameter
//   inst.crossFade(stormOrdinal, 2.0f);       // an authored cue
//   inst.stop();
// The DOC is an immutable authored asset (tour::asset::AudioGraphDoc,
// engine/Engine/Asset/audiograph.h); the instance owns its copy. Clip
// resolution (sound VFS path -> ClipHandle) runs on the CALLER thread
// BEFORE the create command — pak reads + WAV decode never touch the
// mixer thread.
//
// Usage (the legacy voice lane):
//   tour::audio::AudioEngine audio;
//   audio::Config cfg; cfg.dedicatedCore = -1; // last core
//   if (audio.Init(cfg)) {
//       auto clip = audio.LoadWav("content/audio/shot.wav");
//       audio.Play(clip, 0.8f, false);
//   }
//   ...
//   audio.Shutdown();
//

#include <Engine/Asset/audiograph.h>    // AudioGraphDoc (the asset model)
#include <Engine/Scene/entity_handle.h> // AudioInstanceHandle

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tour::audio {

struct Config {
    int sampleRate = 48000;
    int channels = 2;          // 1 = mono, 2 = stereo
    int framesPerChunk = 1024; // mixer quantum pushed per iteration
    int maxVoices = 64;
    // Dedicated core for the mixer thread:
    //   >= 0 : pin to that core index
    //   -1   : pin to last logical core (default)
    //   -2   : no pinning (OS schedules)
    int dedicatedCore = -1;
    float masterVolume = 1.0f;
    // OFFLINE/TEST mode: no SDL audio device, no mixer thread. The
    // caller drives rendering with PumpOffline() (deterministic, no
    // wall clock — the test lane + a future offline bounce).
    bool deviceless = false;
};

struct ClipHandle {
    uint32_t index = UINT32_MAX;
    bool valid() const {
        return index != UINT32_MAX;
    }
};

struct VoiceHandle {
    uint32_t id = 0; // 0 = invalid
    bool valid() const {
        return id != 0;
    }
};

// The voice's ROUTE (the environmental bus contract, docs/
// audio_graph.md): DIRECT attaches to the endpoint (the default —
// every pre-room caller keeps its exact old behavior); WORLDSFX
// attaches to the shared environmental bus, whose listener-driven
// Schroeder send the scene-aware layer drives from the listener's
// room coverage. A failed bus build degrades WORLDSFX to DIRECT
// (logged once — audio keeps working, the reverb is simply off).
enum class AudioRoute : uint8_t { Direct = 0, WorldSfx = 1 };

// The environmental reverb TARGET the scene-aware layer computes
// each frame from the listener's room coverage (the ordered-coverage
// blend over the room rows). Applied at a chunk boundary, smoothed
// on the mixer thread — never sample-stepped from the game thread.
struct EnvironmentReverb {
    float wet = 0.0f;     // 0..1, the send gain (0 = dry)
    float rt60 = 0.35f;   // s, the outdoor/default baseline
    float damping = 0.5f; // 0..1
};

// Clip resolution for instance creation (called on the GAME thread):
// returns the decoded clip for a sound asset's VFS path, or an
// invalid handle to fail the create (the named path lands in `err`).
// The app layer's implementation reads through the content session
// (Sound-kind blobs by path) and decodes through LoadWavFromMemory.
using AudioClipResolver = ClipHandle (*)(void* user, const char* soundVfsPath);

class AudioEngine {
  public:
    AudioEngine() = default;
    ~AudioEngine() {
        Shutdown();
    }
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Init(const Config& cfg = {});
    void Shutdown();
    bool IsInitialized() const {
        return initialized_;
    }

    // Loads a .wav file, converting to engine format (float32, engine rate).
    // Returns invalid handle on failure; error text via LastError().
    ClipHandle LoadWav(const std::string& path);
    ClipHandle LoadWavFromMemory(const uint8_t* data, size_t size);
    void UnloadClip(ClipHandle clip);
    bool ClipValid(ClipHandle clip) const;

    // Fire-and-forget playback. volume in [0,1], pitch = playback-rate
    // multiplier (1 = normal), loop restarts the clip at end. The
    // ROUTE picks the attachment: Direct (the endpoint, the default)
    // or the shared environmental WorldSfx bus (the scene-aware layer
    // routes spatial emitters there so listener-driven rooms reverber
    // them; 2D/music/UI stay direct).
    VoiceHandle Play(ClipHandle clip,
                     float volume = 1.0f,
                     bool loop = false,
                     float pitch = 1.0f,
                     AudioRoute route = AudioRoute::Direct);
    void Stop(VoiceHandle voice);
    void StopAll();
    void SetVoiceVolume(VoiceHandle voice, float volume);
    // SPATIAL playback control (the scene-aware audio layer feeds
    // these per frame): pan in [-1, +1] (full-left .. full-right,
    // constant-power equal-loudness at the extremes; 0 = center — the
    // pre-pan behavior), and the combined pan+volume set. The mixer
    // clamps pan to [-1,1]. Stereo engine configs only: mono configs
    // apply the volume and drop the pan.
    void SetVoicePan(VoiceHandle voice, float pan);
    void SetVoiceSpatial(VoiceHandle voice, float volume, float pan);
    bool IsVoicePlaying(VoiceHandle voice) const;

    // The constant-power stereo pan pair for a pan value in [-1,+1]
    // (left, right gains). Equal-loudness at the extremes (unlike
    // linear pan, which attenuates -3dB at center and audibly dips
    // moving sounds). Pure math, exposed for the spatial layer +
    // tests.
    static void ConstantPowerPan(float pan, float& left, float& right);

    void SetMasterVolume(float v);
    float GetMasterVolume() const {
        return config_.masterVolume;
    }
    void SetPaused(bool paused);

    // Sets the ENVIRONMENTAL REVERB target (the shared world-SFX
    // bus's send): the wet gain + the Schroeder's rt60/damping,
    // clamped, applied at the next chunk boundary and smoothed on
    // the mixer thread (the listener's room blend glides, never
    // steps). A failed bus build makes this a logged no-op.
    void SetEnvironmentReverb(const EnvironmentReverb& target);

    // ---- AUTHORED AUDIO GRAPH INSTANCES (docs/audio_graph.md) -----
    // Creates a runtime instance of an authored graph: resolves every
    // source's clip through `resolver` (CALLER thread — pak reads +
    // WAV decode never touch the mixer thread; a failed resolution
    // fails the create LOUDLY with the named path), then hands the
    // built payload to the mixer thread, which constructs the whole
    // node set (buses, effects, per-(source,cue) voices — the ONE
    // allocation burst; steady-state mutation is alloc-free) at the
    // next chunk boundary. The instance OWNS its doc copy.
    // `doc` must have passed ParseAudioGraphText (the parser's
    // structural guarantees are the build's preconditions).
    smvk::AudioInstanceHandle CreateInstance(const asset::AudioGraphDoc& doc,
                                             AudioClipResolver resolver,
                                             void* resolverUser,
                                             std::string* err = nullptr);
    // Destroys the instance (stops its voices, tears its node set
    // down at the next chunk boundary). Stale handles fail false.
    bool DestroyInstance(smvk::AudioInstanceHandle handle);
    bool IsInstanceAlive(smvk::AudioInstanceHandle handle) const;

    // OFFLINE/TEST rendering (deviceless mode ONLY): drains every
    // pending command, advances instance timelines by `frames`,
    // renders exactly `frames` interleaved float frames into `out`
    // (caller-owned, engine channel count wide) — one mixer-thread
    // iteration without the thread. Returns false when not
    // initialized deviceless. NOT for the live path.
    bool PumpOffline(float* out, int frames);

    int MixerCore() const {
        return mixerCore_;
    }
    const std::string& LastError() const {
        return lastError_;
    }

  private:
    // The mixer-thread core (ma_engine, the clip store, voices,
    // instances) — an opaque pimpl: the public header never sees a
    // miniaudio type (the vendored include path is PRIVATE to
    // engine/src/Audio). The clip store lives here because a clip's
    // decoded bytes are wrapped by an ma_audio_buffer (a miniaudio
    // type). The GAME thread reaches it only through the mutex-guarded
    // store/unload entry points in the .cpp.
    struct Core;
    friend struct Core;
    // A resolved instance-create payload (the doc copy + the clip
    // table); definition + ownership transfer in the .cpp.
    struct CreatePayload;
    friend struct CreatePayload;
    // The instance facade drives the engine through the same private
    // command surface as its own methods (the tour::Animator pattern).
    friend class AudioInstance;

    struct InstanceSlot {
        uint32_t generation = 0; // 0 = never allocated; first live = 1
        bool alive = false;
        // The control-side doc mirror (name -> ordinal lookups for the
        // facade). The mixer-side state owns its own reference.
        std::shared_ptr<const asset::AudioGraphDoc> doc;
    };
    // Legacy command queue shape (POD-ish; instance commands carry
    // indices into the registries).
    struct Command {
        enum class Type {
            Play,
            Stop,
            StopAll,
            setVolume,
            SetPan,
            SetSpatial,
            SetMaster,
            SetPaused,
            UnloadClip,
            // Instance lane (generation-validated at the drain)
            InstanceCreate,
            InstanceDestroy,
            InstancePlay,
            InstanceCrossFade,
            InstanceStop,
            InstanceSetParam,
            InstanceSetVolume,
            // The environmental bus (the shared world-SFX send)
            SetEnvironment,
        } type;
        uint32_t voiceId = 0;
        uint32_t clipIndex = UINT32_MAX;
        uint32_t slot = UINT32_MAX;    // instance slot index
        uint32_t generation = 0;       // instance slot generation
        uint32_t ordinal = UINT32_MAX; // cue / param ordinal
        float volume = 1.0f;
        float pan = 0.0f;
        float pitch = 1.0f;
        float fadeSeconds = 0.0f;
        // The environmental target (SetEnvironment).
        float envRt60 = 0.35f;
        float envDamping = 0.5f;
        // The voice route (Play): 0 = direct, 1 = world-SFX.
        uint8_t route = 0;
        bool loop = false;
        bool paused = false;
        // InstanceCreate only (ownership transfers at the drain).
        CreatePayload* create = nullptr;
    };

    void MixerLoop();
    void DrainCommands();
    void MixChunk(float* out, int frames);
    friend void RunMixerLoop(AudioEngine* engine);
    bool OpenDeviceStream();
    void CloseDeviceStream();
    ClipHandle StoreClip(std::vector<float>&& samples);
    void PushCommand(Command&& c);
    bool ValidateInstance(smvk::AudioInstanceHandle handle) const;

    Config config_{};
    // Game-side mirrors under mutex_ (the authoritative clip store
    // lives in Core): the alive flags ClipValid answers from + the
    // mixer-published playback snapshot (live voice ids / instance
    // playing bits) the const queries scan.
    std::vector<uint8_t> clipAlive_;
    std::vector<uint32_t> liveVoiceIds_;
    std::vector<uint8_t> instancePlaying_;
    // Instance registries (control side; the mixer side lives in Core)
    std::vector<InstanceSlot> instanceSlots_;
    std::vector<uint32_t> instanceFree_;
    std::vector<Command> pending_;  // guarded by mutex_, consumed by mixer
    mutable void* mutex_ = nullptr; // SDL_Mutex* (void* to avoid SDL include in header)
    void* mixerThread_ = nullptr;   // SDL_Thread*
    void* deviceStream_ = nullptr;  // SDL_AudioStream* bound to output device
    Core* core_ = nullptr;          // mixer-thread-owned state (pimpl)
    bool initialized_ = false;
    bool stopMixer_ = false;
    bool paused_ = false;
    uint32_t nextVoiceId_ = 1;
    int mixerCore_ = -2;
    std::string lastError_;
};

// ---- THE INSTANCE FACADE (the tour::Animator parallel) ----------------
// A lightweight borrowed view over AudioEngine-owned state. Every
// operation validates the handle; a destroyed instance fails every
// call silently. Cues + params address by ORDINAL (the generated
// project.h constants carry them) or by NAME (resolved against the
// graph doc the instance was created from).
class AudioInstance {
  public:
    AudioInstance() = default;
    AudioInstance(AudioEngine& engine, smvk::AudioInstanceHandle handle)
        : m_engine(&engine), m_handle(handle) {}

    [[nodiscard]] explicit operator bool() const;
    // (Re)starts the graph's DEFAULT cue (a running cue is replaced,
    // the classic Play semantics — no fade).
    void play();
    // Starts a cue by ordinal / name (ordinal = the cue's index in
    // the authored doc; the generated project::audiocues constants
    // bundle it). Unknown names fail silently.
    void playCue(uint32_t cueOrdinal);
    void playCue(const char* cueName);
    // Cross-fades to a cue over fadeSeconds: the running cue's
    // sources + automation ramp out as the target's ramp in (the
    // AnimatorCrossFade two-layer weight model). fadeSeconds <= 0 is
    // an immediate playCue.
    void crossFade(uint32_t cueOrdinal, float fadeSeconds);
    void crossFade(const char* cueName, float fadeSeconds);
    // Stops the instance (every cue layer's sources stop; the node
    // set stays built for a later Play).
    void stop();
    // Sets an exposed parameter (by ordinal / name) IMMEDIATELY. The
    // value holds until the active cue's automation for that param
    // reaches its last keyframe; past it the param is manual again
    // (cue entry captures the current value as the automation
    // baseline).
    void set(uint32_t paramOrdinal, float value);
    void set(const char* paramName, float value);
    // The instance-level output gain (the instance bus volume,
    // separate from any authored master node).
    void setVolume(float volume);
    [[nodiscard]] bool playing() const;

    [[nodiscard]] smvk::AudioInstanceHandle handle() const {
        return m_handle;
    }

  private:
    AudioEngine* m_engine = nullptr;
    smvk::AudioInstanceHandle m_handle{};
};

/// Pins the calling thread to a logical core. Returns the core used or -2.
/// Platform: Windows (SetThreadAffinityMask) + pthreads (Linux); no-op elsewhere.
int PinCurrentThreadToCore(int core);

/// Logical CPU count (wraps SDL_GetNumLogicalCPUCores without needing SDL headers).
int LogicalCoreCount();

} // namespace tour::audio

// COMPAT (the namespace/RHI boundary audit, todo item 8.4): the audio
// subsystem is ENGINE-OWNED and lives in tour::audio; these smvk::
// aliases keep the existing call sites compiling through the consumer
// migration. RETIRE them as call sites move to tour::audio. (The
// graph-era additions — AudioClipResolver, AudioInstance — carry NO
// aliases: new surface is tour::-only from day one, never a new R7
// row.)
namespace smvk::audio {
using ::tour::audio::AudioEngine;
using ::tour::audio::ClipHandle;
using ::tour::audio::Config;
using ::tour::audio::LogicalCoreCount;
using ::tour::audio::PinCurrentThreadToCore;
using ::tour::audio::VoiceHandle;
} // namespace smvk::audio
