// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// AUTHORED AUDIO GRAPHS + CUES (docs/audio_graph.md) — the audio
// system's ASSET layer, the direct parallel of touranim.h's skeletal
// model:
//
//   Skeleton   ->  AudioGraphDoc   (the reusable authored topology)
//   Joint      ->  node            (source / gain / filter / delay)
//   Anim clip  ->  AudioCue        (a named timeline: starts sources,
//                                  automates exposed parameters)
//   Animator   ->  audio instance  (tour::audio, engine-side runtime)
//
// An authored graph is ONE asset (one .audiograph file, one catalog
// id): the node topology, its exposed parameters, and ALL of its cues.
// Cues ride the graph asset the way clips ride a .tourAnim file —
// there is no separate cue asset kind; the generated project.h emits
// per-cue constants (project::audiocues::<graph>_<cue>) that bundle
// (graph id, cue ordinal).
//
// The authored text format (STRICT parse — a malformed graph fails
// with file+line and never packs; the corrupt-refused-at-cook
// discipline, same as the shader-blob magic gate):
//
//   version 1
//
//   # nodes (names unique; "output" is RESERVED — the graph exit)
//   node -name "wind"  -type source -sound "audio/wind.wav" -gain 0.8 -loop on
//   node -name "lpf"   -type lpf    -cutoff 900 -order 2
//   node -name "env"   -type gain   -gain 0.0
//   node -name "master" -type gain  -gain 1.0
//
//   # edges: fan-in AND fan-out allowed; every node must reach "output"
//   connect -from "wind" -to "lpf"
//   connect -from "lpf" -to "env"
//   connect -from "env" -to "master"
//   connect -from "master" -to "output"
//
//   # exposed parameters: a gameplay-facing name bound to one node
//   # field (the ONLY automated/controllable surface)
//   param -name "wind" -node "env" -field gain
//
//   # cues (names unique; the FIRST cue is the graph's default)
//   cue -name "calm"
//   start -node "wind" -t 0
//   ramp  -param "wind" -t 0 -value 0.0
//   ramp  -param "wind" -t 6 -value 0.7
//   set   -param "wind" -t 30 -value 0.4
//
//   cue -name "storm"
//   start -node "wind" -t 0
//   set   -param "wind" -t 0 -value 1.0
//
// Node types + their authored fields:
//   source — a sound asset by VFS path (the pak's Sound-kind blob;
//            resolved LOUDLY by path at instance create) + authored
//            level (-gain) + -loop
//   gain   — a submix bus (volume); also the master/output shape
//   lpf / hpf / bpf — biquads (-cutoff Hz, -order 1..8, default 2)
//   notch / peak    — (-frequency, -q; peak adds -gainDb)
//   loshelf / hishelf — (-frequency, -gainDb, -q)
//   delay  — (-delay seconds, -decay 0..1 the feedback blend)
//
// Param fields (the -field vocabulary): gain, cutoff, frequency, q,
// gainDb, decay. A param's field must be valid for its node's type
// (the biquad lpf/hpf/bpf carry cutoff only — no resonance knob; the
// parametric Q lives on notch/peak/shelf; a delay's LENGTH is static
// in v1, its -decay is live).
//
// Cue events:
//   start/stop -node <source> -t <sec>   (edge-fired at their time)
//   set/ramp   -param <name> -t <sec> -value <v>
//     keyframes on the param's track: set = step (hold), ramp =
//     linear to the NEXT keyframe's value. A track OWNS its param
//     from cue entry (before the first key it holds the BASELINE —
//     the value captured when the cue was entered; a gameplay
//     SetParam between keyframes updates the baseline and holds)
//     through its LAST keyframe; past the last key the track RELEASES
//     the param back to manual control (the last automated value
//     stays applied — cues sustain indefinitely, and a forever-hold
//     would starve gameplay SetParam). Keyframe times are strictly
//     ascending.
//
// A cue has NO duration: its timeline ends at its last event (tracks
// hold their final value) and the sources it started keep sounding
// until stopped, replaced by another cue, or the instance stops.

#include <cstdint>
#include <string>
#include <vector>

namespace tour::asset {

enum class AudioNodeType : uint32_t {
	Source = 1,
	Gain = 2,     // a submix bus (volume); "master output" is a gain node
	LowPass = 3,  // biquad
	HighPass = 4, // biquad
	BandPass = 5, // biquad
	Notch = 6,    // biquad
	Peak = 7,     // biquad EQ
	LowShelf = 8, // biquad EQ
	HighShelf = 9,// biquad EQ
	Delay = 10,   // delay + feedback blend (feedback lives INSIDE the node)
};

// The node's authored fields. Which ones a type consumes is documented
// on the format above; ParseAudioGraphText validates the vocabulary.
struct AudioGraphNode {
	std::string name;
	AudioNodeType type = AudioNodeType::Gain;
	// Source
	std::string sound; // the sound asset's VFS path (Sound-kind blob)
	bool loop = false;
	// Source level / Gain(submix) volume
	float gain = 1.0f;
	// Biquads: lpf/hpf/bpf cutoff; notch/peak/shelf frequency
	float cutoff = 1000.0f;
	float frequency = 1000.0f;
	float q = 1.0f;
	float gainDb = 0.0f; // peak/shelf
	uint32_t order = 2;  // lpf/hpf/bpf (1..8)
	// Delay
	float delaySeconds = 0.25f;
	float decay = 0.5f;
};

enum class AudioParamField : uint32_t {
	Gain = 1,
	Cutoff = 2,
	Frequency = 3,
	Q = 4,
	GainDb = 5,
	Decay = 6,
};

// An EXPOSED parameter: the gameplay-facing name bound to one node
// field. This is the ONLY surface cues automate and gameplay sets —
// raw node addressing never leaves the asset.
struct AudioGraphParam {
	std::string name;
	std::string node;
	AudioParamField field = AudioParamField::Gain;
};

struct AudioGraphEdge {
	std::string from;
	std::string to; // "output" = the graph exit
};

struct AudioCueKeyframe {
	double t = 0.0;
	float value = 0.0f;
	bool linear = true; // true: linear to the NEXT key; false: step (hold)
};

// Edge-fired source activity (sorted by t at finalize).
struct AudioCueEvent {
	enum class Type : uint32_t { StartSource = 1, StopSource = 2 };
	double t = 0.0;
	Type type = Type::StartSource;
	std::string node; // must name a Source node
};

// One param's keyframe track inside a cue (keys sorted by t).
struct AudioCueParamTrack {
	std::string param; // must name an exposed param
	std::vector<AudioCueKeyframe> keys;
};

struct AudioCue {
	std::string name;
	std::vector<AudioCueEvent> events;
	std::vector<AudioCueParamTrack> tracks;
	// Computed at finalize: the timeline end (last event/key time;
	// automation holds past it — this is bookkeeping for callers).
	double endT = 0.0;
};

// ONE authored audio graph asset. Immutable + shared once parsed; the
// runtime instance (tour::audio) owns its own COPY (clips pass by
// value in the animator model — graphs are small, the instance keeps
// its copy so the caller's storage can be transient).
struct AudioGraphDoc {
	std::vector<AudioGraphNode> nodes;
	std::vector<AudioGraphEdge> edges;
	std::vector<AudioGraphParam> params;
	std::vector<AudioCue> cues; // cues[0] is the graph's default cue
};

// Parses the authored text. STRICT: any malformed verb, unknown type,
// duplicate name, dangling edge, cycle, invalid param binding or
// empty-cue-set fails with err carrying the LINE NUMBER (the
// nodeschema discipline — cook-time validation rejects the file before
// it can pack). `warnings` (optional) carries non-fatal notes.
bool ParseAudioGraphText(const std::string& text, AudioGraphDoc& out,
	std::string& err, std::vector<std::string>* warnings = nullptr);

// ---- lookups for consumers (the runtime + the catalog emitter) ----
// Index of the named node / param / cue, or -1 when absent.
[[nodiscard]] int AudioGraphFindNode(const AudioGraphDoc& doc,
	const std::string& name);
[[nodiscard]] int AudioGraphFindParam(const AudioGraphDoc& doc,
	const std::string& name);
[[nodiscard]] int AudioGraphFindCue(const AudioGraphDoc& doc,
	const std::string& name);

// Whether a param field is meaningful for a node type (the -field
// vocabulary gate used by the parser + the emitter).
[[nodiscard]] bool AudioGraphNodeFieldValid(AudioNodeType type,
	AudioParamField field);

// The reserved edge target naming the graph exit (not a node).
inline constexpr const char* kAudioGraphOutput = "output";

} // namespace tour::asset
