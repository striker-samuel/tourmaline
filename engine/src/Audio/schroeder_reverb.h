// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE SCHROEDER REVERB NODE (docs/audio_graph.md §6) — a custom
// miniaudio `ma_node`: four staggered feedback combs in parallel,
// then two all-pass diffusers in series, per channel, with distinct
// left/right delay tunings (stereo decorrelation) and a one-pole
// high-frequency damper inside each comb's feedback path.
//
// TWO MODES (one implementation):
//   MIX     — the authored-graph inline form: out = (1-wet)*dry +
//             wet*processed (wet = the dry/wet mix fraction; wet 0 is
//             an EXACT bypass — the callback short-circuits to a
//             copy, no processing at all).
//   WET-ONLY — the environmental send form (the world-SFX bus): the
//             node emits ONLY the processed tail (the dry path rides
//             a separate splitter branch), so the send gain never
//             attenuates the dry signal.
//
// THE AUDIO-THREAD CONTRACT (the audio-graph boundary discipline):
//   - ALL heap allocation happens in SchroederReverbInit (the
//     sanctioned initialization burst) and is released in
//     SchroederReverbUninit. The processing callback NEVER allocates,
//     locks, logs or reinitializes anything.
//   - Delay line LENGTHS are fixed for the node's lifetime (scaled
//     from the 44.1 kHz reference tunings); only the coefficients
//     (comb feedback from RT60, the damping pole, the wet mix) are
//     live — and they are SMOOTHED toward their targets inside the
//     callback (a ~30 ms one-pole per parameter), so room changes and
//     boundary crossings cannot zipper.
//   - MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_
//     INPUT: the callback keeps running through input silence so
//     feedback tails dissipate NATURALLY (a stopped source never
//     truncates its reverb; the tail drains while the node stays
//     attached).
//   - Guards: non-finite input samples read as zero; comb filter
//     states zero-snap below 1e-20 (denormal protection); feedback is
//     clamped strictly below unity; rt60 is clamped to [0.15, 8] s.
//
// Feedback derivation (the RT60 law): a comb of delay D seconds
// decays 60 dB after N = 60/(D) round trips... solving |fb|^n = 1e-3
// at t = rt60 gives fb = 10^(-3*D/rt60) — the classic Schroeder/Freeverb
// mapping. Longer rt60 -> feedback nearer unity -> longer tail.

#include "miniaudio_config.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace tour::audio {

// The SchroederReverbNode's configurable shape.
struct SchroederReverbConfig {
    ma_uint32 channels = 2;      // 1 = mono (L tunings), 2 = stereo
    ma_uint32 sampleRate = 48000;
    float rt60 = 1.2f;           // seconds (clamped [0.15, 8])
    float damping = 0.5f;        // 0..1 comb feedback HF damping
    float wet = 1.0f;            // 0..1, the MIX mode's dry/wet mix
    bool wetOnly = false;        // true = the send form (wet-only out)
};

struct SchroederReverbNode {
    ma_node_base base; // MUST be first (the ma_node API contract)

    static constexpr int kCombs = 4;
    static constexpr int kAllpasses = 2;

    struct Channel {
        // Fixed-length delay lines (allocated once at init).
        std::vector<float> comb[kCombs];
        size_t combPos[kCombs] = {};
        float combFilter[kCombs] = {}; // the damping one-pole state
        std::vector<float> allpass[kAllpasses];
        size_t allpassPos[kAllpasses] = {};
        // The comb delay in SECONDS (the RT60->feedback mapping's
        // input; fixed with the line lengths).
        double combSeconds[kCombs] = {};
        // The live feedback coefficient per comb (recomputed from the
        // smoothed rt60 once per processing block).
        float combFeedback[kCombs] = {};
    };

    ma_uint32 channels = 2;
    ma_uint32 sampleRate = 48000;
    bool wetOnly = false;

    Channel chan[2]; // chan[1] unused in mono configurations

    // Live parameters: targets (set from ANY thread — the mixer thread
    // in practice) + the smoothed current values (callback-owned).
    float rt60Target = 1.2f, rt60Cur = 1.2f;
    float dampTarget = 0.5f, dampCur = 0.5f;
    float wetTarget = 1.0f, wetCur = 1.0f;
};

// Initializes the node into `graph` (the one allocation burst).
// Returns false on a bad config (channels not 1/2, rate 0) or a node
// init failure — the caller treats that as the documented graceful
// degradation (direct audio, reverb disabled).
bool SchroederReverbInit(ma_node_graph* graph,
    const SchroederReverbConfig& cfg, SchroederReverbNode* node);

// Tears the node down (detaches + frees the delay lines).
void SchroederReverbUninit(SchroederReverbNode* node);

// Live parameter setters (clamped; targets are smoothed inside the
// processing callback — safe to call at chunk boundaries).
void SchroederReverbSetRt60(SchroederReverbNode* node, float seconds);
void SchroederReverbSetDamping(SchroederReverbNode* node, float zeroToOne);
void SchroederReverbSetWet(SchroederReverbNode* node, float zeroToOne);

} // namespace tour::audio
