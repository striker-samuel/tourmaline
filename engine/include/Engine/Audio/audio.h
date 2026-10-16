// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
//
// Audio subsystem (SDL3): clip loading + voice mixing on a dedicated,
// CPU-pinned mixer thread.
//
// Model:
//   - Game thread calls Play/Stop/SetVolume (lock-protected command queue).
//   - One mixer thread owns all voice state, mixes float32 stereo at the
//     engine rate, and pushes to an SDL audio-device stream.
//   - The mixer thread is pinned to a dedicated core (default: last logical
//     core) and raised to time-critical priority so asset/job threads in
//     ThreadPool can't starve it.
//
// Usage:
//   smvk::audio::AudioEngine audio;
//   audio::Config cfg; cfg.dedicatedCore = -1; // last core
//   if (audio.Init(cfg)) {
//       auto clip = audio.LoadWav("content/audio/shot.wav");
//       audio.Play(clip, 0.8f, false);
//   }
//   ...
//   audio.Shutdown();
//
#include <cstdint>
#include <string>
#include <vector>

namespace smvk::audio {

struct Config {
    int sampleRate = 48000;
    int channels = 2;               // 1 = mono, 2 = stereo
    int framesPerChunk = 1024;      // mixer quantum pushed per iteration
    int maxVoices = 64;
    // Dedicated core for the mixer thread:
    //   >= 0 : pin to that core index
    //   -1   : pin to last logical core (default)
    //   -2   : no pinning (OS schedules)
    int dedicatedCore = -1;
    float masterVolume = 1.0f;
};

struct ClipHandle {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};

struct VoiceHandle {
    uint32_t id = 0; // 0 = invalid
    bool valid() const { return id != 0; }
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine() { Shutdown(); }
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Init(const Config& cfg = {});
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Loads a .wav file, converting to engine format (float32, engine rate).
    // Returns invalid handle on failure; error text via LastError().
    ClipHandle LoadWav(const std::string& path);
    ClipHandle LoadWavFromMemory(const uint8_t* data, size_t size);
    void UnloadClip(ClipHandle clip);
    bool ClipValid(ClipHandle clip) const;

    // Fire-and-forget playback. volume in [0,1], pitch = playback-rate
    // multiplier (1 = normal), loop restarts the clip at end.
    VoiceHandle Play(ClipHandle clip, float volume = 1.0f, bool loop = false, float pitch = 1.0f);
    void Stop(VoiceHandle voice);
    void StopAll();
    void SetVoiceVolume(VoiceHandle voice, float volume);
    bool IsVoicePlaying(VoiceHandle voice) const;

    void SetMasterVolume(float v);
    float GetMasterVolume() const { return config_.masterVolume; }
    void SetPaused(bool paused);

    int MixerCore() const { return mixerCore_; }
    const std::string& LastError() const { return lastError_; }

private:
    struct ClipData {
        std::vector<float> samples; // interleaved, engine channels/rate
        bool alive = false;
    };
    struct Voice {
        uint32_t id = 0;
        uint32_t clipIndex = UINT32_MAX;
        double cursor = 0.0; // fractional frame position
        float volume = 1.0f;
        float pitch = 1.0f;
        bool loop = false;
        bool playing = false;
    };
    struct Command {
        enum class Type { Play, Stop, StopAll, SetVolume, SetMaster, SetPaused } type;
        uint32_t voiceId = 0;
        uint32_t clipIndex = UINT32_MAX;
        float volume = 1.0f;
        float pitch = 1.0f;
        bool loop = false;
        bool paused = false;
    };

    void MixerLoop();
    void DrainCommands();
    void MixChunk(float* out, int frames);
    friend void RunMixerLoop(AudioEngine* engine);
    bool OpenDeviceStream();
    void CloseDeviceStream();
    ClipHandle StoreClip(std::vector<float>&& samples);

    Config config_{};
    std::vector<ClipData> clips_;
    std::vector<Voice> voices_;
    std::vector<Command> pending_; // guarded by mutex_, consumed by mixer
    mutable void* mutex_ = nullptr; // SDL_Mutex* (void* to avoid SDL include in header)
    void* mixerThread_ = nullptr;   // SDL_Thread*
    void* deviceStream_ = nullptr;  // SDL_AudioStream* bound to output device
    bool initialized_ = false;
    bool stopMixer_ = false;
    bool paused_ = false;
    uint32_t nextVoiceId_ = 1;
    int mixerCore_ = -2;
    std::string lastError_;
};

/// Pins the calling thread to a logical core. Returns the core used or -2.
/// Platform: Windows (SetThreadAffinityMask) + pthreads (Linux); no-op elsewhere.
int PinCurrentThreadToCore(int core);

/// Logical CPU count (wraps SDL_GetNumLogicalCPUCores without needing SDL headers).
int LogicalCoreCount();

} // namespace smvk::audio
