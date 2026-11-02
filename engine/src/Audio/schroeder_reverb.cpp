// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "schroeder_reverb.h"

#include <algorithm>
#include <cstring>

namespace tour::audio {

namespace {

// The reference tunings (Freeverb's, at 44.1 kHz): four staggered
// combs + two all-pass diffusers, with a +23-sample right-channel
// offset for stereo decorrelation.
constexpr int kRefRate = 44100;
constexpr int kRefCombs[SchroederReverbNode::kCombs] = { 1116, 1188,
	1277, 1400 };
constexpr int kRefCombsR[SchroederReverbNode::kCombs] = { 1139, 1211,
	1300, 1423 };
constexpr int kRefAllpass[SchroederReverbNode::kAllpasses] = { 556,
	441 };
constexpr int kRefAllpassR[SchroederReverbNode::kAllpasses] = { 579,
	464 };
constexpr float kAllpassGain = 0.5f; // Schroeder's diffuser coefficient

constexpr float kMinRt60 = 0.15f;
constexpr float kMaxRt60 = 8.0f;

// The parameter smoothing time constant (~30 ms): a boundary
// crossing or room change glides, never steps.
constexpr double kSmoothSeconds = 0.030;

float ClampRt60(float v) {
    if (!std::isfinite(v))
        return 1.2f;
    return std::clamp(v, kMinRt60, kMaxRt60);
}

float Clamp01(float v) {
    if (!std::isfinite(v))
        return 0.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

// RT60 -> comb feedback (the classic mapping): fb = 10^(-3*D/rt60),
// clamped strictly below unity (a feedback of exactly 1 never decays).
float FeedbackFor(double delaySeconds, float rt60) {
    const double fb = std::pow(10.0, -3.0 * delaySeconds / rt60);
    if (!std::isfinite(fb) || fb <= 0.0)
        return 0.0f;
    return std::min(static_cast<float>(fb), 0.999985f);
}

// The processing callback: 1 input bus -> 1 output bus, same channel
// count, continuous processing (tails drain through input silence).
void SchroederProcess(ma_node* pNode, const float** ppFramesIn,
    ma_uint32* pFrameCountIn, float** ppFramesOut,
    ma_uint32* pFrameCountOut) {
    auto* n = reinterpret_cast<SchroederReverbNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    const ma_uint32 ch = n->channels;
    const bool haveInput = ppFramesIn != nullptr && ppFramesIn[0] != nullptr
        && pFrameCountIn != nullptr && *pFrameCountIn > 0;
    float* out = ppFramesOut[0];
    const float* in = haveInput ? ppFramesIn[0] : nullptr;

    // --- per-block coefficient update (the smoothed parameters) ---
    const float smooth = static_cast<float>(1.0
        - std::exp(-1.0 / (kSmoothSeconds * n->sampleRate)));
    n->rt60Cur += (n->rt60Target - n->rt60Cur) * smooth;
    n->dampCur += (n->dampTarget - n->dampCur) * smooth;
    n->wetCur += (n->wetTarget - n->wetCur) * smooth;
    const float rt60 = ClampRt60(n->rt60Cur);
    const float damp = Clamp01(n->dampCur);
    const float wet = Clamp01(n->wetCur);
    for (ma_uint32 c = 0; c < ch; ++c)
        for (int k = 0; k < SchroederReverbNode::kCombs; ++k)
            n->chan[c].combFeedback[k] = FeedbackFor(
                n->chan[c].combSeconds[k], rt60);

    // The exact bypass: MIX mode at wet == 0 short-circuits (the dry
    // bytes pass through UNTOUCHED — never processed and rescaled).
    if (!n->wetOnly && wet <= 0.0f) {
        if (in != nullptr && frames > 0)
            std::memcpy(out, in, sizeof(float) * frames * ch);
        else
            std::memset(out, 0, sizeof(float) * frames * ch);
        return;
    }

    const float dryGain = n->wetOnly ? 0.0f : (1.0f - wet);
    const float wetGain = wet;

    for (ma_uint32 f = 0; f < frames; ++f) {
        for (ma_uint32 c = 0; c < ch; ++c) {
            // Non-finite input reads as silence (the guard contract).
            float x = (in != nullptr) ? in[f * ch + c] : 0.0f;
            if (!std::isfinite(x))
                x = 0.0f;

            // --- the four parallel damped feedback combs ---------
            float proc = 0.0f;
            auto& chan = n->chan[c];
            for (int k = 0; k < SchroederReverbNode::kCombs; ++k) {
                std::vector<float>& buf = chan.comb[k];
                const size_t pos = chan.combPos[k];
                const float delayOut = buf[pos];
                // The one-pole HF damper inside the feedback path.
                float filt = delayOut * damp
                    + chan.combFilter[k] * (1.0f - damp);
                if (filt < 1e-20f && filt > -1e-20f)
                    filt = 0.0f; // denormal snap
                chan.combFilter[k] = filt;
                buf[pos] = x + filt * chan.combFeedback[k];
                chan.combPos[k] = (pos + 1 < buf.size()) ? pos + 1 : 0;
                proc += delayOut;
            }
            proc *= (1.0f / SchroederReverbNode::kCombs); // normalize

            // --- the two series all-pass diffusers ---------------
            for (int k = 0; k < SchroederReverbNode::kAllpasses; ++k) {
                std::vector<float>& buf = chan.allpass[k];
                const size_t pos = chan.allpassPos[k];
                const float wDelayed = buf[pos];
                // y = w[n-D] - g*x; w[n] = x + g*y  (the Schroeder
                // all-pass in its one-buffer form).
                const float y = wDelayed - kAllpassGain * proc;
                buf[pos] = proc + kAllpassGain * y;
                chan.allpassPos[k] = (pos + 1 < buf.size()) ? pos + 1
                    : 0;
                proc = y;
            }
            if (!std::isfinite(proc))
                proc = 0.0f;

            out[f * ch + c] = dryGain * x + wetGain * proc;
        }
    }
}

const ma_node_vtable kSchroederVtable = {
    SchroederProcess,
    nullptr, // 1:1 frame mapping — no required-input callback
    1,       // one input bus
    1,       // one output bus
    // Continuous processing (tails drain through input silence) +
    // null-input allowed (silence in when nothing feeds the node).
    MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT,
};

} // namespace

bool SchroederReverbInit(ma_node_graph* graph,
    const SchroederReverbConfig& cfg, SchroederReverbNode* node) {
    if (!graph || !node)
        return false;
    if (cfg.channels != 1 && cfg.channels != 2)
        return false;
    if (cfg.sampleRate == 0)
        return false;

    std::memset(node, 0, sizeof(*node));
    node->channels = cfg.channels;
    node->sampleRate = cfg.sampleRate;
    node->wetOnly = cfg.wetOnly;
    node->rt60Target = node->rt60Cur = ClampRt60(cfg.rt60);
    node->dampTarget = node->dampCur = Clamp01(cfg.damping);
    node->wetTarget = node->wetCur = Clamp01(cfg.wet);

    const double scale = static_cast<double>(cfg.sampleRate)
        / static_cast<double>(kRefRate);
    for (ma_uint32 c = 0; c < cfg.channels; ++c) {
        auto& chan = node->chan[c];
        for (int k = 0; k < SchroederReverbNode::kCombs; ++k) {
            const int ref = (c == 1) ? kRefCombsR[k] : kRefCombs[k];
            const size_t len = std::max<size_t>(1,
                static_cast<size_t>(ref * scale + 0.5));
            chan.comb[k].assign(len, 0.0f);
            chan.combSeconds[k] = static_cast<double>(len)
                / cfg.sampleRate;
            chan.combFeedback[k] = FeedbackFor(chan.combSeconds[k],
                node->rt60Target);
        }
        for (int k = 0; k < SchroederReverbNode::kAllpasses; ++k) {
            const int ref = (c == 1) ? kRefAllpassR[k]
                : kRefAllpass[k];
            const size_t len = std::max<size_t>(1,
                static_cast<size_t>(ref * scale + 0.5));
            chan.allpass[k].assign(len, 0.0f);
        }
    }

    ma_node_config nc = ma_node_config_init();
    nc.vtable = &kSchroederVtable;
    ma_uint32 inCh[1] = { cfg.channels };
    ma_uint32 outCh[1] = { cfg.channels };
    nc.pInputChannels = inCh;
    nc.pOutputChannels = outCh;
    if (ma_node_init(graph, &nc, nullptr, &node->base) != MA_SUCCESS) {
        // Roll the (already-allocated) lines back — the caller sees a
        // clean false, never a half-built node.
        for (ma_uint32 c = 0; c < cfg.channels; ++c) {
            for (int k = 0; k < SchroederReverbNode::kCombs; ++k)
                node->chan[c].comb[k].clear();
            for (int k = 0; k < SchroederReverbNode::kAllpasses; ++k)
                node->chan[c].allpass[k].clear();
        }
        return false;
    }
    return true;
}

void SchroederReverbUninit(SchroederReverbNode* node) {
    if (!node)
        return;
    ma_node_uninit(&node->base, nullptr);
    for (ma_uint32 c = 0; c < node->channels && c < 2; ++c) {
        for (int k = 0; k < SchroederReverbNode::kCombs; ++k)
            node->chan[c].comb[k].clear();
        for (int k = 0; k < SchroederReverbNode::kAllpasses; ++k)
            node->chan[c].allpass[k].clear();
    }
}

void SchroederReverbSetRt60(SchroederReverbNode* node, float seconds) {
    if (node)
        node->rt60Target = ClampRt60(seconds);
}

void SchroederReverbSetDamping(SchroederReverbNode* node,
    float zeroToOne) {
    if (node)
        node->dampTarget = Clamp01(zeroToOne);
}

void SchroederReverbSetWet(SchroederReverbNode* node, float zeroToOne) {
    if (node)
        node->wetTarget = Clamp01(zeroToOne);
}

} // namespace tour::audio
