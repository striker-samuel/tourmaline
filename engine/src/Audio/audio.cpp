// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Audio/audio.h>

// The ONE miniaudio configuration (feature macros + include; see the
// wrapper's header comment — no TU includes miniaudio.h directly).
#include "miniaudio_config.h"
// The Schroeder reverb custom node (the shared DSP behind the
// authored-graph reverb node + the environmental world bus).
#include "schroeder_reverb.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace tour::audio {

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

// ===========================================================================
// The mixer-thread state (the Core pimpl) — every miniaudio object in
// the process lives below this line. See audio.h for the ownership +
// threading contract: the GAME thread owns the registries + the
// command queue; the MIXER thread owns Core exclusively (commands are
// the only crossing); SDL3 keeps the device; paks keep the content.
// ===========================================================================

// One decoded clip: interleaved float32 at the engine rate/channels
// (SDL decodes). The clip owns the BYTES; every voice owns its own
// ma_audio_buffer_ref cursor over them (never a shared buffer — one
// buffer is one cursor, and the first voice to play a clip exhausts
// it for every later voice).
struct ClipData {
    std::vector<float> samples;
    bool alive = false;
};

// A legacy fire-and-forget voice: one ma_sound over a PER-VOICE
// ma_audio_buffer_ref — a lightweight CURSOR over the clip's decoded
// bytes (never a copy). THE PER-VOICE REF IS LOAD-BEARING (the
// upstream-cursor trap, pinned by audiograph_tests): a shared
// ma_audio_buffer carries ONE cursor, so a second voice wrapping the
// same buffer reads it EXHAUSTED (the first voice played it to the
// end) and plays silence — and two SIMULTANEOUS voices would fight
// over the cursor. The clip owns the bytes; every voice owns its own
// cursor. (The instance lane's per-voice seek-to-0 masked this for
// sequential instances; overlapping same-clip instances still raced.)
struct Voice {
    bool inUse = false;
    uint32_t id = 0;
    uint32_t clipIndex = UINT32_MAX;
    ma_audio_buffer_ref dataSource{}; // the voice's OWN cursor
    bool dataSourceInit = false;
    ma_sound sound{};
};

// The runtime mirror of one AUTHORED node. Only the member matching
// `type` is initialized (a fat struct on purpose — graphs are small;
// one row per authored node keeps the build + teardown trivial).
struct InstanceNode {
    asset::AudioNodeType type = asset::AudioNodeType::Gain;
    bool built = false;
    ma_sound_group group{}; // Gain (a submix bus)
    ma_lpf_node lpf{};
    ma_hpf_node hpf{};
    ma_bpf_node bpf{};
    ma_notch_node notch{};
    ma_peak_node peak{};
    ma_loshelf_node loshelf{};
    ma_hishelf_node hishelf{};
    ma_delay_node delay{};
    // The Schroeder reverb (the custom ma_node — mix mode inline).
    SchroederReverbNode reverb{};
    // A splitter inserted after the node when it fans out to more
    // than one destination (an output bus feeds ONE input; fan-out
    // is a splitter).
    bool hasSplit = false;
    ma_splitter_node split{};
    // The authored filter shape (automation rebuilds configs from
    // these cached values).
    float cutoff = 1000.0f;
    float frequency = 1000.0f;
    float q = 1.0f;
    float gainDb = 0.0f;
    uint32_t order = 2;
};

// One pre-created (source, cue) voice. Created STOPPED (sounds init
// stopped by default) and volume 0; a cue's start event starts it, a
// crossfade's layer weights scale it. `authoredGain` is the source
// node's level (possibly automated through an exposed param bound to
// the source's gain field).
struct InstanceVoice {
    ma_sound sound{};
    bool built = false;
    bool started = false;
    int sourceOrdinal = -1;
    int cueOrdinal = -1;
    uint32_t clipIndex = UINT32_MAX;
    bool loop = false;
    float authoredGain = 1.0f;
    float lastVolume = -1.0f;
    // The voice's OWN cursor over the clip's bytes (see the build
    // site — never the clip's shared buffer).
    ma_audio_buffer_ref dataSource{};
    bool dataSourceInit = false;
    // A splitter when the SOURCE fans out to more than one
    // destination (the sound feeds the splitter, the splitter's
    // outputs feed the destinations).
    bool hasSplit = false;
    ma_splitter_node split{};
};

// ONE live graph instance (mixer-owned). The playback model is the
// AnimatorCrossFade two-layer shape: `currentCue` is the running
// layer, `fadeCue` the crossfade target ramping in over
// fadeDuration; each layer keeps its own cursor, fired-event index
// and automation baseline.
struct GraphInstanceState {
    std::shared_ptr<const asset::AudioGraphDoc> doc;
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
    bool built = false;
    bool playing = false;

    std::vector<InstanceNode> nodes;   // by authored node ordinal
    std::vector<InstanceVoice> voices; // [sourceOrdinal * cueCount + cueOrdinal]
    ma_sound_group bus{};              // the instance exit into the endpoint

    // Authoring joins (precomputed at build)
    std::vector<int> sourceOrdinalOfNode; // node -> source ordinal (-1)
    std::vector<int> nodeOfSource;        // source ordinal -> node ordinal
    std::vector<int> nodeOfParam;         // param ordinal -> node ordinal
    // Per cue: the param-automation track join (param ordinal ->
    // track index in the cue, -1 = the cue does not automate it).
    std::vector<std::vector<int>> trackOfParam;

    // The current layer
    int currentCue = -1;
    double curCursor = 0.0;
    size_t curEventIdx = 0;
    std::vector<float> curBaseline;
    // The crossfade layer (active while fading)
    int fadeCue = -1;
    double fadeCursor = 0.0;
    size_t fadeEventIdx = 0;
    std::vector<float> fadeBaseline;
    float fadeDuration = 0.0f;
    float fadeT = 0.0f;
    bool fading = false;

    // The applied parameter store (authored defaults at build;
    // automation + gameplay set write it) + change detection.
    std::vector<float> paramStore;
    std::vector<float> lastApplied;

    float volume = 1.0f; // the instance bus volume
};

// The create payload crosses the queue (game -> mixer); the mixer
// takes ownership at the drain. The clip table is resolved on the
// CALLER thread (pak reads + WAV decode never touch the mixer).
struct AudioEngine::CreatePayload {
    std::shared_ptr<const asset::AudioGraphDoc> doc;
    std::vector<ClipHandle> clips; // by authored NODE ordinal (sources only)
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
};

struct AudioEngine::Core {
    ma_engine engine{};
    bool engineInit = false;
    std::deque<ClipData> clips; // stable element addresses
    std::vector<Voice> voices;  // legacy fire-and-forget lane
    // Instance states by slot index (null/absent when never built or
    // torn down; validated by generation at every use).
    std::vector<GraphInstanceState*> instances;

    // ---- THE ENVIRONMENTAL WORLD-SFX BUS (docs/audio_graph.md) ------
    // spatial scene voices -> worldSfxGroup -> splitter -> {dry ->
    // endpoint, wet-only Schroeder -> wetGain -> endpoint}. The SEND
    // is listener-driven (SetEnvironmentReverb); the dry path is
    // NEVER attenuated by the send. A failed build leaves worldBusOk
    // false — Play(WorldSfx) degrades to a direct endpoint attach
    // (logged once), everything else keeps working.
    ma_sound_group worldSfxGroup{};
    ma_splitter_node worldSplit{};
    SchroederReverbNode worldReverb{};
    ma_sound_group wetGain{};
    bool worldBusBuilt = false; // the groups' init flags
    bool worldSfxGroupInit = false;
    bool worldSplitInit = false;
    bool worldReverbInit = false;
    bool wetGainInit = false;

    // Builds the bus (the ONE allocation burst at engine init).
    // Transactional: a failure uninits every partial piece.
    bool BuildWorldBus();
    // Tears the bus down (Shutdown order: before the engine uninit).
    void TeardownWorldBus();
    // Applies an environmental target (mixer thread, chunk boundary).
    void ApplyEnvironment(float wet, float rt60, float damping);

    // The instance engine (all mixer-thread-only; see the free
    // functions below for the pure-math halves).
    void UninitInstance(GraphInstanceState* st);
    GraphInstanceState* BuildInstance(AudioEngine::CreatePayload* payload);
    void ApplyInstanceParam(GraphInstanceState& st, uint32_t paramOrdinal, float value);
    void AdvanceInstance(GraphInstanceState& st, double dt);
    void ApplyInstancePlay(GraphInstanceState& st, uint32_t cueOrdinal);
    void ApplyInstanceCrossFade(GraphInstanceState& st, uint32_t cueOrdinal, float fadeSeconds);
    static void ApplyInstanceStop(GraphInstanceState& st);
    void ApplyInstanceSetParam(GraphInstanceState& st, uint32_t paramOrdinal, float value);

    // Tears one legacy voice down (the sound detaches from the graph,
    // the per-voice cursor clears). Mixer thread, chunk boundary.
    void UninitVoice(Voice& v);

    ~Core();
};

void AudioEngine::Core::UninitVoice(Voice& v) {
    if (v.inUse) {
        ma_sound_stop(&v.sound);
        ma_sound_uninit(&v.sound);
    }
    if (v.dataSourceInit) {
        ma_audio_buffer_ref_uninit(&v.dataSource);
        v.dataSourceInit = false;
    }
    v.inUse = false;
}

bool AudioEngine::Core::BuildWorldBus() {
    ma_node_graph* graph = ma_engine_get_node_graph(&engine);
    ma_node* endpoint = ma_engine_get_endpoint(&engine);
    const ma_uint32 channels = ma_engine_get_channels(&engine);
    const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
    // The wet gain: defaults to an endpoint attach (exactly what the
    // wet path wants); its volume is the SEND gain.
    {
        ma_sound_group_config cfg = ma_sound_group_config_init_2(&engine);
        if (ma_sound_group_init_ex(&engine, &cfg, &wetGain) != MA_SUCCESS)
            return false;
        wetGainInit = true;
        ma_sound_group_set_volume(&wetGain, 0.0f);
    }
    // The wet-only Schroeder send.
    {
        SchroederReverbConfig cfg;
        cfg.channels = channels;
        cfg.sampleRate = rate;
        cfg.rt60 = 0.35f; // the outdoor baseline until a room wins
        cfg.damping = 0.5f;
        cfg.wet = 1.0f;
        cfg.wetOnly = true; // the SEND form: dry rides the splitter
        if (!SchroederReverbInit(graph, cfg, &worldReverb))
            return false;
        worldReverbInit = true;
        ma_node_attach_output_bus(&worldReverb.base, 0, (ma_node*)&wetGain, 0);
    }
    // The dry/wet splitter.
    {
        ma_splitter_node_config cfg = ma_splitter_node_config_init(channels);
        cfg.outputBusCount = 2;
        if (ma_splitter_node_init(graph, &cfg, nullptr, &worldSplit) != MA_SUCCESS)
            return false;
        worldSplitInit = true;
        // Bus 0: the untouched dry path. Bus 1: the wet send.
        if (ma_node_attach_output_bus(&worldSplit.base, 0, endpoint, 0) != MA_SUCCESS)
            return false;
        if (ma_node_attach_output_bus(&worldSplit.base, 1, &worldReverb.base, 0) != MA_SUCCESS)
            return false;
    }
    // The shared group every world-SFX voice attaches into.
    {
        ma_sound_group_config cfg = ma_sound_group_config_init_2(&engine);
        cfg.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
        if (ma_sound_group_init_ex(&engine, &cfg, &worldSfxGroup) != MA_SUCCESS)
            return false;
        worldSfxGroupInit = true;
        if (ma_node_attach_output_bus((ma_node*)&worldSfxGroup, 0, &worldSplit.base, 0) !=
            MA_SUCCESS)
            return false;
    }
    worldBusBuilt = true;
    return true;
}

void AudioEngine::Core::TeardownWorldBus() {
    // Detach + uninit in reverse build order (a node's inputs go
    // before the node; nothing may reference a freed buffer when
    // the graph next reads — the mixer thread is parked at
    // Shutdown/teardown time, so plain uninits are safe).
    if (worldSfxGroupInit) {
        ma_node_detach_all_output_buses((ma_node*)&worldSfxGroup);
        ma_sound_group_uninit(&worldSfxGroup);
        worldSfxGroupInit = false;
    }
    if (worldSplitInit) {
        ma_node_detach_all_output_buses(&worldSplit.base);
        ma_splitter_node_uninit(&worldSplit, nullptr);
        worldSplitInit = false;
    }
    if (worldReverbInit) {
        ma_node_detach_all_output_buses(&worldReverb.base);
        SchroederReverbUninit(&worldReverb);
        worldReverbInit = false;
    }
    if (wetGainInit) {
        ma_node_detach_all_output_buses((ma_node*)&wetGain);
        ma_sound_group_uninit(&wetGain);
        wetGainInit = false;
    }
    worldBusBuilt = false;
}

void AudioEngine::Core::ApplyEnvironment(float wet, float rt60, float damping) {
    if (!worldBusBuilt)
        return;
    // The send gain rides the wet group (256-frame smoothed by the
    // engine node); the coefficients smooth inside the reverb's
    // processing callback (~30 ms) — both on the mixer thread.
    ma_sound_group_set_volume(&wetGain, std::clamp(wet, 0.0f, 1.0f));
    SchroederReverbSetRt60(&worldReverb, rt60);
    SchroederReverbSetDamping(&worldReverb, damping);
}

AudioEngine::Core::~Core() {
    // Defensive only — AudioEngine::Shutdown tears everything down in
    // order BEFORE deleting core_ (voices first, then instances, then
    // the engine). A core_ that outlived its engine would be a bug.
    for (GraphInstanceState* st : instances)
        delete st;
    instances.clear();
    for (Voice& v : voices)
        UninitVoice(v);
    voices.clear();
    clips.clear();
    if (engineInit)
        ma_engine_uninit(&engine);
    engineInit = false;
}

// ---- Core helpers (all MIXER-thread-only) ---------------------------------

static ma_node* NodeOf(InstanceNode& n) {
    switch (n.type) {
    case asset::AudioNodeType::Gain:
        return (ma_node*)&n.group;
    case asset::AudioNodeType::LowPass:
        return &n.lpf.baseNode;
    case asset::AudioNodeType::HighPass:
        return &n.hpf.baseNode;
    case asset::AudioNodeType::BandPass:
        return &n.bpf.baseNode;
    case asset::AudioNodeType::Notch:
        return &n.notch.baseNode;
    case asset::AudioNodeType::Peak:
        return &n.peak.baseNode;
    case asset::AudioNodeType::LowShelf:
        return &n.loshelf.baseNode;
    case asset::AudioNodeType::HighShelf:
        return &n.hishelf.baseNode;
    case asset::AudioNodeType::Delay:
        return &n.delay.baseNode;
    case asset::AudioNodeType::Reverb:
        return &n.reverb.base;
    case asset::AudioNodeType::Source:
        return nullptr; // sources are per-(source,cue) voices, not nodes
    }
    return nullptr;
}

static void UninitInstanceNode(InstanceNode& n) {
    if (!n.built)
        return;
    switch (n.type) {
    case asset::AudioNodeType::Gain:
        ma_sound_group_uninit(&n.group);
        break;
    case asset::AudioNodeType::LowPass:
        ma_lpf_node_uninit(&n.lpf, nullptr);
        break;
    case asset::AudioNodeType::HighPass:
        ma_hpf_node_uninit(&n.hpf, nullptr);
        break;
    case asset::AudioNodeType::BandPass:
        ma_bpf_node_uninit(&n.bpf, nullptr);
        break;
    case asset::AudioNodeType::Notch:
        ma_notch_node_uninit(&n.notch, nullptr);
        break;
    case asset::AudioNodeType::Peak:
        ma_peak_node_uninit(&n.peak, nullptr);
        break;
    case asset::AudioNodeType::LowShelf:
        ma_loshelf_node_uninit(&n.loshelf, nullptr);
        break;
    case asset::AudioNodeType::HighShelf:
        ma_hishelf_node_uninit(&n.hishelf, nullptr);
        break;
    case asset::AudioNodeType::Delay:
        ma_delay_node_uninit(&n.delay, nullptr);
        break;
    case asset::AudioNodeType::Reverb:
        SchroederReverbUninit(&n.reverb);
        break;
    case asset::AudioNodeType::Source:
        break;
    }
    if (n.hasSplit) {
        ma_splitter_node_uninit(&n.split, nullptr);
        n.hasSplit = false;
    }
    n.built = false;
}

// Tears a built instance down completely (stop + uninit every voice,
// every node, the bus). Mixer thread, at a chunk boundary.
void AudioEngine::Core::UninitInstance(GraphInstanceState* st) {
    if (!st)
        return;
    if (st->built) {
        for (InstanceVoice& v : st->voices) {
            if (v.built) {
                ma_sound_stop(&v.sound);
                ma_sound_uninit(&v.sound);
                if (v.dataSourceInit) {
                    ma_audio_buffer_ref_uninit(&v.dataSource);
                    v.dataSourceInit = false;
                }
                if (v.hasSplit) {
                    ma_splitter_node_uninit(&v.split, nullptr);
                    v.hasSplit = false;
                }
                v.built = false;
            }
        }
        for (InstanceNode& n : st->nodes)
            UninitInstanceNode(n);
        ma_sound_group_uninit(&st->bus);
    }
    st->built = false;
    st->playing = false;
    delete st;
}

// Builds the whole instance node set from the payload. Returns null
// on a structural failure (all failure modes that matter were
// validated by the parser + the caller-thread clip resolution; ma
// init failures are the remaining, unlikely ones).
GraphInstanceState* AudioEngine::Core::BuildInstance(AudioEngine::CreatePayload* payload) {
    const asset::AudioGraphDoc& doc = *payload->doc;
    const ma_uint32 channels = ma_engine_get_channels(&engine);
    const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
    ma_node_graph* graph = ma_engine_get_node_graph(&engine);

    std::unique_ptr<GraphInstanceState> st(new GraphInstanceState());
    st->doc = payload->doc;
    st->slot = payload->slot;
    st->generation = payload->generation;
    st->nodes.resize(doc.nodes.size());
    st->sourceOrdinalOfNode.assign(doc.nodes.size(), -1);
    st->nodeOfParam.assign(doc.params.size(), -1);
    st->trackOfParam.assign(doc.cues.size(), std::vector<int>(doc.params.size(), -1));

    // The instance bus: the SINGLE exit — authored edges into
    // "output" attach here, the bus attaches to the endpoint. Its
    // volume is the instance-level gain (setVolume).
    {
        ma_sound_group_config cfg = ma_sound_group_config_init_2(&engine);
        cfg.pInitialAttachment = ma_engine_get_endpoint(&engine);
        if (ma_sound_group_init_ex(&engine, &cfg, &st->bus) != MA_SUCCESS) {
            std::cerr << "[Audio] instance create: bus init failed\n";
            return nullptr;
        }
    }

    // Pass 1: create the node objects (unattached; wiring follows so
    // edge targets always exist).
    int sourceCount = 0;
    for (size_t i = 0; i < doc.nodes.size(); ++i) {
        const asset::AudioGraphNode& a = doc.nodes[i];
        InstanceNode& n = st->nodes[i];
        n.type = a.type;
        n.cutoff = a.cutoff;
        n.frequency = a.frequency;
        n.q = a.q;
        n.gainDb = a.gainDb;
        n.order = a.order;
        switch (a.type) {
        case asset::AudioNodeType::Gain: {
            // Authored gain nodes are submix buses, created detached
            // (NO_DEFAULT_ATTACHMENT) and wired like any other node.
            ma_sound_group_config cfg = ma_sound_group_config_init_2(&engine);
            cfg.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
            if (ma_sound_group_init_ex(&engine, &cfg, &n.group) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: gain node '" << a.name << "' init failed\n";
                return nullptr;
            }
            ma_sound_group_set_volume(&n.group, a.gain);
            n.built = true;
            break;
        }
        case asset::AudioNodeType::LowPass: {
            ma_lpf_node_config cfg = ma_lpf_node_config_init(channels, rate, a.cutoff, a.order);
            if (ma_lpf_node_init(graph, &cfg, nullptr, &n.lpf) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: lpf node '" << a.name << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::HighPass: {
            ma_hpf_node_config cfg = ma_hpf_node_config_init(channels, rate, a.cutoff, a.order);
            if (ma_hpf_node_init(graph, &cfg, nullptr, &n.hpf) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: hpf node '" << a.name << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::BandPass: {
            ma_bpf_node_config cfg = ma_bpf_node_config_init(channels, rate, a.cutoff, a.order);
            if (ma_bpf_node_init(graph, &cfg, nullptr, &n.bpf) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: bpf node '" << a.name << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::Notch: {
            ma_notch_node_config cfg = ma_notch_node_config_init(channels, rate, a.q, a.frequency);
            if (ma_notch_node_init(graph, &cfg, nullptr, &n.notch) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: notch node '" << a.name << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::Peak: {
            ma_peak_node_config cfg =
                ma_peak_node_config_init(channels, rate, a.gainDb, a.q, a.frequency);
            if (ma_peak_node_init(graph, &cfg, nullptr, &n.peak) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: peak node '" << a.name << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::LowShelf: {
            ma_loshelf_node_config cfg =
                ma_loshelf_node_config_init(channels, rate, a.gainDb, a.q, a.frequency);
            if (ma_loshelf_node_init(graph, &cfg, nullptr, &n.loshelf) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: loshelf node '" << a.name
                          << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::HighShelf: {
            ma_hishelf_node_config cfg =
                ma_hishelf_node_config_init(channels, rate, a.gainDb, a.q, a.frequency);
            if (ma_hishelf_node_init(graph, &cfg, nullptr, &n.hishelf) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: hishelf node '" << a.name
                          << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::Delay: {
            ma_uint32 frames = static_cast<ma_uint32>(a.delaySeconds * static_cast<float>(rate));
            if (frames < 1)
                frames = 1;
            ma_delay_node_config cfg = ma_delay_node_config_init(channels, rate, frames, a.decay);
            if (ma_delay_node_init(graph, &cfg, nullptr, &n.delay) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: delay node '" << a.name << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::Reverb: {
            // The INLINE form: dry/wet mixed inside the node by the
            // authored -wet (the send form — the world bus — runs the
            // same DSP wet-only; see Core's environmental lane).
            SchroederReverbConfig cfg;
            cfg.channels = channels;
            cfg.sampleRate = rate;
            cfg.rt60 = a.reverbRt60;
            cfg.damping = a.reverbDamping;
            cfg.wet = a.reverbWet;
            cfg.wetOnly = false;
            if (!SchroederReverbInit(graph, cfg, &n.reverb)) {
                std::cerr << "[Audio] instance create: reverb node '" << a.name
                          << "' init failed\n";
                return nullptr;
            }
            n.built = true;
            break;
        }
        case asset::AudioNodeType::Source: {
            // No node object — the per-(source,cue) voices are created
            // in pass 3. Register the source joins.
            st->sourceOrdinalOfNode[i] = sourceCount;
            st->nodeOfSource.push_back(static_cast<int>(i));
            ++sourceCount;
            break;
        }
        }
    }

    // Pass 2: wire the edges. Node fan-out inserts a splitter (an
    // output bus feeds ONE input).
    std::vector<std::vector<ma_node*>> targets(doc.nodes.size());
    for (const asset::AudioGraphEdge& e : doc.edges) {
        const int from = asset::AudioGraphFindNode(doc, e.from);
        const int to = e.to == asset::kAudioGraphOutput ? -1 : asset::AudioGraphFindNode(doc, e.to);
        ma_node* dst = (to < 0) ? (ma_node*)&st->bus : NodeOf(st->nodes[to]);
        targets[from].push_back(dst);
    }
    for (size_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].type == asset::AudioNodeType::Source)
            continue; // wired per-voice in pass 3
        if (targets[i].empty())
            continue; // the parser guarantees reachability
        InstanceNode& n = st->nodes[i];
        ma_node* self = NodeOf(n);
        if (targets[i].size() == 1) {
            ma_node_attach_output_bus(self, 0, targets[i][0], 0);
        } else {
            ma_splitter_node_config cfg = ma_splitter_node_config_init(channels);
            cfg.outputBusCount = static_cast<ma_uint32>(targets[i].size());
            if (ma_splitter_node_init(graph, &cfg, nullptr, &n.split) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: splitter for '" << doc.nodes[i].name
                          << "' init failed\n";
                return nullptr;
            }
            n.hasSplit = true;
            ma_node_attach_output_bus(self, 0, &n.split.base, 0);
            for (size_t k = 0; k < targets[i].size(); ++k)
                ma_node_attach_output_bus(
                    &n.split.base, static_cast<ma_uint32>(k), targets[i][k], 0);
        }
    }

    // Pass 3: the per-(source, cue) voices — pre-created, stopped,
    // volume 0 (the sanctioned allocation burst; cue starts are pure
    // seek/start/volume ops afterwards).
    const size_t cueCount = doc.cues.size();
    st->voices.resize(sourceCount * cueCount);
    // The per-cue param-track join.
    for (size_t ci = 0; ci < doc.cues.size(); ++ci) {
        const asset::AudioCue& cue = doc.cues[ci];
        for (size_t ti = 0; ti < cue.tracks.size(); ++ti) {
            const int pi = asset::AudioGraphFindParam(doc, cue.tracks[ti].param);
            if (pi >= 0)
                st->trackOfParam[ci][pi] = static_cast<int>(ti);
        }
    }
    // The param -> node join.
    for (size_t pi = 0; pi < doc.params.size(); ++pi)
        st->nodeOfParam[pi] = asset::AudioGraphFindNode(doc, doc.params[pi].node);

    for (int s = 0; s < sourceCount; ++s) {
        const int nodeOrdinal = st->nodeOfSource[s];
        const asset::AudioGraphNode& a = doc.nodes[nodeOrdinal];
        const ClipHandle clip = payload->clips[nodeOrdinal];
        if (!clip.valid()) {
            std::cerr << "[Audio] instance create: source '" << a.name
                      << "' has no resolved clip (the create was validated "
                         "caller-side — a pak or clip went away?)\n";
            return nullptr;
        }
        const std::vector<ma_node*>& dsts = targets[nodeOrdinal];
        for (size_t ci = 0; ci < cueCount; ++ci) {
            InstanceVoice& v = st->voices[s * cueCount + ci];
            v.sourceOrdinal = s;
            v.cueOrdinal = static_cast<int>(ci);
            v.clipIndex = clip.index;
            v.loop = a.loop;
            v.authoredGain = a.gain;
            // The PER-VOICE CURSOR (the legacy lane's trap applies here
            // too: one shared buffer = one cursor — two instances of
            // the same graph, or two cues firing the same source,
            // would fight over it. Each voice wraps the clip's bytes
            // in its OWN ref).
            {
                const ClipData& cd = clips[clip.index];
                const ma_uint64 frames =
                    channels > 0 ? cd.samples.size() / static_cast<ma_uint64>(channels) : 0;
                if (frames == 0 ||
                    ma_audio_buffer_ref_init(
                        ma_format_f32, channels, cd.samples.data(), frames, &v.dataSource) !=
                        MA_SUCCESS) {
                    std::cerr << "[Audio] instance create: voice"
                                 " cursor for '"
                              << a.name << "' failed\n";
                    return nullptr;
                }
                v.dataSourceInit = true;
            }
            ma_sound_config cfg = ma_sound_config_init_2(&engine);
            cfg.pDataSource = (ma_data_source*)&v.dataSource;
            cfg.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
            if (dsts.size() == 1) {
                cfg.pInitialAttachment = dsts[0];
            } else {
                // Fan-out: the sound feeds a splitter, the splitter's
                // outputs feed the destinations.
                cfg.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
                ma_splitter_node_config sc = ma_splitter_node_config_init(channels);
                sc.outputBusCount = static_cast<ma_uint32>(dsts.size());
                if (ma_splitter_node_init(graph, &sc, nullptr, &v.split) != MA_SUCCESS) {
                    std::cerr << "[Audio] instance create: voice "
                                 "splitter for '"
                              << a.name << "' init failed\n";
                    return nullptr;
                }
                v.hasSplit = true;
            }
            if (ma_sound_init_ex(&engine, &cfg, &v.sound) != MA_SUCCESS) {
                std::cerr << "[Audio] instance create: voice for '" << a.name << "' init failed\n";
                return nullptr;
            }
            v.built = true;
            ma_sound_set_looping(&v.sound, a.loop ? MA_TRUE : MA_FALSE);
            ma_sound_set_volume(&v.sound, 0.0f); // silent until started
            if (v.hasSplit) {
                for (size_t k = 0; k < dsts.size(); ++k)
                    ma_node_attach_output_bus(&v.split.base, static_cast<ma_uint32>(k), dsts[k], 0);
            }
        }
    }

    // The parameter store starts at the authored field values.
    st->paramStore.resize(doc.params.size(), 0.0f);
    st->lastApplied.resize(doc.params.size(), 0.0f);
    st->curBaseline.resize(doc.params.size(), 0.0f);
    st->fadeBaseline.resize(doc.params.size(), 0.0f);
    for (size_t pi = 0; pi < doc.params.size(); ++pi) {
        const int ni = st->nodeOfParam[pi];
        float value = 0.0f;
        if (ni >= 0) {
            const asset::AudioGraphNode& a = doc.nodes[ni];
            switch (doc.params[pi].field) {
            case asset::AudioParamField::Gain:
                value = a.gain;
                break;
            case asset::AudioParamField::Cutoff:
                value = a.cutoff;
                break;
            case asset::AudioParamField::Frequency:
                value = a.frequency;
                break;
            case asset::AudioParamField::Q:
                value = a.q;
                break;
            case asset::AudioParamField::GainDb:
                value = a.gainDb;
                break;
            case asset::AudioParamField::Decay:
                value = a.decay;
                break;
            case asset::AudioParamField::Wet:
                value = a.reverbWet;
                break;
            case asset::AudioParamField::Rt60:
                value = a.reverbRt60;
                break;
            case asset::AudioParamField::Damping:
                value = a.reverbDamping;
                break;
            }
        }
        st->paramStore[pi] = value;
        st->lastApplied[pi] = value;
        st->curBaseline[pi] = value;
        st->fadeBaseline[pi] = value;
    }

    st->built = true;
    return st.release();
}

// Applies one parameter value to its bound node (mixer thread). The
// biquad reinit rebuilds the filter coefficients (miniaudio resets
// the biquad's internal state on reinit — coarse cutoff automation
// can click slightly; a smoothed param node is the documented
// follow-up).
void AudioEngine::Core::ApplyInstanceParam(GraphInstanceState& st,
                                           uint32_t paramOrdinal,
                                           float value) {
    const asset::AudioGraphDoc& doc = *st.doc;
    const asset::AudioGraphParam& p = doc.params[paramOrdinal];
    const int ni = st.nodeOfParam[paramOrdinal];
    if (ni < 0)
        return;
    InstanceNode& n = st.nodes[ni];
    const ma_uint32 channels = ma_engine_get_channels(&engine);
    const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
    switch (p.field) {
    case asset::AudioParamField::Gain:
        if (n.type == asset::AudioNodeType::Gain) {
            ma_sound_group_set_volume(&n.group, value);
        } else if (n.type == asset::AudioNodeType::Source) {
            // The source's authored level scales every (source, cue)
            // voice — folded into the per-boundary volume pass.
            const int s = st.sourceOrdinalOfNode[ni];
            if (s >= 0) {
                const size_t cueCount = st.doc->cues.size();
                for (size_t ci = 0; ci < cueCount; ++ci)
                    st.voices[s * cueCount + ci].authoredGain = value;
            }
        }
        break;
    case asset::AudioParamField::Cutoff:
        n.cutoff = value;
        if (n.type == asset::AudioNodeType::LowPass) {
            ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, channels, rate, value, n.order);
            ma_lpf_node_reinit(&cfg, &n.lpf);
        } else if (n.type == asset::AudioNodeType::HighPass) {
            ma_hpf_config cfg = ma_hpf_config_init(ma_format_f32, channels, rate, value, n.order);
            ma_hpf_node_reinit(&cfg, &n.hpf);
        } else if (n.type == asset::AudioNodeType::BandPass) {
            ma_bpf_config cfg = ma_bpf_config_init(ma_format_f32, channels, rate, value, n.order);
            ma_bpf_node_reinit(&cfg, &n.bpf);
        }
        break;
    case asset::AudioParamField::Frequency:
        n.frequency = value;
        if (n.type == asset::AudioNodeType::Notch) {
            ma_notch_config cfg = ma_notch2_config_init(ma_format_f32, channels, rate, n.q, value);
            ma_notch_node_reinit(&cfg, &n.notch);
        } else if (n.type == asset::AudioNodeType::Peak) {
            ma_peak_config cfg =
                ma_peak2_config_init(ma_format_f32, channels, rate, n.gainDb, n.q, value);
            ma_peak_node_reinit(&cfg, &n.peak);
        } else if (n.type == asset::AudioNodeType::LowShelf) {
            ma_loshelf_config cfg =
                ma_loshelf2_config_init(ma_format_f32, channels, rate, n.gainDb, n.q, value);
            ma_loshelf_node_reinit(&cfg, &n.loshelf);
        } else if (n.type == asset::AudioNodeType::HighShelf) {
            ma_hishelf_config cfg =
                ma_hishelf2_config_init(ma_format_f32, channels, rate, n.gainDb, n.q, value);
            ma_hishelf_node_reinit(&cfg, &n.hishelf);
        }
        break;
    case asset::AudioParamField::Q:
        n.q = value;
        if (n.type == asset::AudioNodeType::Notch) {
            ma_notch_config cfg =
                ma_notch2_config_init(ma_format_f32, channels, rate, value, n.frequency);
            ma_notch_node_reinit(&cfg, &n.notch);
        } else if (n.type == asset::AudioNodeType::Peak) {
            ma_peak_config cfg =
                ma_peak2_config_init(ma_format_f32, channels, rate, n.gainDb, value, n.frequency);
            ma_peak_node_reinit(&cfg, &n.peak);
        } else if (n.type == asset::AudioNodeType::LowShelf) {
            ma_loshelf_config cfg = ma_loshelf2_config_init(
                ma_format_f32, channels, rate, n.gainDb, value, n.frequency);
            ma_loshelf_node_reinit(&cfg, &n.loshelf);
        } else if (n.type == asset::AudioNodeType::HighShelf) {
            ma_hishelf_config cfg = ma_hishelf2_config_init(
                ma_format_f32, channels, rate, n.gainDb, value, n.frequency);
            ma_hishelf_node_reinit(&cfg, &n.hishelf);
        }
        break;
    case asset::AudioParamField::GainDb:
        n.gainDb = value;
        if (n.type == asset::AudioNodeType::Peak) {
            ma_peak_config cfg =
                ma_peak2_config_init(ma_format_f32, channels, rate, value, n.q, n.frequency);
            ma_peak_node_reinit(&cfg, &n.peak);
        } else if (n.type == asset::AudioNodeType::LowShelf) {
            ma_loshelf_config cfg =
                ma_loshelf2_config_init(ma_format_f32, channels, rate, value, n.q, n.frequency);
            ma_loshelf_node_reinit(&cfg, &n.loshelf);
        } else if (n.type == asset::AudioNodeType::HighShelf) {
            ma_hishelf_config cfg =
                ma_hishelf2_config_init(ma_format_f32, channels, rate, value, n.q, n.frequency);
            ma_hishelf_node_reinit(&cfg, &n.hishelf);
        }
        break;
    case asset::AudioParamField::Decay:
        if (n.type == asset::AudioNodeType::Delay)
            ma_delay_node_set_decay(&n.delay, value);
        break;
    case asset::AudioParamField::Wet:
        if (n.type == asset::AudioNodeType::Reverb)
            SchroederReverbSetWet(&n.reverb, value);
        break;
    case asset::AudioParamField::Rt60:
        if (n.type == asset::AudioNodeType::Reverb)
            SchroederReverbSetRt60(&n.reverb, value);
        break;
    case asset::AudioParamField::Damping:
        if (n.type == asset::AudioNodeType::Reverb)
            SchroederReverbSetDamping(&n.reverb, value);
        break;
    }
}

// One automation keyframe track evaluated at time t. Before the first
// key the track outputs the layer's BASELINE (captured at cue entry);
// a step key holds its value, a linear key ramps to the NEXT key.
// PAST the last key the track RELEASES the param to manual control
// (cues sustain indefinitely — a forever-hold would starve gameplay
// set; the last automated value simply stays applied).
static bool TrackOpinion(const std::vector<asset::AudioCueKeyframe>& keys,
                         double t,
                         float baseline,
                         float& out) {
    if (keys.empty())
        return false;
    if (t > keys.back().t)
        return false; // released
    if (t < keys.front().t) {
        out = baseline;
        return true;
    }
    if (t >= keys.back().t) {
        out = keys.back().value;
        return true;
    }
    // Find the bracketing segment (few keys — a linear scan).
    for (size_t k = 0; k + 1 < keys.size(); ++k) {
        if (t >= keys[k].t && t < keys[k + 1].t) {
            if (!keys[k].linear) {
                out = keys[k].value;
                return true;
            }
            const double span = keys[k + 1].t - keys[k].t;
            if (span <= 0.0) {
                out = keys[k + 1].value;
                return true;
            }
            const double f = (t - keys[k].t) / span;
            out = keys[k].value + static_cast<float>(f) * (keys[k + 1].value - keys[k].value);
            return true;
        }
    }
    out = keys.back().value;
    return true;
}

// Stops every voice a cue layer started (mixer thread).
static void StopLayerVoices(GraphInstanceState& st, int cueOrdinal) {
    if (cueOrdinal < 0 || st.doc->cues.empty())
        return;
    const size_t cueCount = st.doc->cues.size();
    for (size_t s = 0; s < st.nodeOfSource.size(); ++s) {
        InstanceVoice& v = st.voices[s * cueCount + cueOrdinal];
        if (v.built && v.started) {
            ma_sound_stop(&v.sound);
            v.started = false;
            v.lastVolume = -1.0f;
        }
    }
}

// The crossfade layer weight of one cue's voices: 1 for the sole
// running cue; (1-w)/(w) for the current/fade cues during a fade.
static float LayerWeightOf(const GraphInstanceState& st, int cueOrdinal) {
    if (!st.fading)
        return cueOrdinal == st.currentCue ? 1.0f : 0.0f;
    const float w = std::clamp(st.fadeT / st.fadeDuration, 0.0f, 1.0f);
    if (cueOrdinal == st.currentCue)
        return 1.0f - w;
    if (cueOrdinal == st.fadeCue)
        return w;
    return 0.0f;
}

// Advances ONE cue layer: fires the source events whose time the
// cursor crosses (edge-triggered through eventIdx) and returns.
static void AdvanceLayer(GraphInstanceState& st,
                         int cueOrdinal,
                         double& cursor,
                         size_t& eventIdx,
                         double dt) {
    if (cueOrdinal < 0)
        return;
    const asset::AudioCue& cue = st.doc->cues[cueOrdinal];
    const double newT = cursor + dt;
    const size_t cueCount = st.doc->cues.size();
    while (eventIdx < cue.events.size() && cue.events[eventIdx].t <= newT) {
        const asset::AudioCueEvent& ev = cue.events[eventIdx];
        const int nodeOrdinal = asset::AudioGraphFindNode(*st.doc, ev.node);
        const int s = nodeOrdinal >= 0 ? st.sourceOrdinalOfNode[nodeOrdinal] : -1;
        if (s >= 0) {
            InstanceVoice& v = st.voices[s * cueCount + cueOrdinal];
            if (v.built) {
                if (ev.type == asset::AudioCueEvent::Type::StartSource) {
                    ma_sound_seek_to_pcm_frame(&v.sound, 0);
                    ma_sound_start(&v.sound);
                    v.started = true;
                    v.lastVolume = -1.0f; // force the volume pass
                } else {
                    ma_sound_stop(&v.sound);
                    v.started = false;
                    v.lastVolume = -1.0f;
                }
            }
        }
        ++eventIdx;
    }
    cursor = newT;
}

// The per-boundary instance tick: advance both layers, evaluate +
// apply automation, refresh the voice volumes (authored gain x layer
// weight), complete fades. Mixer thread; dt = the chunk duration.
void AudioEngine::Core::AdvanceInstance(GraphInstanceState& st, double dt) {
    if (!st.built || !st.playing)
        return;
    AdvanceLayer(st, st.currentCue, st.curCursor, st.curEventIdx, dt);
    if (st.fading) {
        AdvanceLayer(st, st.fadeCue, st.fadeCursor, st.fadeEventIdx, dt);
        st.fadeT += static_cast<float>(dt);
    }

    // Parameter evaluation: the active cues' tracks, blended by the
    // fade weight when both automate the same param. A track OPINION
    // spans cue entry (the baseline hold) through its LAST keyframe —
    // past that the param releases to manual control.
    const size_t paramCount = st.doc->params.size();
    for (size_t pi = 0; pi < paramCount; ++pi) {
        const asset::AudioCue* curCue = st.currentCue >= 0 ? &st.doc->cues[st.currentCue] : nullptr;
        const asset::AudioCue* fadeCue =
            st.fading && st.fadeCue >= 0 ? &st.doc->cues[st.fadeCue] : nullptr;
        const int tiCur = curCue ? st.trackOfParam[st.currentCue][pi] : -1;
        const int tiFade = fadeCue ? st.trackOfParam[st.fadeCue][pi] : -1;
        float vCur = 0.0f, vFade = 0.0f;
        const bool hasCur =
            tiCur >= 0 &&
            TrackOpinion(curCue->tracks[tiCur].keys, st.curCursor, st.curBaseline[pi], vCur);
        const bool hasFade =
            tiFade >= 0 &&
            TrackOpinion(fadeCue->tracks[tiFade].keys, st.fadeCursor, st.fadeBaseline[pi], vFade);
        if (!hasCur && !hasFade)
            continue; // no active opinion — the store holds
        float value;
        if (hasCur && hasFade) {
            const float w = std::clamp(st.fadeT / st.fadeDuration, 0.0f, 1.0f);
            value = vCur + (vFade - vCur) * w;
        } else if (hasCur) {
            value = vCur;
        } else {
            value = vFade;
        }
        if (value != st.lastApplied[pi]) {
            st.paramStore[pi] = value;
            st.lastApplied[pi] = value;
            ApplyInstanceParam(st, static_cast<uint32_t>(pi), value);
        }
    }

    // Voice volumes: authored (possibly automated) gain x the layer
    // weight; change-detected. Naturally-ended non-loop voices retire
    // their started flag here too.
    const size_t cueCount = st.doc->cues.size();
    for (size_t vi = 0; vi < st.voices.size(); ++vi) {
        InstanceVoice& v = st.voices[vi];
        if (!v.built || !v.started)
            continue;
        if (!ma_sound_is_playing(&v.sound)) {
            // A non-loop source that ran to its end.
            v.started = false;
            v.lastVolume = -1.0f;
            continue;
        }
        const float target = v.authoredGain * LayerWeightOf(st, v.cueOrdinal);
        if (target != v.lastVolume) {
            ma_sound_set_volume(&v.sound, target);
            v.lastVolume = target;
        }
    }

    // Fade completion: the fade layer becomes current with its own
    // cursor/event/baseline state; the old layer's voices stop.
    if (st.fading && st.fadeT >= st.fadeDuration) {
        StopLayerVoices(st, st.currentCue);
        st.currentCue = st.fadeCue;
        st.curCursor = st.fadeCursor;
        st.curEventIdx = st.fadeEventIdx;
        st.curBaseline = st.fadeBaseline;
        st.fadeCue = -1;
        st.fading = false;
    }
}

// ---- instance control ops (drain-time, mixer thread) ----------------------

void AudioEngine::Core::ApplyInstancePlay(GraphInstanceState& st, uint32_t cueOrdinal) {
    if (!st.built || cueOrdinal >= st.doc->cues.size())
        return;
    // Snap semantics: any running fade completes instantly (the old
    // layer's voices stop, the fade layer wins) — then the named cue
    // (re)starts from zero.
    if (st.fading) {
        StopLayerVoices(st, st.currentCue);
        st.currentCue = st.fadeCue;
        st.curCursor = st.fadeCursor;
        st.curEventIdx = st.fadeEventIdx;
        st.curBaseline = st.fadeBaseline;
        st.fadeCue = -1;
        st.fading = false;
    }
    StopLayerVoices(st, st.currentCue);
    st.currentCue = static_cast<int>(cueOrdinal);
    st.curCursor = 0.0;
    st.curEventIdx = 0;
    // The entering layer's automation baseline = the current store
    // (unautomated params hold their applied values through the cue).
    st.curBaseline = st.paramStore;
    st.playing = true;
}

void AudioEngine::Core::ApplyInstanceCrossFade(GraphInstanceState& st,
                                               uint32_t cueOrdinal,
                                               float fadeSeconds) {
    if (!st.built || cueOrdinal >= st.doc->cues.size())
        return;
    if (fadeSeconds <= 0.0f || st.currentCue < 0 || !st.playing) {
        ApplyInstancePlay(st, cueOrdinal);
        return;
    }
    // A stacked fade snaps the previous one first (documented v1).
    if (st.fading) {
        StopLayerVoices(st, st.currentCue);
        st.currentCue = st.fadeCue;
        st.curCursor = st.fadeCursor;
        st.curEventIdx = st.fadeEventIdx;
        st.curBaseline = st.fadeBaseline;
    }
    st.fadeCue = static_cast<int>(cueOrdinal);
    st.fadeCursor = 0.0;
    st.fadeEventIdx = 0;
    st.fadeDuration = std::max(fadeSeconds, 0.001f);
    st.fadeT = 0.0f;
    st.fading = true;
    st.playing = true;
    // The entering layer's baseline = the current store (before its
    // own automation begins, it holds what was sounding).
    st.fadeBaseline = st.paramStore;
}

void AudioEngine::Core::ApplyInstanceStop(GraphInstanceState& st) {
    StopLayerVoices(st, st.currentCue);
    StopLayerVoices(st, st.fadeCue);
    st.currentCue = -1;
    st.fadeCue = -1;
    st.fading = false;
    st.playing = false;
}

void AudioEngine::Core::ApplyInstanceSetParam(GraphInstanceState& st,
                                              uint32_t paramOrdinal,
                                              float value) {
    if (!st.built || paramOrdinal >= st.doc->params.size())
        return;
    st.paramStore[paramOrdinal] = value;
    st.lastApplied[paramOrdinal] = value;
    // The active layers' baselines follow: a pre-first-key hold
    // reflects the new value (automation that already began overrides
    // at the next boundary).
    st.curBaseline[paramOrdinal] = value;
    st.fadeBaseline[paramOrdinal] = value;
    ApplyInstanceParam(st, paramOrdinal, value);
}

// Tears one legacy voice down COMPLETELY (the sound first — it lives
// inside the carrier's bus — then the carrier group itself). Mixer
// thread, at a chunk boundary. (A Core member — the type is private.)
// ===========================================================================
// AudioEngine — game-thread entry points
// ===========================================================================

static int SDLCALL MixerThreadEntry(void* data) {
    auto* self = static_cast<AudioEngine*>(data);
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

    core_ = new Core();

    // The miniaudio engine ALWAYS runs device-less (SDL3 keeps the
    // device; the mixer thread renders + pushes). One process, one
    // ma_engine: every voice, bus and effect node lives in its graph.
    ma_engine_config maCfg = ma_engine_config_init();
    maCfg.channels = static_cast<ma_uint32>(config_.channels);
    maCfg.sampleRate = static_cast<ma_uint32>(config_.sampleRate);
    maCfg.periodSizeInFrames = static_cast<ma_uint32>(config_.framesPerChunk);
    maCfg.noDevice = MA_TRUE;
    maCfg.listenerCount = 1;
    // Volume smoothing for every sound/group (the per-boundary
    // automation + crossfade steps land zipper-free).
    maCfg.defaultVolumeSmoothTimeInPCMFrames = 256;
    if (ma_engine_init(&maCfg, &core_->engine) != MA_SUCCESS) {
        lastError_ = "ma_engine_init failed";
        delete core_;
        core_ = nullptr;
        SDL_DestroyMutex(static_cast<SDL_Mutex*>(mutex_));
        mutex_ = nullptr;
        return false;
    }
    core_->engineInit = true;
    ma_engine_set_volume(&core_->engine, config_.masterVolume);
    // The environmental world-SFX bus (the shared listener-driven
    // reverb send). A failure is NEVER fatal: audio keeps working,
    // Play(WorldSfx) degrades to a direct endpoint attach.
    if (!core_->BuildWorldBus())
        std::cerr << "[Audio] environmental bus init failed -"
                     " world-SFX voices fall back to direct (no reverb)\n";

    if (config_.deviceless) {
        initialized_ = true;
        return true;
    }

    if (!OpenDeviceStream()) {
        Shutdown();
        return false;
    }

    stopMixer_ = false;
    mixerCore_ = config_.dedicatedCore;
    mixerThread_ = SDL_CreateThread(MixerThreadEntry, "AudioMixer", this);
    if (!mixerThread_) {
        lastError_ = std::string("SDL_CreateThread failed: ") + SDL_GetError();
        Shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void AudioEngine::Shutdown() {
    if (mixerThread_) {
        stopMixer_ = true;
        SDL_WaitThread(static_cast<SDL_Thread*>(mixerThread_), nullptr);
        mixerThread_ = nullptr;
    }
    // Single-threaded from here: tear the miniaudio state down in
    // dependency order (voices + instances first — they reference
    // clips — then the clip buffers, then the engine).
    if (core_) {
        for (GraphInstanceState* st : core_->instances) {
            core_->UninitInstance(st); // deletes st
        }
        core_->instances.clear();
        for (Voice& v : core_->voices)
            core_->UninitVoice(v);
        core_->voices.clear();
        // The environmental bus (every dependent voice is detached
        // above; the reverb's delay lines free here).
        core_->TeardownWorldBus();
        core_->clips.clear(); // the decoded bytes (voice cursors gone)
        delete core_;         // uninits the engine last (Core dtor)
        core_ = nullptr;
    }
    if (deviceStream_)
        CloseDeviceStream();
    if (mutex_) {
        SDL_DestroyMutex(static_cast<SDL_Mutex*>(mutex_));
        mutex_ = nullptr;
    }
    clipAlive_.clear();
    liveVoiceIds_.clear();
    instancePlaying_.clear();
    instanceSlots_.clear();
    instanceFree_.clear();
    pending_.clear();
    initialized_ = false;
    stopMixer_ = false;
}

bool AudioEngine::OpenDeviceStream() {
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = config_.channels;
    spec.freq = config_.sampleRate;
    deviceStream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
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
    for (uint32_t i = 0; i < core_->clips.size(); ++i) {
        if (!core_->clips[i].alive) {
            idx = i;
            break;
        }
    }
    if (idx == UINT32_MAX) {
        idx = static_cast<uint32_t>(core_->clips.size());
        core_->clips.emplace_back();
        clipAlive_.push_back(0);
    }
    ClipData& cd = core_->clips[idx];
    cd.samples = std::move(samples);
    // The clip owns the DECODED BYTES only — every voice wraps them in
    // its OWN ma_audio_buffer_ref cursor at play time (the
    // shared-cursor trap: one ma_audio_buffer = one cursor, exhausted
    // by the first voice to play it). No clip-side miniaudio object.
    cd.alive = true;
    clipAlive_[idx] = 1;
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    return ClipHandle{idx};
}

void AudioEngine::UnloadClip(ClipHandle clip) {
    if (!clip.valid() || !mutex_)
        return;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    if (clip.index < core_->clips.size() && core_->clips[clip.index].alive) {
        core_->clips[clip.index].alive = false;
        if (clip.index < clipAlive_.size())
            clipAlive_[clip.index] = 0;
    }
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    // The mixer stops + frees dependents and the bytes at the next
    // boundary (no read can touch them in between — reads follow
    // drains inside every mixer iteration).
    Command c{};
    c.type = Command::Type::UnloadClip;
    c.clipIndex = clip.index;
    PushCommand(std::move(c));
}

bool AudioEngine::ClipValid(ClipHandle clip) const {
    if (!clip.valid() || !mutex_)
        return false;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    const bool ok = clip.index < clipAlive_.size() && clipAlive_[clip.index] != 0;
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    return ok;
}

void AudioEngine::PushCommand(Command&& c) {
    if (!mutex_)
        return;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    pending_.push_back(std::move(c));
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
}

VoiceHandle AudioEngine::Play(ClipHandle clip,
                              float volume,
                              bool loop,
                              float pitch,
                              AudioRoute route) {
    if (!clip.valid() || !mutex_)
        return {};
    Command c{};
    c.type = Command::Type::Play;
    c.clipIndex = clip.index;
    c.volume = std::clamp(volume, 0.0f, 1.0f);
    c.pitch = std::clamp(pitch, 0.1f, 4.0f);
    c.loop = loop;
    c.route = route == AudioRoute::WorldSfx ? 1u : 0u;
    c.voiceId = nextVoiceId_++;
    if (nextVoiceId_ == 0)
        nextVoiceId_ = 1;
    PushCommand(std::move(c));
    return VoiceHandle{c.voiceId};
}

void AudioEngine::Stop(VoiceHandle voice) {
    if (!voice.valid())
        return;
    Command c{};
    c.type = Command::Type::Stop;
    c.voiceId = voice.id;
    PushCommand(std::move(c));
}

void AudioEngine::StopAll() {
    Command c{};
    c.type = Command::Type::StopAll;
    PushCommand(std::move(c));
}

void AudioEngine::SetVoiceVolume(VoiceHandle voice, float volume) {
    if (!voice.valid())
        return;
    Command c{};
    c.type = Command::Type::setVolume;
    c.voiceId = voice.id;
    c.volume = std::clamp(volume, 0.0f, 1.0f);
    PushCommand(std::move(c));
}

void AudioEngine::SetVoicePan(VoiceHandle voice, float pan) {
    if (!voice.valid())
        return;
    Command c{};
    c.type = Command::Type::SetPan;
    c.voiceId = voice.id;
    c.pan = std::clamp(pan, -1.0f, 1.0f);
    PushCommand(std::move(c));
}

void AudioEngine::SetVoiceSpatial(VoiceHandle voice, float volume, float pan) {
    if (!voice.valid())
        return;
    Command c{};
    c.type = Command::Type::SetSpatial;
    c.voiceId = voice.id;
    c.volume = std::clamp(volume, 0.0f, 1.0f);
    c.pan = std::clamp(pan, -1.0f, 1.0f);
    PushCommand(std::move(c));
}

void AudioEngine::ConstantPowerPan(float pan, float& left, float& right) {
    // Equal-loudness (constant power) pan: theta sweeps the quadrant
    // 0..pi/2 as pan goes -1..+1; cos/sin keep the summed power at 1
    // (linear pan's -6dB center dip audibly attenuates moving sounds).
    const float t = std::clamp(pan, -1.0f, 1.0f) * 0.5f + 0.5f;
    const float theta = t * (3.14159265358979323846f * 0.5f);
    left = std::cos(theta);
    right = std::sin(theta);
}

bool AudioEngine::IsVoicePlaying(VoiceHandle voice) const {
    if (!voice.valid() || !mutex_)
        return false;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    const bool playing =
        std::find(liveVoiceIds_.begin(), liveVoiceIds_.end(), voice.id) != liveVoiceIds_.end();
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
    PushCommand(std::move(c));
}

void AudioEngine::SetPaused(bool paused) {
    if (!mutex_) {
        paused_ = paused;
        return;
    }
    Command c{};
    c.type = Command::Type::SetPaused;
    c.paused = paused;
    PushCommand(std::move(c));
}

void AudioEngine::SetEnvironmentReverb(const EnvironmentReverb& target) {
    if (!mutex_)
        return; // not initialized: nothing to drive (a logged no-op)
    Command c{};
    c.type = Command::Type::SetEnvironment;
    c.volume = std::clamp(target.wet, 0.0f, 1.0f);
    c.envRt60 = target.rt60;
    c.envDamping = target.damping;
    PushCommand(std::move(c));
}

// ---- instances: game-thread entry points ----------------------------------

bool AudioEngine::ValidateInstance(smvk::AudioInstanceHandle handle) const {
    if (!handle)
        return false;
    const uint32_t idx = handle.Index();
    if (idx >= instanceSlots_.size())
        return false;
    const InstanceSlot& s = instanceSlots_[idx];
    return s.alive && s.generation == handle.Generation();
}

smvk::AudioInstanceHandle AudioEngine::CreateInstance(const asset::AudioGraphDoc& doc,
                                                      AudioClipResolver resolver,
                                                      void* resolverUser,
                                                      std::string* err) {
    const auto fail = [&](const std::string& m) {
        if (err)
            *err = m;
        return smvk::AudioInstanceHandle{};
    };
    if (!initialized_)
        return fail("audio engine not initialized");
    if (doc.nodes.empty() || doc.cues.empty())
        return fail("the graph doc is empty (it must have passed "
                    "ParseAudioGraphText)");

    // Resolve every source's clip HERE — the caller thread. Pak reads
    // + WAV decode never touch the mixer thread; a failed resolution
    // fails the create loudly with the named path.
    std::vector<ClipHandle> clips(doc.nodes.size());
    for (size_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].type != asset::AudioNodeType::Source)
            continue;
        if (!resolver)
            return fail("no clip resolver supplied");
        const ClipHandle h = resolver(resolverUser, doc.nodes[i].sound.c_str());
        bool ok = h.valid();
        if (ok) {
            SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
            ok = h.index < core_->clips.size() && core_->clips[h.index].alive;
            SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
        }
        if (!ok)
            return fail("sound '" + doc.nodes[i].sound + "' (node '" + doc.nodes[i].name +
                        "') failed to resolve");
        clips[i] = h;
    }

    uint32_t slot;
    if (!instanceFree_.empty()) {
        slot = instanceFree_.back();
        instanceFree_.pop_back();
    } else {
        slot = static_cast<uint32_t>(instanceSlots_.size());
        instanceSlots_.push_back({});
    }
    InstanceSlot& s = instanceSlots_[slot];
    if (s.generation == 0)
        s.generation = 1;
    s.alive = true;
    s.doc = std::make_shared<const asset::AudioGraphDoc>(doc);

    auto* payload = new CreatePayload();
    payload->doc = s.doc;
    payload->clips = std::move(clips);
    payload->slot = slot;
    payload->generation = s.generation;

    Command c{};
    c.type = Command::Type::InstanceCreate;
    c.slot = slot;
    c.generation = s.generation;
    c.create = payload;
    PushCommand(std::move(c));
    return smvk::MakeEntityHandle<smvk::AudioInstanceTag>(slot, s.generation);
}

bool AudioEngine::DestroyInstance(smvk::AudioInstanceHandle handle) {
    if (!ValidateInstance(handle))
        return false;
    InstanceSlot& s = instanceSlots_[handle.Index()];
    const uint32_t oldGen = s.generation;
    s.alive = false;
    s.doc.reset();
    s.generation++; // stale handles fail validation forever
    instanceFree_.push_back(handle.Index());
    Command c{};
    c.type = Command::Type::InstanceDestroy;
    c.slot = handle.Index();
    c.generation = oldGen;
    PushCommand(std::move(c));
    return true;
}

bool AudioEngine::IsInstanceAlive(smvk::AudioInstanceHandle handle) const {
    return ValidateInstance(handle);
}

bool AudioEngine::PumpOffline(float* out, int frames) {
    if (!initialized_ || !config_.deviceless || !out || frames <= 0)
        return false;
    DrainCommands();
    if (!paused_) {
        const double dt = static_cast<double>(frames) / static_cast<double>(config_.sampleRate);
        for (GraphInstanceState* st : core_->instances)
            if (st)
                core_->AdvanceInstance(*st, dt);
    }
    // Reap finished voices + publish the game-thread snapshot. The
    // full teardown (sound + its carrier group) rides UninitVoice —
    // the carrier MUST die with the voice (the atEnd shield).
    for (Voice& v : core_->voices) {
        if (v.inUse && !ma_sound_is_playing(&v.sound))
            core_->UninitVoice(v);
    }
    {
        SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
        liveVoiceIds_.clear();
        for (const Voice& v : core_->voices)
            if (v.inUse)
                liveVoiceIds_.push_back(v.id);
        instancePlaying_.assign(core_->instances.size(), 0);
        for (size_t i = 0; i < core_->instances.size(); ++i)
            if (core_->instances[i])
                instancePlaying_[i] = core_->instances[i]->playing ? 1 : 0;
        SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    }
    MixChunk(out, frames);
    return true;
}

// ===========================================================================
// The mixer iteration (drain -> advance -> reap -> publish -> render)
// ===========================================================================

void AudioEngine::DrainCommands() {
    std::vector<Command> cmds;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    cmds.swap(pending_);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));

    for (const auto& c : cmds) {
        switch (c.type) {
        case Command::Type::Play: {
            if (c.clipIndex >= core_->clips.size() || !core_->clips[c.clipIndex].alive)
                break; // the clip died between Play and the drain
            Voice* slot = nullptr;
            for (auto& v : core_->voices) {
                if (!v.inUse) {
                    slot = &v;
                    break;
                }
            }
            if (!slot && static_cast<int>(core_->voices.size()) < config_.maxVoices) {
                core_->voices.emplace_back();
                slot = &core_->voices.back();
            }
            if (!slot)
                break; // voice budget exhausted — drop silently (v1)
            // THE PER-VOICE CURSOR (see Voice's comment): a fresh
            // ma_audio_buffer_ref over the clip's decoded bytes —
            // never the clip's own buffer (whose shared cursor the
            // last voice may have exhausted).
            {
                const ClipData& cd = core_->clips[c.clipIndex];
                const ma_uint64 frames =
                    config_.channels > 0
                        ? cd.samples.size() / static_cast<ma_uint64>(config_.channels)
                        : 0;
                if (frames == 0) {
                    std::cerr << "[Audio] voice over an empty clip"
                                 " (dropped)\n";
                    break;
                }
                if (ma_audio_buffer_ref_init(ma_format_f32,
                                             static_cast<ma_uint32>(config_.channels),
                                             cd.samples.data(),
                                             frames,
                                             &slot->dataSource) != MA_SUCCESS) {
                    std::cerr << "[Audio] voice cursor init failed"
                                 " (dropped)\n";
                    break;
                }
                slot->dataSourceInit = true;
            }
            ma_sound_config cfg = ma_sound_config_init_2(&core_->engine);
            cfg.pDataSource = (ma_data_source*)&slot->dataSource;
            cfg.flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
            // The ROUTE: the shared environmental bus (spatial scene
            // voices) or the endpoint (everything else; a failed bus
            // build degrades WorldSfx to the direct attach).
            if (c.route == 1u && core_->worldBusBuilt)
                cfg.pInitialAttachment = (ma_node*)&core_->worldSfxGroup;
            if (ma_sound_init_ex(&core_->engine, &cfg, &slot->sound) != MA_SUCCESS) {
                std::cerr << "[Audio] voice init failed (dropped)\n";
                core_->UninitVoice(*slot);
                break;
            }
            ma_sound_set_volume(&slot->sound, c.volume);
            ma_sound_set_pan(&slot->sound, c.pan);
            ma_sound_set_pitch(&slot->sound, c.pitch);
            ma_sound_set_looping(&slot->sound, c.loop ? MA_TRUE : MA_FALSE);
            ma_sound_start(&slot->sound);
            slot->inUse = true;
            slot->id = c.voiceId;
            slot->clipIndex = c.clipIndex;
            break;
        }
        case Command::Type::Stop:
            for (auto& v : core_->voices) {
                if (v.inUse && v.id == c.voiceId)
                    core_->UninitVoice(v);
            }
            break;
        case Command::Type::StopAll:
            for (auto& v : core_->voices) {
                if (v.inUse)
                    core_->UninitVoice(v);
            }
            break;
        case Command::Type::setVolume:
            for (auto& v : core_->voices)
                if (v.inUse && v.id == c.voiceId)
                    ma_sound_set_volume(&v.sound, c.volume);
            break;
        case Command::Type::SetPan:
            for (auto& v : core_->voices)
                if (v.inUse && v.id == c.voiceId)
                    ma_sound_set_pan(&v.sound, c.pan);
            break;
        case Command::Type::SetSpatial:
            for (auto& v : core_->voices) {
                if (v.inUse && v.id == c.voiceId) {
                    ma_sound_set_volume(&v.sound, c.volume);
                    ma_sound_set_pan(&v.sound, c.pan);
                }
            }
            break;
        case Command::Type::SetMaster:
            ma_engine_set_volume(&core_->engine, c.volume);
            config_.masterVolume = c.volume;
            break;
        case Command::Type::SetPaused:
            paused_ = c.paused;
            if (deviceStream_) {
                auto* stream = static_cast<SDL_AudioStream*>(deviceStream_);
                if (paused_)
                    SDL_PauseAudioStreamDevice(stream);
                else
                    SDL_ResumeAudioStreamDevice(stream);
            }
            break;
        case Command::Type::UnloadClip: {
            if (c.clipIndex >= core_->clips.size())
                break;
            ClipData& cd = core_->clips[c.clipIndex];
            if (!cd.alive)
                break;
            // Stop + uninit every dependent BEFORE freeing the bytes
            // (reads only happen after drains — nothing can touch the
            // buffer past this point in the iteration).
            for (auto& v : core_->voices) {
                if (v.inUse && v.clipIndex == c.clipIndex)
                    core_->UninitVoice(v);
            }
            for (GraphInstanceState* st : core_->instances) {
                if (!st)
                    continue;
                for (InstanceVoice& v : st->voices) {
                    if (v.built && v.clipIndex == c.clipIndex) {
                        ma_sound_stop(&v.sound);
                        ma_sound_uninit(&v.sound);
                        if (v.dataSourceInit) {
                            ma_audio_buffer_ref_uninit(&v.dataSource);
                            v.dataSourceInit = false;
                        }
                        v.built = false;
                        v.started = false;
                        if (v.hasSplit) {
                            ma_splitter_node_uninit(&v.split, nullptr);
                            v.hasSplit = false;
                        }
                    }
                }
            }
            cd.samples.clear();
            cd.samples.shrink_to_fit();
            break;
        }
        case Command::Type::InstanceCreate: {
            std::unique_ptr<CreatePayload> payload(c.create);
            if (c.slot >= core_->instances.size())
                core_->instances.resize(c.slot + 1, nullptr);
            GraphInstanceState* existing = core_->instances[c.slot];
            if (existing && existing->generation == c.generation) {
                // A same-generation rebuild (should not happen —
                // destroys bump the generation); drop the old state.
                core_->UninitInstance(existing);
                core_->instances[c.slot] = nullptr;
            }
            GraphInstanceState* st = core_->BuildInstance(payload.get());
            if (!st) {
                std::cerr << "[Audio] instance create failed (slot " << c.slot
                          << ") — the slot stays empty\n";
                break;
            }
            core_->instances[c.slot] = st;
            break;
        }
        case Command::Type::InstanceDestroy: {
            if (c.slot >= core_->instances.size())
                break;
            GraphInstanceState* st = core_->instances[c.slot];
            if (!st || st->generation != c.generation)
                break; // stale (a newer create already replaced it)
            core_->UninitInstance(st);
            core_->instances[c.slot] = nullptr;
            break;
        }
        case Command::Type::InstancePlay:
        case Command::Type::InstanceCrossFade:
        case Command::Type::InstanceStop:
        case Command::Type::InstanceSetParam:
        case Command::Type::InstanceSetVolume: {
            if (c.slot >= core_->instances.size())
                break;
            GraphInstanceState* st = core_->instances[c.slot];
            if (!st || st->generation != c.generation)
                break; // stale handle — the destroy won the race
            switch (c.type) {
            case Command::Type::InstancePlay:
                core_->ApplyInstancePlay(*st, c.ordinal);
                break;
            case Command::Type::InstanceCrossFade:
                core_->ApplyInstanceCrossFade(*st, c.ordinal, c.fadeSeconds);
                break;
            case Command::Type::InstanceStop:
                core_->ApplyInstanceStop(*st);
                break;
            case Command::Type::InstanceSetParam:
                core_->ApplyInstanceSetParam(*st, c.ordinal, c.volume);
                break;
            case Command::Type::InstanceSetVolume:
                st->volume = c.volume;
                if (st->built)
                    ma_sound_group_set_volume(&st->bus, c.volume);
                break;
            default:
                break;
            }
            break;
        }
        case Command::Type::SetEnvironment:
            // The environmental send: applied at the boundary, all
            // smoothing mixer-thread-side (the wet group's 256-frame
            // gainer + the reverb's ~30 ms coefficient glide).
            core_->ApplyEnvironment(c.volume, c.envRt60, c.envDamping);
            break;
        }
    }
}

void AudioEngine::MixChunk(float* out, int frames) {
    const int ch = config_.channels;
    const size_t n = static_cast<size_t>(frames) * ch;
    if (paused_ || !core_ || !core_->engineInit) {
        std::fill(out, out + n, 0.0f);
        return;
    }
    // The whole graph — legacy voices + every instance bus — renders
    // through ONE ma_engine read. The lock serializes against the
    // game thread's clip store/unload (the read dereferences clip
    // buffers).
    ma_uint64 read = 0;
    SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
    ma_engine_read_pcm_frames(&core_->engine, out, static_cast<ma_uint64>(frames), &read);
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
    if (read < static_cast<ma_uint64>(frames))
        std::fill(out + read * ch, out + n, 0.0f);
    // Soft-clip to [-1,1] (the summed graph can exceed unity).
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
    const double chunkSeconds =
        static_cast<double>(frames) / static_cast<double>(config_.sampleRate);
    while (!stopMixer_) {
        // ONE mixer iteration: mutations + scheduling at the boundary,
        // then the render.
        DrainCommands();
        if (!paused_) {
            for (GraphInstanceState* st : core_->instances)
                if (st)
                    core_->AdvanceInstance(*st, chunkSeconds);
        }
        // Reap finished one-shot voices (the FULL teardown — the
        // carrier group dies with the voice: the atEnd shield).
        for (Voice& v : core_->voices) {
            if (v.inUse && !ma_sound_is_playing(&v.sound))
                core_->UninitVoice(v);
        }
        // Publish the game-thread visibility snapshot.
        {
            SDL_LockMutex(static_cast<SDL_Mutex*>(mutex_));
            liveVoiceIds_.clear();
            for (const Voice& v : core_->voices)
                if (v.inUse)
                    liveVoiceIds_.push_back(v.id);
            instancePlaying_.assign(core_->instances.size(), 0);
            for (size_t i = 0; i < core_->instances.size(); ++i)
                if (core_->instances[i])
                    instancePlaying_[i] = core_->instances[i]->playing ? 1 : 0;
            SDL_UnlockMutex(static_cast<SDL_Mutex*>(mutex_));
        }
        MixChunk(mix.data(), frames);
        if (stream && !paused_) {
            int queued = SDL_GetAudioStreamQueued(stream);
            // SDL_GetAudioStreamQueued returns bytes.
            const int bytesPerMs = (config_.sampleRate * ch * 4) / 1000;
            if (queued < bytesPerMs * targetQueuedMs) {
                SDL_PutAudioStreamData(
                    stream, mix.data(), static_cast<int>(mix.size() * sizeof(float)));
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

// ===========================================================================
// The instance facade (the tour::Animator parallel)
// ===========================================================================

AudioInstance::operator bool() const {
    return m_engine && m_engine->IsInstanceAlive(m_handle);
}

void AudioInstance::play() {
    playCue(0u); // the authored default cue
}

void AudioInstance::playCue(uint32_t cueOrdinal) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle))
        return;
    if (m_engine->instanceSlots_[m_handle.Index()].doc &&
        cueOrdinal >= m_engine->instanceSlots_[m_handle.Index()].doc->cues.size())
        return;
    AudioEngine::Command c{};
    c.type = AudioEngine::Command::Type::InstancePlay;
    c.slot = m_handle.Index();
    c.generation = m_handle.Generation();
    c.ordinal = cueOrdinal;
    m_engine->PushCommand(std::move(c));
}

void AudioInstance::playCue(const char* cueName) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle) || !cueName)
        return;
    const auto& doc = m_engine->instanceSlots_[m_handle.Index()].doc;
    if (!doc)
        return;
    const int ordinal = asset::AudioGraphFindCue(*doc, cueName);
    if (ordinal < 0)
        return;
    playCue(static_cast<uint32_t>(ordinal));
}

void AudioInstance::crossFade(uint32_t cueOrdinal, float fadeSeconds) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle))
        return;
    const auto& doc = m_engine->instanceSlots_[m_handle.Index()].doc;
    if (doc && cueOrdinal >= doc->cues.size())
        return;
    AudioEngine::Command c{};
    c.type = AudioEngine::Command::Type::InstanceCrossFade;
    c.slot = m_handle.Index();
    c.generation = m_handle.Generation();
    c.ordinal = cueOrdinal;
    c.fadeSeconds = std::max(fadeSeconds, 0.0f);
    m_engine->PushCommand(std::move(c));
}

void AudioInstance::crossFade(const char* cueName, float fadeSeconds) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle) || !cueName)
        return;
    const auto& doc = m_engine->instanceSlots_[m_handle.Index()].doc;
    if (!doc)
        return;
    const int ordinal = asset::AudioGraphFindCue(*doc, cueName);
    if (ordinal < 0)
        return;
    crossFade(static_cast<uint32_t>(ordinal), fadeSeconds);
}

void AudioInstance::stop() {
    if (!m_engine || !m_engine->ValidateInstance(m_handle))
        return;
    AudioEngine::Command c{};
    c.type = AudioEngine::Command::Type::InstanceStop;
    c.slot = m_handle.Index();
    c.generation = m_handle.Generation();
    m_engine->PushCommand(std::move(c));
}

void AudioInstance::set(uint32_t paramOrdinal, float value) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle))
        return;
    const auto& doc = m_engine->instanceSlots_[m_handle.Index()].doc;
    if (doc && paramOrdinal >= doc->params.size())
        return;
    AudioEngine::Command c{};
    c.type = AudioEngine::Command::Type::InstanceSetParam;
    c.slot = m_handle.Index();
    c.generation = m_handle.Generation();
    c.ordinal = paramOrdinal;
    c.volume = value;
    m_engine->PushCommand(std::move(c));
}

void AudioInstance::set(const char* paramName, float value) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle) || !paramName)
        return;
    const auto& doc = m_engine->instanceSlots_[m_handle.Index()].doc;
    if (!doc)
        return;
    const int ordinal = asset::AudioGraphFindParam(*doc, paramName);
    if (ordinal < 0)
        return;
    set(static_cast<uint32_t>(ordinal), value);
}

void AudioInstance::setVolume(float volume) {
    if (!m_engine || !m_engine->ValidateInstance(m_handle))
        return;
    AudioEngine::Command c{};
    c.type = AudioEngine::Command::Type::InstanceSetVolume;
    c.slot = m_handle.Index();
    c.generation = m_handle.Generation();
    c.volume = std::max(volume, 0.0f);
    m_engine->PushCommand(std::move(c));
}

bool AudioInstance::playing() const {
    if (!m_engine || !m_engine->ValidateInstance(m_handle))
        return false;
    SDL_LockMutex(static_cast<SDL_Mutex*>(m_engine->mutex_));
    const uint32_t idx = m_handle.Index();
    const bool playing =
        idx < m_engine->instancePlaying_.size() && m_engine->instancePlaying_[idx] != 0;
    SDL_UnlockMutex(static_cast<SDL_Mutex*>(m_engine->mutex_));
    return playing;
}

} // namespace tour::audio
