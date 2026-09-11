#include <Engine/Audio/audio.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace smvk::audio {

int LogicalCoreCount() {
    int n = SDL_GetNumLogicalCPUCores();
    return n > 0 ? n : 4;
}

int PinCurrentThreadToCore(int core) {
    int count = LogicalCoreCount();
    if (core < 0) {
        if (core == -1)
            core = count - 1; // dedicated: last core
        else
            return -2;
    }
    if (core >= count)
        core = count - 1;
#ifdef _WIN32
    HANDLE h = GetCurrentThread();
    DWORD_PTR mask = (core >= 64) ? (DWORD_PTR)~0 : ((DWORD_PTR)1 << core);
    DWORD_PTR prev = SetThreadAffinityMask(h, mask);
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    (void)prev;
    return core;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_t self = pthread_self();
    if (pthread_setaffinity_np(self, sizeof(set), &set) == 0)
        return core;
    return -2;
#else
    (void)core;
    return -2; // macOS and others: no portable pinning; thread still dedicated
#endif
}

static int SDLCALL MixerThreadEntry(void* data) {
    auto* self = static_cast<AudioEngine*>(data);
    // MixerLoop is private; friend-free approach: cast via public detail is
    // avoided by exposing a small trampoline through Shutdown/Init only.
    // We call back through a lambda stored in thread-local; simplest is to
    // reinterpret: AudioEngine declares this function as friend.
    extern void RunMixerLoop(AudioEngine*);
    RunMixerLoop(self);
    return 0;
}

void RunMixerLoop(AudioEngine* engine);

bool AudioEngine::Init(const Config& cfg) {
    if (initialized_)
        return true;
    config_ = cfg;
    if (config_.channels != 1 && config_.channels != 2)
        config_.channels = 2;
    if (config_.sampleRate <= 0)
        config_.sampleRate = 48000;
    if (config_.framesPerChunk <= 0)
        config_.framesPerChunk = 1024;
    if (config_.maxVoices <= 0)
        config_.maxVoices = 64;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            lastError_ = std::string("SDL audio init failed: ") + SDL_GetError();
            return false;
        }
    }

    mutex_ = SDL_CreateMutex();
    if (!mutex_) {
        lastError_ = std::string("SDL_CreateMutex failed: ") + SDL_GetError();
        return false;
    }

    voices_.reserve(static_cast<size_t>(config_.maxVoices));

    if (!OpenDeviceStream()) {
        SDL_DestroyMutex(static_cast<SDL_Mutex*>(mutex_));
        mutex_ = nullptr;
        return false;
    }

    stopMixer_ = false;
    mixerCore_ = config_.dedicatedCore;
    mixerThread_ = SDL_CreateThread(MixerThreadEntry, "AudioMixer", this);
    if (!mixerThread_) {
        lastError_ = std::string("SDL_CreateThread failed: ") + SDL_GetError();
        CloseDeviceStream();
        SDL_DestroyMutex(static_cast<SDL_Mutex*>(mutex_));
        mutex_ = nullptr;
        return false;
    }

    initialized_ = true;
    return true;
}

void AudioEngine::Shutdown() {
    if (!initialized_) {
        if (mixerThread_) {
            stopMixer_ = true;
            SDL_WaitThread(static_cast<SDL_Thread*>(mixerThread_), nullptr);
            mixerThread_ = nullptr;
        }
        if (deviceStream_) {
            CloseDeviceStream();
        }
        if (mutex_) {
            SDL_DestroyMutex(static_cast<SDL_Mutex*>(mutex_));
            mutex_ = nullptr;
        }
        return;
    }
    stopMixer_ = true;
    if (mixerThread_) {
        SDL_WaitThread(static_cast<SDL_Thread*>(mixerThread_), nullptr);
        mixerThread_ = nullptr;
    }
    CloseDeviceStream();
    if (mutex_) {
        SDL_DestroyMutex(static_cast<SDL_Mutex*>(mutex_));
        mutex_ = nullptr;
    }
    voices_.clear();
    clips_.clear();
    pending_.clear();
    initialized_ = false;
}

bool AudioEngine::OpenDeviceStream() {
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = config_.channels;
    spec.freq = config_.sampleRate;
    deviceStream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!deviceStream_) {
        lastError_ = std::string("SDL_OpenAudioDeviceStream failed: ") + SDL_GetError();
        return false;
    }
    SDL_ResumeAudioStreamDevice(static_cast<SDL_AudioStream*>(deviceStream_));
    return true;
}

void AudioEngine::CloseDeviceStream() {
    if (deviceStream_) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(deviceStream_));
        deviceStream_ = nullptr;
    }
}

ClipHandle AudioEngine::LoadWav(const std::string& path) {
    SDL_AudioSpec srcSpec{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.c_str(), &srcSpec, &buf, &len)) {
        lastError_ = std::string("SDL_LoadWAV failed (") + path + "): " + SDL_GetError();
        return {};
    }
    SDL_AudioSpec dstSpec{};
    dstSpec.format = SDL_AUDIO_F32;
    dstSpec.channels = config_.channels;
    dstSpec.freq = config_.sampleRate;
    SDL_AudioStream* conv = SDL_CreateAudioStream(&srcSpec, &dstSpec);
    if (!conv) {
        lastError_ = std::string("SDL_CreateAudioStream failed: ") + SDL_GetError();
        SDL_free(buf);
        return {};
    }
    if (!SDL_PutAudioStreamData(conv, buf, static_cast<int>(len)) || !SDL_FlushAudioStream(conv)) {
        lastError_ = std::string("SDL audio convert failed: ") + SDL_GetError();
        SDL_free(buf);
        SDL_DestroyAudioStream(conv);
        return {};
    }
    SDL_free(buf);
    std::vector<float> samples;
    samples.resize(4096);
    size_t totalFloats = 0;
    for (;;) {
        if (totalFloats == samples.size())
            samples.resize(samples.size() * 2);
        int want = static_cast<int>((samples.size() - totalFloats) * sizeof(float));
        int got = SDL_GetAudioStreamData(conv, samples.data() + totalFloats, want);
        if (got <= 0)
            break;
        totalFloats += static_cast<size_t>(got) / sizeof(float);
    }
    SDL_DestroyAudioStream(conv);
    samples.resize(totalFloats);
    if (samples.empty()) {
        lastError_ = path + ": no audio frames after conversion";
        return {};
    }
    return StoreClip(std::move(samples));
}

ClipHandle AudioEngine::LoadWavFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        lastError_ = "LoadWavFromMemory: empty data";
        return {};
    }
    SDL_IOStream* io = SDL_IOFromConstMem(data, size);
    if (!io) {
        lastError_ = std::string("SDL_IOFromConstMem failed: ") + SDL_GetError();
        return {};
    }
    SDL_AudioSpec srcSpec{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    bool ok = SDL_LoadWAV_IO(io, true, &srcSpec, &buf, &len);
    if (!ok) {
        lastError_ = std::string("SDL_LoadWAV_IO failed: ") + SDL_GetError();
        return {};
    }
    SDL_AudioSpec dstSpec{};
    dstSpec.format = SDL_AUDIO_F32;
    dstSpec.channels = config_.channels;
    dstSpec.freq = config_.sampleRate;
    SDL_AudioStream* conv = SDL_CreateAudioStream(&srcSpec, &dstSpec);
    if (!conv) {
        lastError_ = std::string("SDL_CreateAudioStream failed: ") + SDL_GetError();
        SDL_free(buf);
        return {};
    }
    if (!SDL_PutAudioStreamData(conv, buf, static_cast<int>(len)) || !SDL_FlushAudioStream(conv)) {
        lastError_ = std::string("SDL audio convert failed: ") + SDL_GetError();
        SDL_free(buf);
        SDL_DestroyAudioStream(conv);
        return {};
    }
    SDL_free(buf);
    std::vector<float> samples(4096);
    size_t totalFloats = 0;
    for (;;) {
        if (totalFloats == samples.size())
            samples.resize(samples.size() * 2);
        int want = static_cast<int>((samples.size() - totalFloats) * sizeof(float));
        int got = SDL_GetAudioStreamData(conv, samples.data() + totalFloats, want);
        if (got <= 0)
            break;
        totalFloats += static_cast<size_t>(got) / sizeof(float);
    }
    SDL_DestroyAudioStream(conv);
    samples.resize(totalFloats);
    if (samples.empty()) {
        lastError_ = "memory wav: no audio frames after conversion";
        return {};
    }
    return StoreClip(std::move(samples));
}

ClipHandle AudioEngine::StoreClip(std::vector<float>&& samples) {
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < clips_.size(); ++i) {
        if (!clips_[i].alive) {
            idx = i;
            break;
        }
    }
    if (idx == UINT32_MAX) {
        idx = static_cast<uint32_t>(clips_.size());
        clips_.push_back({});
    }
    clips_[idx].samples = std::move(samples);
    clips_[idx].alive = true;
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    return ClipHandle{ idx };
}

void AudioEngine::UnloadClip(ClipHandle clip) {
    if (!clip.valid())
        return;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    if (clip.index < clips_.size()) {
        clips_[clip.index].alive = false;
        clips_[clip.index].samples.clear();
        clips_[clip.index].samples.shrink_to_fit();
        for (auto& v : voices_) {
            if (v.playing && v.clipIndex == clip.index)
                v.playing = false;
        }
    }
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

bool AudioEngine::ClipValid(ClipHandle clip) const {
    if (!clip.valid() || !mutex_)
        return false;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    bool ok = clip.index < clips_.size() && clips_[clip.index].alive;
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    return ok;
}

VoiceHandle AudioEngine::Play(ClipHandle clip, float volume, bool loop, float pitch) {
    if (!clip.valid() || !mutex_)
        return {};
    Command c{};
    c.type = Command::Type::Play;
    c.clipIndex = clip.index;
    c.volume = std::clamp(volume, 0.0f, 1.0f);
    c.pitch = std::clamp(pitch, 0.1f, 4.0f);
    c.loop = loop;
    c.voiceId = nextVoiceId_++;
    if (nextVoiceId_ == 0)
        nextVoiceId_ = 1;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(c);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    return VoiceHandle{ c.voiceId };
}

void AudioEngine::Stop(VoiceHandle voice) {
    if (!voice.valid() || !mutex_)
        return;
    Command c{};
    c.type = Command::Type::Stop;
    c.voiceId = voice.id;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(c);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

void AudioEngine::StopAll() {
    if (!mutex_)
        return;
    Command c{};
    c.type = Command::Type::StopAll;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(c);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

void AudioEngine::SetVoiceVolume(VoiceHandle voice, float volume) {
    if (!voice.valid() || !mutex_)
        return;
    Command c{};
    c.type = Command::Type::SetVolume;
    c.voiceId = voice.id;
    c.volume = std::clamp(volume, 0.0f, 1.0f);
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(c);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

bool AudioEngine::IsVoicePlaying(VoiceHandle voice) const {
    if (!voice.valid() || !mutex_)
        return false;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    bool playing = false;
    for (const auto& v : voices_) {
        if (v.id == voice.id && v.playing) {
            playing = true;
            break;
        }
    }
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    return playing;
}

void AudioEngine::SetMasterVolume(float v) {
    Command c{};
    c.type = Command::Type::SetMaster;
    c.volume = std::clamp(v, 0.0f, 1.0f);
    if (!mutex_) {
        config_.masterVolume = c.volume;
        return;
    }
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(c);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

void AudioEngine::SetPaused(bool paused) {
    if (!mutex_) {
        paused_ = paused;
        return;
    }
    Command c{};
    c.type = Command::Type::SetPaused;
    c.paused = paused;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(c);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

void AudioEngine::DrainCommands() {
    // Called on the mixer thread with mutex_ held briefly to swap.
    // Voices vector is mixer-owned; clips are read under lock per chunk.
    std::vector<Command> cmds;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    cmds.swap(pending_);
    // Snapshot clip liveness for Play validation.
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    for (const auto& c : cmds) {
        switch (c.type) {
        case Command::Type::Play: {
            SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
            bool clipOk = c.clipIndex < clips_.size() && clips_[c.clipIndex].alive;
            SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
            if (!clipOk)
                break;
            Voice* slot = nullptr;
            for (auto& v : voices_) {
                if (!v.playing) {
                    slot = &v;
                    break;
                }
            }
            if (!slot) {
                if (static_cast<int>(voices_.size()) < config_.maxVoices) {
                    voices_.push_back({});
                    slot = &voices_.back();
                }
            }
            if (slot) {
                slot->id = c.voiceId;
                slot->clipIndex = c.clipIndex;
                slot->cursor = 0.0;
                slot->volume = c.volume;
                slot->pitch = c.pitch;
                slot->loop = c.loop;
                slot->playing = true;
            }
            break;
        }
        case Command::Type::Stop:
            for (auto& v : voices_)
                if (v.id == c.voiceId)
                    v.playing = false;
            break;
        case Command::Type::StopAll:
            for (auto& v : voices_)
                v.playing = false;
            break;
        case Command::Type::SetVolume:
            for (auto& v : voices_)
                if (v.id == c.voiceId)
                    v.volume = c.volume;
            break;
        case Command::Type::SetMaster:
            config_.masterVolume = c.volume;
            break;
        case Command::Type::SetPaused:
            paused_ = c.paused;
            if (paused_) {
                auto* stream = static_cast<SDL_AudioStream*>(deviceStream_);
                if (stream)
                    SDL_PauseAudioStreamDevice(stream);
            } else {
                auto* stream = static_cast<SDL_AudioStream*>(deviceStream_);
                if (stream)
                    SDL_ResumeAudioStreamDevice(stream);
            }
            break;
        }
    }
}

void AudioEngine::MixChunk(float* out, int frames) {
    const int ch = config_.channels;
    std::fill(out, out + static_cast<size_t>(frames) * ch, 0.0f);
    if (paused_)
        return;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    for (auto& v : voices_) {
        if (!v.playing)
            continue;
        if (v.clipIndex >= clips_.size() || !clips_[v.clipIndex].alive) {
            v.playing = false;
            continue;
        }
        const std::vector<float>& s = clips_[v.clipIndex].samples;
        const size_t totalFrames = ch > 0 ? s.size() / static_cast<size_t>(ch) : 0;
        if (totalFrames == 0) {
            v.playing = false;
            continue;
        }
        const float gain = v.volume * config_.masterVolume;
        for (int f = 0; f < frames; ++f) {
            if (v.cursor >= static_cast<double>(totalFrames)) {
                if (v.loop) {
                    v.cursor = std::fmod(v.cursor, static_cast<double>(totalFrames));
                } else {
                    v.playing = false;
                    break;
                }
            }
            size_t i0 = static_cast<size_t>(v.cursor);
            size_t i1 = i0 + 1 < totalFrames ? i0 + 1 : (v.loop ? 0 : i0);
            float frac = static_cast<float>(v.cursor - static_cast<double>(i0));
            for (int c = 0; c < ch; ++c) {
                float a = s[i0 * ch + c];
                float b = s[i1 * ch + c];
                out[f * ch + c] += (a + (b - a) * frac) * gain;
            }
            v.cursor += v.pitch;
        }
    }
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    // Soft-clip to [-1,1].
    const size_t n = static_cast<size_t>(frames) * ch;
    for (size_t i = 0; i < n; ++i)
        out[i] = std::tanh(out[i]);
}

void AudioEngine::MixerLoop() {
    // Pin to the dedicated core first — this is the "dedicated core" guarantee.
    mixerCore_ = PinCurrentThreadToCore(config_.dedicatedCore);
    const int frames = config_.framesPerChunk;
    const int ch = config_.channels;
    std::vector<float> mix(static_cast<size_t>(frames) * ch);
    auto* stream = static_cast<SDL_AudioStream*>(deviceStream_);
    const int targetQueuedMs = 60;
    while (!stopMixer_) {
        DrainCommands();
        MixChunk(mix.data(), frames);
        if (stream && !paused_) {
            int queued = SDL_GetAudioStreamQueued(stream);
            // SDL_GetAudioStreamQueued returns bytes.
            const int bytesPerMs = (config_.sampleRate * ch * 4) / 1000;
            if (queued < bytesPerMs * targetQueuedMs) {
                SDL_PutAudioStreamData(stream, mix.data(),
                                       static_cast<int>(mix.size() * sizeof(float)));
            } else {
                SDL_Delay(2);
            }
        } else {
            SDL_Delay(4);
        }
        // Pace to real time when the device eats exactly what we push:
        // one chunk duration minus a small margin keeps latency low.
        SDL_Delay(static_cast<Uint32>(1000 * frames / config_.sampleRate / 2));
    }
}

void RunMixerLoop(AudioEngine* engine) {
    engine->MixerLoop();
}

} // namespace smvk::audio
