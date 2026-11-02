// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// audiograph_tests: the .audiograph authored-format parser + the
// miniaudio-backed instance engine, headless + DETERMINISTIC (the
// AudioEngine's deviceless lane: PumpOffline renders each chunk with
// no device, no thread, no wall clock). Run via CTest.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <Engine/Asset/audiograph.h>
#include <Engine/Audio/audio.h>

// The raw world-bus isolation block (TestWorldBus) drives miniaudio
// directly through the SAME configuration wrapper the engine uses
// (plus the Schroeder node for the full-bus probe).
#include "miniaudio_config.h"
#include "schroeder_reverb.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

bool Near(float a, float b, float eps = 1e-3f) {
	return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// The parser (the nodeschema discipline: strict, line-numbered errors).
// ---------------------------------------------------------------------------

const char* kGoodGraph =
	"version 1\n"
	"\n"
	"# the demo wind graph\n"
	"node -name \"wind\" -type source -sound \"audio/wind.wav\" -gain 0.8 -loop on\n"
	"node -name \"windLpf\" -type lpf -cutoff 900 -order 2\n"
	"node -name \"env\" -type gain -gain 0.0\n"
	"node -name \"master\" -type gain -gain 1.0\n"
	"\n"
	"connect -from \"wind\" -to \"windLpf\"\n"
	"connect -from \"windLpf\" -to \"env\"\n"
	"connect -from \"env\" -to \"master\"\n"
	"connect -from \"master\" -to \"output\"\n"
	"\n"
	"param -name \"wind\" -node \"env\" -field gain\n"
	"param -name \"tone\" -node \"windLpf\" -field cutoff\n"
	"\n"
	"cue -name \"calm\"\n"
	"start -node \"wind\" -t 0\n"
	"ramp -param \"wind\" -t 0 -value 0.0\n"
	"ramp -param \"wind\" -t 3 -value 0.8\n"
	"set -param \"tone\" -t 0 -value 600\n"
	"\n"
	"cue -name \"storm\"\n"
	"start -node \"wind\" -t 0\n"
	"set -param \"wind\" -t 0 -value 1.0\n"
	"set -param \"tone\" -t 0 -value 2000\n";

void TestParse() {
	using namespace tour::asset;
	AudioGraphDoc doc;
	std::string err;
	std::vector<std::string> warnings;
	Check(ParseAudioGraphText(kGoodGraph, doc, err, &warnings),
		"ag.parse.good");
	if (!err.empty())
		std::cout << "  (err: " << err << ")\n";
	if (doc.nodes.size() != 4 || doc.cues.size() != 2) {
		// The structural checks below index into the parsed doc — a
		// failed good-parse already reported; stop here loudly.
		std::cout << "  (good graph failed to parse — skipping the "
			"field checks)\n";
		Check(false, "ag.parse.doc-usable");
		return;
	}
	Check(doc.edges.size() == 4, "ag.parse.edge-count");
	Check(doc.nodes.size() == 4, "ag.parse.node-count");
	Check(doc.params.size() == 2, "ag.parse.param-count");
	Check(doc.cues.size() == 2, "ag.parse.cue-count");
	Check(doc.cues[0].name == "calm", "ag.parse.default-cue-first");
	Check(AudioGraphFindNode(doc, "windLpf") == 1, "ag.parse.find-node");
	Check(AudioGraphFindParam(doc, "wind") == 0, "ag.parse.find-param");
	Check(AudioGraphFindCue(doc, "storm") == 1, "ag.parse.find-cue");
	Check(AudioGraphFindNode(doc, "nope") == -1, "ag.parse.find-miss");
	Check(doc.nodes[0].loop && doc.nodes[0].gain == 0.8f,
		"ag.parse.source-fields");
	Check(doc.nodes[1].cutoff == 900.0f && doc.nodes[1].order == 2,
		"ag.parse.lpf-fields");
	Check(doc.cues[0].events.size() == 1
		&& doc.cues[0].events[0].t == 0.0, "ag.parse.cue-events");
	Check(doc.cues[0].tracks.size() == 2, "ag.parse.cue-tracks");
	Check(doc.cues[0].tracks[0].param == "wind"
		&& doc.cues[0].tracks[1].param == "tone",
		"ag.parse.tracks-in-declaration-order");
	Check(doc.cues[0].tracks[0].keys.size() == 2
		&& doc.cues[0].tracks[0].keys[1].value == 0.8f,
		"ag.parse.keyframes");
	Check(doc.cues[0].tracks[1].keys[0].linear == false,
		"ag.parse.step-vs-ramp");
	Check(Near(static_cast<float>(doc.cues[0].endT), 3.0f, 1e-6f),
		"ag.parse.cue-end");

	// The failure ladder: every malformation names its line.
	auto failsWith = [&](const std::string& text, const char* needle,
		const char* name) {
		AudioGraphDoc d;
		std::string e;
		const bool ok = ParseAudioGraphText(text, d, e);
		Check(!ok && e.find(needle) != std::string::npos, name);
		if (!ok && e.find(needle) == std::string::npos)
			std::cout << "  (err was: " << e << ")\n";
	};
	failsWith("node -name \"a\" -type gain\n", "version",
		"ag.parse.missing-version");
	failsWith(std::string(kGoodGraph) + "node -name \"wind\" -type gain\n",
		"duplicate node", "ag.parse.duplicate-node");
	failsWith(
		"version 1\nnode -name \"a\" -type source -sound \"s.wav\"\n"
		"connect -from \"a\" -to \"ghost\"\n",
		"names no declared node", "ag.parse.dangling-edge");
	failsWith(
		"version 1\nnode -name \"a\" -type gain\n",
		"no cues declared", "ag.parse.no-cues");
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"m\" -type gain\n"
		"connect -from \"a\" -to \"m\"\n"
		"connect -from \"m\" -to \"output\"\n"
		"cue -name \"c\"\n"
		"start -node \"m\" -t 0\n",
		"not a source", "ag.parse.start-non-source");
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"m\" -type gain\n"
		"connect -from \"a\" -to \"m\"\n"
		"connect -from \"m\" -to \"output\"\n"
		"param -name \"p\" -node \"m\" -field cutoff\n"
		"cue -name \"c\"\n",
		"field not valid", "ag.parse.bad-param-field");
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"m\" -type gain\n"
		"connect -from \"a\" -to \"m\"\n"
		"connect -from \"m\" -to \"output\"\n"
		"cue -name \"c\"\n"
		"ramp -param \"nope\" -t 0 -value 1\n",
		"names no exposed param", "ag.parse.unknown-param");
	// A branch that never reaches the exit is a structural error.
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"m\" -type gain\n"
		"connect -from \"a\" -to \"m\"\n"
		"cue -name \"c\"\n",
		"does not reach 'output'", "ag.parse.dangling-branch");
	// A cycle in the edge topology (feedback belongs inside delay).
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"g1\" -type gain\n"
		"node -name \"g2\" -type gain\n"
		"connect -from \"a\" -to \"g1\"\n"
		"connect -from \"g1\" -to \"g2\"\n"
		"connect -from \"g2\" -to \"g1\"\n"
		"connect -from \"g2\" -to \"output\"\n"
		"cue -name \"c\"\n",
		"cycle", "ag.parse.cycle");
	// Non-ascending keyframes.
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"m\" -type gain\n"
		"connect -from \"a\" -to \"m\"\n"
		"connect -from \"m\" -to \"output\"\n"
		"param -name \"p\" -node \"m\" -field gain\n"
		"cue -name \"c\"\n"
		"ramp -param \"p\" -t 2 -value 1\n"
		"ramp -param \"p\" -t 1 -value 0\n",
		"non-ascending", "ag.parse.key-order");
	// A source without -sound.
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source\n"
		"node -name \"m\" -type gain\n"
		"connect -from \"a\" -to \"m\"\n"
		"connect -from \"m\" -to \"output\"\n"
		"cue -name \"c\"\n",
		"needs -sound", "ag.parse.source-needs-sound");
	// The Q vocabulary gate: the biquad lpf carries cutoff only.
	failsWith(
		"version 1\n"
		"node -name \"a\" -type source -sound \"s.wav\"\n"
		"node -name \"f\" -type lpf -cutoff 500\n"
		"connect -from \"a\" -to \"f\"\n"
		"connect -from \"f\" -to \"output\"\n"
		"param -name \"p\" -node \"f\" -field q\n"
		"cue -name \"c\"\n",
		"field not valid", "ag.parse.lpf-no-q");
}

// ---------------------------------------------------------------------------
// The instance engine (deviceless, deterministic offline rendering).
// ---------------------------------------------------------------------------

// A canonical 16-bit PCM stereo WAV (a sine) built in memory.
std::vector<uint8_t> MakeWavSine(float freq, float seconds, float amp,
	int rate = 48000, int ch = 2) {
	const uint32_t frames = static_cast<uint32_t>(seconds * rate);
	const uint32_t dataBytes = frames * static_cast<uint32_t>(ch) * 2;
	std::vector<uint8_t> b(44 + dataBytes, 0);
	auto put4 = [&](size_t o, const char* s) {
		std::memcpy(b.data() + o, s, 4);
	};
	auto putU32 = [&](size_t o, uint32_t v) {
		std::memcpy(b.data() + o, &v, 4);
	};
	auto putU16 = [&](size_t o, uint16_t v) {
		std::memcpy(b.data() + o, &v, 2);
	};
	put4(0, "RIFF");
	putU32(4, 36 + dataBytes);
	put4(8, "WAVE");
	put4(12, "fmt ");
	putU32(16, 16);
	putU16(20, 1); // PCM
	putU16(22, static_cast<uint16_t>(ch));
	putU32(24, static_cast<uint32_t>(rate));
	putU32(28, static_cast<uint32_t>(rate) * static_cast<uint32_t>(ch) * 2);
	putU16(32, static_cast<uint16_t>(ch) * 2);
	putU16(34, 16);
	put4(36, "data");
	putU32(40, dataBytes);
	auto* s = reinterpret_cast<int16_t*>(b.data() + 44);
	for (uint32_t f = 0; f < frames; ++f) {
		const float v = amp
			* std::sin(2.0f * 3.14159265f * freq
				* static_cast<float>(f) / static_cast<float>(rate));
		for (int c = 0; c < ch; ++c)
			s[f * static_cast<uint32_t>(ch)
				+ static_cast<uint32_t>(c)]
				= static_cast<int16_t>(v * 32767.0f);
	}
	return b;
}

float RmsOf(const std::vector<float>& buf) {
	if (buf.empty())
		return 0.0f;
	double acc = 0.0;
	for (float v : buf)
		acc += static_cast<double>(v) * static_cast<double>(v);
	return static_cast<float>(std::sqrt(acc / buf.size()));
}

// The clip resolver over a fixed path -> bytes table.
struct ResolverCtx {
	tour::audio::AudioEngine* engine = nullptr;
	std::vector<std::pair<std::string, tour::audio::ClipHandle>> table;
};

tour::audio::ClipHandle ResolveClip(void* user, const char* path) {
	auto* ctx = static_cast<ResolverCtx*>(user);
	if (!ctx || !path)
		return {};
	for (const auto& [p, h] : ctx->table)
		if (p == path)
			return h;
	return {};
}

void TestInstances() {
	using namespace tour::audio;
	AudioEngine audio;
	Config cfg{};
	cfg.deviceless = true; // deterministic offline rendering
	cfg.framesPerChunk = 1024;
	if (!audio.Init(cfg)) {
		Check(false, "ag.inst.engine-init");
		std::cout << "  (err: " << audio.LastError() << ")\n";
		return;
	}
	Check(true, "ag.inst.engine-init");

	// Two clips: the graph's wind bed + a legacy one-shot.
	const std::vector<uint8_t> windWav =
		MakeWavSine(220.0f, 2.0f, 0.5f);
	const std::vector<uint8_t> blipWav = MakeWavSine(880.0f, 0.1f, 0.5f);
	const ClipHandle windClip =
		audio.LoadWavFromMemory(windWav.data(), windWav.size());
	const ClipHandle blipClip =
		audio.LoadWavFromMemory(blipWav.data(), blipWav.size());
	Check(windClip.valid() && blipClip.valid(), "ag.inst.clips-loaded");

	ResolverCtx resolver;
	resolver.engine = &audio;
	resolver.table.push_back({ "audio/wind.wav", windClip });

	tour::asset::AudioGraphDoc doc;
	std::string err;
	std::vector<std::string> warnings;
	Check(tour::asset::ParseAudioGraphText(kGoodGraph, doc, err, &warnings),
		"ag.inst.doc-parses");

	// A graph referencing an unknown sound fails the create LOUDLY with
	// the named path (clip resolution is caller-side).
	{
		tour::asset::AudioGraphDoc bad = doc;
		bad.nodes[0].sound = "audio/missing.wav";
		std::string e2;
		const auto h = audio.CreateInstance(bad, ResolveClip, &resolver,
			&e2);
		Check(!h && e2.find("audio/missing.wav") != std::string::npos,
			"ag.inst.unresolved-sound-fails");
	}

	// The real create + facade.
	std::string createErr;
	const auto handle = audio.CreateInstance(doc, ResolveClip, &resolver,
		&createErr);
	Check(handle && createErr.empty(), "ag.inst.create");
	Check(audio.IsInstanceAlive(handle), "ag.inst.alive");

	AudioInstance inst(audio, handle);
	Check(static_cast<bool>(inst), "ag.inst.facade-valid");
	Check(!inst.playing(), "ag.inst.not-playing-yet");

	std::vector<float> buf(1024 * 2);
	auto pump = [&](int chunks) {
		// Renders + advances; returns the LAST chunk's RMS.
		float last = 0.0f;
		for (int i = 0; i < chunks; ++i) {
			audio.PumpOffline(buf.data(), 1024);
			last = RmsOf(buf);
		}
		return last;
	};

	// The default cue: the wind bed ramps 0 -> 0.8 over 3 s. Early
	// chunks are near-silent; chunks past the ramp are loud. 3 s at
	// 1024 frames / 48 kHz = ~141 chunks.
	inst.play();
	const float earlyRms = pump(10);   // ~0.21 s in: ramp barely begun
	const float lateRms = pump(160);   // ~3.6 s in: ramp complete
	Check(inst.playing(), "ag.inst.playing-after-play");
	Check(earlyRms < lateRms * 0.25f, "ag.inst.automation-ramps");
	Check(lateRms > 0.01f, "ag.inst.audible");
	std::cout << "  (rms early=" << earlyRms << " late=" << lateRms
		<< ")\n";

	// Gameplay set lands immediately (step above the automation's
	// held value; the cue's track has ended so nothing overrides it).
	inst.set("wind", 0.05f);
	const float quiet = pump(4);
	Check(quiet < lateRms * 0.25f, "ag.inst.setparam-immediate");

	// Crossfade into the storm cue over 1 s: storm sets wind to 1.0 —
	// the blend ends louder than the calm bed.
	inst.crossFade("storm", 1.0f);
	const float midFade = pump(24);    // ~0.5 s: halfway through
	const float postFade = pump(40);   // ~1.35 s: fade complete
	Check(postFade > quiet, "ag.inst.crossfade-ends-audible");
	std::cout << "  (rms quiet=" << quiet << " mid=" << midFade
		<< " post=" << postFade << ")\n";

	// Stop silences the instance (its node set stays built).
	inst.stop();
	const float stopped = pump(4);
	Check(stopped < 0.0005f, "ag.inst.stop-silences");
	Check(!inst.playing(), "ag.inst.stopped-flag");

	// Restart works (the node set survives a stop).
	inst.play();
	const float resumed = pump(150);
	Check(inst.playing() && resumed > 0.01f, "ag.inst.restart");

	// The instance-level volume rides the bus on top of the graph.
	inst.setVolume(0.0f);
	const float muted = pump(4);
	Check(muted < 0.0005f, "ag.inst.instance-volume");
	inst.setVolume(1.0f);

	// The legacy voice lane rides the SAME renderer.
	const auto voice = audio.Play(blipClip, 1.0f, false);
	Check(voice.valid(), "ag.inst.legacy-play");
	audio.PumpOffline(buf.data(), 1024);
	Check(audio.IsVoicePlaying(voice), "ag.inst.legacy-playing");
	audio.Stop(voice);
	audio.PumpOffline(buf.data(), 1024);
	Check(!audio.IsVoicePlaying(voice), "ag.inst.legacy-stops");

	// Destroy: the handle dies, stale facades fail silently, the
	// renderer keeps running.
	Check(audio.DestroyInstance(handle), "ag.inst.destroy");
	Check(!audio.IsInstanceAlive(handle), "ag.inst.dead");
	Check(!static_cast<bool>(inst), "ag.inst.facade-stale");
	inst.play(); // silent no-op on the stale handle
	inst.stop();
	Check(true, "ag.inst.stale-ops-noop");

	// The legacy lane still renders after the instance teardown (the
	// engine's graph survived the instance's node set).
	const auto voice2 = audio.Play(windClip, 0.8f, false);
	audio.PumpOffline(buf.data(), 1024);
	Check(audio.IsVoicePlaying(voice2)
		&& RmsOf(buf) > 0.01f, "ag.inst.engine-survives-teardown");

	audio.Shutdown();
	Check(true, "ag.inst.shutdown");
}

// ---------------------------------------------------------------------------
// The Schroeder reverb: the authored-graph inline form (the DSP is the
// same custom ma_node the environmental world bus will run wet-only —
// PR 3). All deterministic through the deviceless offline lane.
// ---------------------------------------------------------------------------
void TestReverb() {
	using namespace tour::audio;

	// The grammar: the node flags + the bounded shape + the param
	// vocabulary.
	{
		tour::asset::AudioGraphDoc doc;
		std::string err;
		const std::string text =
			"version 1\n"
			"node -name \"air\" -type source"
			" -sound \"audio/blip.wav\" -gain 0.9\n"
			"node -name \"hall\" -type reverb -wet 0.28 -rt60 1.8"
			" -damping 0.6\n"
			"connect -from \"air\" -to \"hall\"\n"
			"connect -from \"hall\" -to \"output\"\n"
			"param -name \"hallWet\" -node \"hall\" -field wet\n"
			"param -name \"hallTail\" -node \"hall\" -field rt60\n"
			"cue -name \"go\"\n"
			"start -node \"air\" -t 0\n";
		Check(tour::asset::ParseAudioGraphText(text, doc, err),
			"ag.reverb.parse");
		if (!err.empty())
			std::cout << "  (err: " << err << ")\n";
		Check(doc.nodes[1].type == tour::asset::AudioNodeType::Reverb
			&& doc.nodes[1].reverbWet == 0.28f
			&& doc.nodes[1].reverbRt60 == 1.8f
			&& doc.nodes[1].reverbDamping == 0.6f,
			"ag.reverb.fields");
		// The bounded shape rejects.
		auto rejects = [&](const std::string& nodeLine,
			const char* name) {
			tour::asset::AudioGraphDoc d;
			std::string e;
			const std::string t =
				"version 1\n"
				"node -name \"air\" -type source"
				" -sound \"audio/blip.wav\"\n"
				+ nodeLine +
				"\nconnect -from \"air\" -to \"hall\"\n"
				"connect -from \"hall\" -to \"output\"\n"
				"cue -name \"go\"\n";
			Check(!tour::asset::ParseAudioGraphText(t, d, e), name);
		};
		rejects("node -name \"hall\" -type reverb -rt60 12",
			"ag.reverb.rt60-bounded");
		rejects("node -name \"hall\" -type reverb -wet 1.5",
			"ag.reverb.wet-bounded");
		rejects("node -name \"hall\" -type reverb -damping -1",
			"ag.reverb.damping-bounded");
		// The field vocabulary: wet is reverb-only.
		{
			tour::asset::AudioGraphDoc d;
			std::string e;
			const std::string t =
				"version 1\n"
				"node -name \"air\" -type source"
				" -sound \"audio/blip.wav\"\n"
				"node -name \"g\" -type gain\n"
				"connect -from \"air\" -to \"g\"\n"
				"connect -from \"g\" -to \"output\"\n"
				"param -name \"p\" -node \"g\" -field wet\n"
				"cue -name \"go\"\n";
			Check(!tour::asset::ParseAudioGraphText(t, d, e),
				"ag.reverb.field-type-gate");
		}
	}

	// The DSP, through graphs: a one-shot blip into a fully-wet hall.
	AudioEngine audio;
	Config cfg{};
	cfg.deviceless = true;
	if (!audio.Init(cfg)) {
		Check(false, "ag.reverb.engine-init");
		return;
	}
	Check(true, "ag.reverb.engine-init");

	// A 50 ms click-ish burst (a sine at full amplitude, one-shot).
	const std::vector<uint8_t> blipWav = MakeWavSine(1000.0f, 0.05f,
		0.9f);
	const ClipHandle blip = audio.LoadWavFromMemory(blipWav.data(),
		blipWav.size());
	Check(blip.valid(), "ag.reverb.clip");

	ResolverCtx resolver;
	resolver.engine = &audio;
	resolver.table.push_back({ "audio/blip.wav", blip });

	// Builds the blip->(reverb?)->output graph + a playing instance,
	// pumps `chunks` and returns the per-chunk RMS series. `middle`
	// is the full node-decl + connect block between the source line
	// and the cue (the reverb form routes air->hall->output; the
	// direct form routes air->output with no extra node).
	struct Run {
		std::vector<float> chunkRms;
	};
	auto run = [&](const std::string& middle, int chunks) -> Run {
		const std::string text =
			"version 1\n"
			"node -name \"air\" -type source"
			" -sound \"audio/blip.wav\" -gain 0.9\n"
			+ middle +
			"cue -name \"go\"\n"
			"start -node \"air\" -t 0\n";
		tour::asset::AudioGraphDoc doc;
		std::string err;
		Run out;
		if (!tour::asset::ParseAudioGraphText(text, doc, err))
			return out;
		const auto h = audio.CreateInstance(doc, ResolveClip, &resolver,
			&err);
		if (!h)
			return out;
		AudioInstance inst(audio, h);
		inst.play();
		std::vector<float> buf(1024 * 2);
		for (int i = 0; i < chunks; ++i) {
			audio.PumpOffline(buf.data(), 1024);
			out.chunkRms.push_back(RmsOf(buf));
		}
		audio.DestroyInstance(h);
		return out;
	};
	const auto reverbMiddle = [](float wet, float rt60) {
		return "node -name \"hall\" -type reverb -wet "
			+ std::to_string(wet) + " -rt60 " + std::to_string(rt60)
			+ "\nconnect -from \"air\" -to \"hall\"\n"
			"connect -from \"hall\" -to \"output\"\n";
	};
	const std::string directMiddle =
		"connect -from \"air\" -to \"output\"\n";

	// The blip is 50 ms = ~2.4 chunks; everything past chunk ~4 is
	// pure reverb tail.
	const Run dry = run("node -name \"hall\" -type reverb -wet 0\n"
		"connect -from \"air\" -to \"hall\"\n"
		"connect -from \"hall\" -to \"output\"\n", 8);
	const Run direct = run(directMiddle, 8); // no reverb node at all
	const Run wetShort = run(reverbMiddle(1.0f, 0.25f), 120);
	const Run wetLong = run(reverbMiddle(1.0f, 3.0f), 120);

	// wet=0 is an EXACT bypass: byte-identical per-chunk RMS to the
	// no-reverb graph (the memcpy short-circuit).
	Check(dry.chunkRms.size() == 8 && direct.chunkRms.size() == 8,
		"ag.reverb.runs-complete");
	{
		bool equal = dry.chunkRms.size() == direct.chunkRms.size();
		for (size_t i = 0; equal && i < dry.chunkRms.size(); ++i)
			equal = dry.chunkRms[i] == direct.chunkRms[i];
		Check(equal, "ag.reverb.wet0-exact-bypass");
	}

	// The tail outlives the 50 ms source: chunks past the dry part
	// stay audible, then decays.
	{
		const auto& r = wetLong.chunkRms;
		Check(r.size() == 120, "ag.reverb.long-run-complete");
		// ~2.5 s in (chunk 118, way past the 50 ms blip): still
		// audible at rt60 3.
		Check(r[118] > 1e-4f, "ag.reverb.tail-outlives-source");
		std::cout << "  (tail rms @2.5s=" << r[118] << ")\n";
	}
	// RT60 ordering: the short tail is dead where the long one rings.
	Check(wetShort.chunkRms.size() == 120
		&& wetLong.chunkRms.size() == 120, "ag.reverb.runs-2");
	Check(wetLong.chunkRms[60] > wetShort.chunkRms[60] * 10.0f,
		"ag.reverb.rt60-decay-order");
	std::cout << "  (short@1.3s=" << wetShort.chunkRms[60]
		<< " long@1.3s=" << wetLong.chunkRms[60] << ")\n";

	// Stereo decorrelation: L != R after the impulse (the +23-sample
	// right-channel tunings).
	{
		const std::string text =
			"version 1\n"
			"node -name \"air\" -type source"
			" -sound \"audio/blip.wav\" -gain 0.9\n"
			"node -name \"hall\" -type reverb -wet 1 -rt60 2.0\n"
			"connect -from \"air\" -to \"hall\"\n"
			"connect -from \"hall\" -to \"output\"\n"
			"cue -name \"go\"\n"
			"start -node \"air\" -t 0\n";
		tour::asset::AudioGraphDoc doc;
		std::string err;
		Check(tour::asset::ParseAudioGraphText(text, doc, err),
			"ag.reverb.stereo-doc");
		const auto h = audio.CreateInstance(doc, ResolveClip, &resolver,
			&err);
		Check(h && err.empty(), "ag.reverb.stereo-create");
		AudioInstance inst(audio, h);
		inst.play();
		std::vector<float> buf(1024 * 2);
		bool decorrelated = false;
		bool finite = true;
		for (int i = 0; i < 40; ++i) {
			audio.PumpOffline(buf.data(), 1024);
			float lAcc = 0.0f, rAcc = 0.0f;
			for (int f = 0; f < 1024; ++f) {
				lAcc += std::fabs(buf[f * 2]);
				rAcc += std::fabs(buf[f * 2 + 1]);
				if (!std::isfinite(buf[f * 2])
					|| !std::isfinite(buf[f * 2 + 1]))
					finite = false;
			}
			// Skip the first chunk (the dry blip is identical L/R by
			// construction — MakeWavSine writes the same sample to
			// both channels).
			if (i > 3 && std::fabs(lAcc - rAcc) > 1e-3f)
				decorrelated = true;
		}
		Check(decorrelated, "ag.reverb.stereo-decorrelated");
		Check(finite, "ag.reverb.output-finite");
		audio.DestroyInstance(h);
	}

	// Live parameter automation: retuning rt60 mid-tail changes the
	// decay (and never allocates/locks — it rides the same command
	// lane as every other param).
	{
		const std::string text =
			"version 1\n"
			"node -name \"air\" -type source"
			" -sound \"audio/blip.wav\" -gain 0.9\n"
			"node -name \"hall\" -type reverb -wet 1 -rt60 0.3\n"
			"connect -from \"air\" -to \"hall\"\n"
			"connect -from \"hall\" -to \"output\"\n"
			"param -name \"tail\" -node \"hall\" -field rt60\n"
			"cue -name \"go\"\n"
			"start -node \"air\" -t 0\n";
		tour::asset::AudioGraphDoc doc;
		std::string err;
		Check(tour::asset::ParseAudioGraphText(text, doc, err),
			"ag.reverb.param-doc");
		const auto h = audio.CreateInstance(doc, ResolveClip, &resolver,
			&err);
		Check(h && err.empty(), "ag.reverb.param-create");
		AudioInstance inst(audio, h);
		inst.play();
		std::vector<float> buf(1024 * 2);
		for (int i = 0; i < 4; ++i)
			audio.PumpOffline(buf.data(), 1024);
		// Stretch the tail from 0.3 s to 6 s mid-flight.
		inst.set("tail", 6.0f);
		float late = 0.0f;
		for (int i = 0; i < 100; ++i) {
			audio.PumpOffline(buf.data(), 1024);
			late = std::max(late, RmsOf(buf));
		}
		// The retune must not silence the tail (the smoothed retarget
		// keeps it ringing) — the exact value depends on where the
		// decay was when retargeted; the assertion is "still audible
		// 2 s later", which the old 0.3 s target could never do.
		Check(late > 1e-4f, "ag.reverb.param-retarget");
		std::cout << "  (retarget late rms=" << late << ")\n";
		audio.DestroyInstance(h);
	}

	audio.Shutdown();
	Check(true, "ag.reverb.shutdown");
}

// ---------------------------------------------------------------------------
// The environmental WORLD-SFX bus (PR 3): spatial voices ride the shared
// send (a wet-only Schroeder), direct voices bypass it, and the send's
// tail outlives the one-shot dry source. All offline/deterministic.
// ---------------------------------------------------------------------------
void TestWorldBus() {
	using namespace tour::audio;
	// RAW MINIAUDIO ISOLATION FIRST: the stock sound->group->endpoint
	// configuration, outside AudioEngine (a diagnostic pin — if this
	// is silent, the bus design must change; if it rings, the engine's
	// config differs somewhere). BISECT LADDER: (a) the stock group;
	// (b) NO_DEFAULT_ATTACHMENT + a manual attach; (c) + a splitter.
	{
		ma_engine eng;
		ma_engine_config ec = ma_engine_config_init();
		ec.channels = 2;
		ec.sampleRate = 48000;
		ec.noDevice = MA_TRUE;
		ec.periodSizeInFrames = 1024;
		ec.defaultVolumeSmoothTimeInPCMFrames = 256;
		if (ma_engine_init(&ec, &eng) != MA_SUCCESS) {
			Check(false, "ag.bus.raw-engine");
		}
		else {
			// A CLEAN float sine buffer (no WAV wrapper).
			std::vector<float> sine(48000); // 0.5 s stereo interleaved
			for (size_t i = 0; i < sine.size() / 2; ++i) {
				const float v = 0.9f
					* std::sin(2.0f * 3.14159265f * 700.0f
						* float(i) / 48000.0f);
				sine[i * 2] = v;
				sine[i * 2 + 1] = v;
			}
			auto probe = [&](const char* name, bool manualAttach,
					bool withSplitter) {
				ma_audio_buffer buf;
				ma_audio_buffer_config bc =
					ma_audio_buffer_config_init(ma_format_f32, 2,
						sine.size() / 2, sine.data(), nullptr);
				bc.sampleRate = 48000;
				if (ma_audio_buffer_init(&bc, &buf) != MA_SUCCESS) {
					Check(false, name);
					return;
				}
				ma_sound_group group;
				ma_splitter_node split;
				bool splitInit = false;
				if (manualAttach) {
					ma_sound_group_config gc =
						ma_sound_group_config_init_2(&eng);
					gc.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
					if (ma_sound_group_init_ex(&eng, &gc, &group)
						!= MA_SUCCESS) {
						Check(false, name);
						ma_audio_buffer_uninit(&buf);
						return;
					}
				}
				else {
					if (ma_sound_group_init(&eng, 0, nullptr, &group)
						!= MA_SUCCESS) {
						Check(false, name);
						ma_audio_buffer_uninit(&buf);
						return;
					}
				}
				if (withSplitter) {
					ma_splitter_node_config sc =
						ma_splitter_node_config_init(2);
					sc.outputBusCount = 2;
					if (ma_splitter_node_init(
							ma_engine_get_node_graph(&eng), &sc,
							nullptr, &split) == MA_SUCCESS) {
						splitInit = true;
						ma_node_attach_output_bus(&split.base, 0,
							ma_engine_get_endpoint(&eng), 0);
						ma_node_attach_output_bus(&split.base, 1,
							ma_engine_get_endpoint(&eng), 0);
						ma_node_attach_output_bus(
							(ma_node*)&group, 0, &split.base, 0);
					}
				}
				else if (manualAttach) {
					ma_node_attach_output_bus((ma_node*)&group, 0,
						ma_engine_get_endpoint(&eng), 0);
				}
				ma_sound sound;
				if (ma_sound_init_from_data_source(&eng,
						(ma_data_source*)&buf,
						MA_SOUND_FLAG_NO_SPATIALIZATION, &group,
						&sound) != MA_SUCCESS) {
					Check(false, name);
				}
				else {
					ma_sound_start(&sound);
					float out[2048];
					float peak = 0.0f;
					for (int i = 0; i < 8; ++i) {
						ma_engine_read_pcm_frames(&eng, out, 1024,
							nullptr);
						for (float v : out)
							peak = std::max(peak, std::fabs(v));
					}
					Check(peak > 0.1f && peak < 2.0f, name);
					std::cout << "  (" << name << " peak=" << peak
						<< ")\n";
					ma_sound_uninit(&sound);
				}
				if (splitInit)
					ma_splitter_node_uninit(&split, nullptr);
				ma_sound_group_uninit(&group);
				ma_audio_buffer_uninit(&buf);
			};
			probe("ag.bus.raw-stock", false, false);
			probe("ag.bus.raw-manual", true, false);
			probe("ag.bus.raw-split", true, true);
			// (e) THE SEQUENTIAL ONE-SHOT REPRO: sound #1 plays +
			// ends naturally; sound #2 plays after — silent?
			{
				// VARIANT A: #1 ends + is UNINITED (the full poison).
				auto runSeq = [&](bool uninitFirst) {
					// PER-VOICE GROUPS (the shield probe): each sound
					// attaches through its OWN throwaway group — a
					// group never signals AT_END, so the endpoint bus
					// never sees a data-source end.
					ma_sound_group grp1;
					ma_sound_group_init(&eng, 0, nullptr, &grp1);
					ma_sound_group grp2;
					ma_sound_group_init(&eng, 0, nullptr, &grp2);
					ma_audio_buffer buf1;
					std::vector<float> blip(48000); // 1 s: LOOPS in #1
					for (size_t i = 0; i < blip.size() / 2; ++i) {
						const float v = 0.9f
							* std::sin(2.0f * 3.14159265f
								* 700.0f * float(i) / 48000.0f);
						blip[i * 2] = v;
						blip[i * 2 + 1] = v;
					}
					ma_audio_buffer_config bc1 =
						ma_audio_buffer_config_init(ma_format_f32, 2,
							blip.size() / 2, blip.data(), nullptr);
					bc1.sampleRate = 48000;
					ma_sound s1;
					ma_audio_buffer_init(&bc1, &buf1);
					ma_sound_init_from_data_source(&eng,
						(ma_data_source*)&buf1,
									MA_SOUND_FLAG_NO_PITCH, &grp1, &s1);
					ma_sound_set_looping(&s1, MA_TRUE); // #1 NEVER ends
					ma_sound_start(&s1);
					float out[2048];
					for (int i = 0; i < 6; ++i) // loops throughout
						ma_engine_read_pcm_frames(&eng, out, 1024,
							nullptr);
					// EXPLICIT stop (never atEnd), then the teardown.
					ma_sound_stop(&s1);
					if (uninitFirst) {
						ma_sound_uninit(&s1);
						ma_audio_buffer_uninit(&buf1);
						// THE WORKAROUND PROBE: destroy the starved
						// group BEFORE #2 attaches — the dead
						// attachment leaves the endpoint bus entirely.
						ma_sound_group_uninit(&grp1);
					}
					// #2 through its own (fresh) group.
					ma_sound s2;
					if (ma_sound_init_from_data_source(&eng,
							(ma_data_source*)&buf1,
										MA_SOUND_FLAG_NO_PITCH, &grp2, &s2)
							!= MA_SUCCESS)
						return -1.0f;
					ma_sound_start(&s2);
					float peak = 0.0f;
					for (int i = 0; i < 4; ++i) {
						ma_engine_read_pcm_frames(&eng, out, 1024,
							nullptr);
						for (float v : out)
							peak = std::max(peak, std::fabs(v));
					}
					ma_sound_uninit(&s2);
					if (!uninitFirst) {
						ma_sound_uninit(&s1);
						ma_audio_buffer_uninit(&buf1);
						ma_sound_group_uninit(&grp1); // still alive
					}
					ma_sound_group_uninit(&grp2);
					return peak;
				};
				const float pa = runSeq(true);
				const float pb = runSeq(false);
				std::cout << "  (raw-seq uninit=" << pa
					<< " keep=" << pb << ")\n";
				Check(pa > 0.1f, "ag.bus.raw-seq-uninit");
				Check(pb > 0.1f, "ag.bus.raw-seq-keep");
				// (f2) THE KEEP-ALIVE WORKAROUND PROBE: an infinite
				// looping ZERO-volume sound pinned to the endpoint for
				// (f2) THE SHARED-CURSOR TRAP + THE FIX (the root-cause
				// pin): a second sound wrapping the SAME exhausted
				// ma_audio_buffer reads silence (one buffer = one
				// cursor, exhausted by the first play) — while a
				// PER-VOICE ma_audio_buffer_ref over the same bytes
				// plays fine. This is exactly the engine's per-voice
				// cursor design.
				{
					std::vector<float> blip(4800);
					for (size_t i = 0; i < blip.size() / 2; ++i) {
						const float v = 0.9f
							* std::sin(2.0f * 3.14159265f * 700.0f
								* float(i) / 48000.0f);
						blip[i * 2] = v;
						blip[i * 2 + 1] = v;
					}
					ma_audio_buffer buf;
					ma_audio_buffer_config bc =
						ma_audio_buffer_config_init(ma_format_f32, 2,
							blip.size() / 2, blip.data(), nullptr);
					bc.sampleRate = 48000;
					ma_audio_buffer_init(&bc, &buf);
					ma_sound_group grp;
					ma_sound_group_init(&eng, 0, nullptr, &grp);
					ma_sound s1;
					ma_sound_init_from_data_source(&eng,
						(ma_data_source*)&buf, 0, &grp, &s1);
					ma_sound_start(&s1);
					float out[2048];
					for (int i = 0; i < 6; ++i) // s1 plays to its end
						ma_engine_read_pcm_frames(&eng, out, 1024,
							nullptr);
					ma_sound_uninit(&s1);
					// (a) The TRAP: a second sound over the SAME
					// buffer reads it exhausted -> silence.
					float trapPeak = 0.0f;
					{
						ma_sound s2;
						ma_sound_init_from_data_source(&eng,
							(ma_data_source*)&buf, 0, &grp, &s2);
						ma_sound_start(&s2);
						for (int i = 0; i < 4; ++i) {
							ma_engine_read_pcm_frames(&eng, out, 1024,
								nullptr);
							for (float v : out)
								trapPeak = std::max(trapPeak,
									std::fabs(v));
						}
						ma_sound_uninit(&s2);
					}
					// (b) THE FIX: a fresh ma_audio_buffer_ref (the
					// per-voice cursor) over the same bytes.
					float fixPeak = 0.0f;
					{
						ma_audio_buffer_ref ref;
						ma_audio_buffer_ref_init(ma_format_f32, 2,
							blip.data(), blip.size() / 2, &ref);
						ma_sound s3;
						ma_sound_init_from_data_source(&eng,
							(ma_data_source*)&ref, 0, &grp, &s3);
						ma_sound_start(&s3);
						for (int i = 0; i < 4; ++i) {
							ma_engine_read_pcm_frames(&eng, out, 1024,
								nullptr);
							for (float v : out)
								fixPeak = std::max(fixPeak,
									std::fabs(v));
						}
						ma_sound_uninit(&s3);
						ma_audio_buffer_ref_uninit(&ref);
					}
					std::cout << "  (cursor trap=" << trapPeak
						<< " fix=" << fixPeak << ")\n";
					Check(trapPeak < 0.01f, "ag.bus.raw-cursor-trap");
					Check(fixPeak > 0.1f, "ag.bus.raw-cursor-fix");
					ma_audio_buffer_uninit(&buf);
					ma_sound_group_uninit(&grp);
				}
				// (f) THE CLOCK PROBE: does the engine's global time
				// advance across a sound's natural end?
				{
					ma_sound_group grp;
					ma_sound_group_init(&eng, 0, nullptr, &grp);
					std::vector<float> blip(4800);
					for (size_t i = 0; i < blip.size() / 2; ++i) {
						const float v = 0.9f
							* std::sin(2.0f * 3.14159265f * 700.0f
								* float(i) / 48000.0f);
						blip[i * 2] = v;
						blip[i * 2 + 1] = v;
					}
					ma_audio_buffer buf;
					ma_audio_buffer_config bc =
						ma_audio_buffer_config_init(ma_format_f32, 2,
							blip.size() / 2, blip.data(), nullptr);
					bc.sampleRate = 48000;
					ma_audio_buffer_init(&bc, &buf);
					ma_sound s1;
					ma_sound_init_from_data_source(&eng,
						(ma_data_source*)&buf, 0, &grp, &s1);
					ma_sound_start(&s1);
					float out[2048];
					const ma_uint64 t0 =
						ma_engine_get_time_in_pcm_frames(&eng);
					for (int i = 0; i < 6; ++i)
						ma_engine_read_pcm_frames(&eng, out, 1024,
							nullptr);
					const ma_uint64 t1 =
						ma_engine_get_time_in_pcm_frames(&eng);
					std::cout << "  (clock t0=" << t0 << " t1=" << t1
						<< " delta=" << (t1 - t0) << ")\n";
					ma_sound_uninit(&s1);
					ma_audio_buffer_uninit(&buf);
					ma_sound_group_uninit(&grp);
				}
			}
			// (d) The FULL bus: splitter + the wet-only Schroeder +
			// the wet gain — exactly BuildWorldBus's shape.
			{
				ma_node_graph* graph = ma_engine_get_node_graph(&eng);
				ma_sound_group group;
				ma_splitter_node split;
				tour::audio::SchroederReverbNode reverb;
				ma_sound_group wetGain;
				bool ok = true;
				{
					ma_sound_group_config gc =
						ma_sound_group_config_init_2(&eng);
					if (ma_sound_group_init_ex(&eng, &gc, &wetGain)
						!= MA_SUCCESS)
						ok = false;
				}
				if (ok) {
					tour::audio::SchroederReverbConfig rc;
					rc.channels = 2;
					rc.sampleRate = 48000;
					rc.wet = 1.0f;
					rc.wetOnly = true;
					if (!tour::audio::SchroederReverbInit(graph, rc,
							&reverb))
						ok = false;
					else
						ma_node_attach_output_bus(&reverb.base, 0,
							(ma_node*)&wetGain, 0);
				}
				if (ok) {
					ma_splitter_node_config sc =
						ma_splitter_node_config_init(2);
					sc.outputBusCount = 2;
					if (ma_splitter_node_init(graph, &sc, nullptr,
							&split) != MA_SUCCESS)
						ok = false;
					else {
						ma_node_attach_output_bus(&split.base, 0,
							ma_engine_get_endpoint(&eng), 0);
						ma_node_attach_output_bus(&split.base, 1,
							&reverb.base, 0);
					}
				}
				if (ok) {
					ma_sound_group_config gc =
						ma_sound_group_config_init_2(&eng);
					gc.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
					if (ma_sound_group_init_ex(&eng, &gc, &group)
						!= MA_SUCCESS)
						ok = false;
					else
						ma_node_attach_output_bus(
							(ma_node*)&group, 0, &split.base, 0);
				}
				if (!ok) {
					Check(false, "ag.bus.raw-full-build");
				}
				else {
					ma_audio_buffer buf;
					std::vector<float> sine2 = sine;
					ma_audio_buffer_config bc =
						ma_audio_buffer_config_init(ma_format_f32, 2,
							sine2.size() / 2, sine2.data(), nullptr);
					bc.sampleRate = 48000;
					ma_audio_buffer_init(&bc, &buf);
					ma_sound sound;
					if (ma_sound_init_from_data_source(&eng,
							(ma_data_source*)&buf,
							MA_SOUND_FLAG_NO_SPATIALIZATION, &group,
							&sound) != MA_SUCCESS) {
						Check(false, "ag.bus.raw-full-sound");
					}
					else {
						ma_sound_start(&sound);
						float out[2048];
						float peak = 0.0f;
						for (int i = 0; i < 8; ++i) {
							ma_engine_read_pcm_frames(&eng, out,
								1024, nullptr);
							for (float v : out)
								peak = std::max(peak, std::fabs(v));
						}
						Check(peak > 0.1f, "ag.bus.raw-full-audible");
						std::cout << "  (raw-full peak=" << peak
							<< ")\n";
						ma_sound_uninit(&sound);
					}
					ma_audio_buffer_uninit(&buf);
				}
				ma_node_detach_all_output_buses(
					(ma_node*)&group);
				ma_sound_group_uninit(&group);
				ma_node_detach_all_output_buses(&split.base);
				ma_splitter_node_uninit(&split, nullptr);
				ma_node_detach_all_output_buses(&reverb.base);
				tour::audio::SchroederReverbUninit(&reverb);
				ma_node_detach_all_output_buses((ma_node*)&wetGain);
				ma_sound_group_uninit(&wetGain);
			}
			ma_engine_uninit(&eng);
		}
	}
	AudioEngine audio;
	Config cfg{};
	cfg.deviceless = true;
	if (!audio.Init(cfg)) {
		Check(false, "ag.bus.engine-init");
		return;
	}
	Check(true, "ag.bus.engine-init");
	const std::vector<uint8_t> blipWav = MakeWavSine(700.0f, 0.05f,
		0.9f);
	const ClipHandle blip = audio.LoadWavFromMemory(blipWav.data(),
		blipWav.size());
	Check(blip.valid(), "ag.bus.clip");
	std::vector<float> buf(1024 * 2);

	// THE SHARED-CURSOR REGRESSION (the trap the raw pin above
	// documents, at the ENGINE level): two DIRECT one-shots of the
	// SAME clip around a natural end + reap — the second must ring
	// (the per-voice cursor fix; before it, the second read the
	// clip's exhausted buffer and played silence).
	{
		const auto d0 = audio.Play(blip, 1.0f, false, 1.0f,
			AudioRoute::Direct);
		(void)d0;
		float first = 0.0f;
		for (int i = 0; i < 6; ++i) { // the 50 ms blip ends + reaps
			audio.PumpOffline(buf.data(), 1024);
			first = std::max(first, RmsOf(buf));
		}
		const auto d1 = audio.Play(blip, 1.0f, false, 1.0f,
			AudioRoute::Direct);
		(void)d1;
		float second = 0.0f;
		for (int i = 0; i < 4; ++i) {
			audio.PumpOffline(buf.data(), 1024);
			second = std::max(second, RmsOf(buf));
		}
		Check(first > 0.05f && second > 0.05f,
			"ag.bus.second-play-rings");
	}
	// SEQUENTIAL WORLD voices around a natural end + reap (the same
	// regression through the environmental route), then a direct
	// voice after everything — every later voice must ring.
	{
		const auto v0 = audio.Play(blip, 1.0f, false, 1.0f,
			AudioRoute::WorldSfx);
		float e0 = 0.0f;
		for (int i = 0; i < 4; ++i) {
			audio.PumpOffline(buf.data(), 1024);
			e0 = std::max(e0, RmsOf(buf));
		}
		Check(v0.valid() && e0 > 0.05f, "ag.bus.world-first-rings");
		// A SECOND world voice after the first's natural end.
		float e1 = 0.0f;
		{
			const auto v1 = audio.Play(blip, 1.0f, false, 1.0f,
				AudioRoute::WorldSfx);
			for (int i = 0; i < 4; ++i) {
				audio.PumpOffline(buf.data(), 1024);
				e1 = std::max(e1, RmsOf(buf));
			}
			audio.Stop(v1);
		}
		Check(e1 > 0.05f, "ag.bus.world-second-rings");
		// Let everything drain, then a DIRECT voice.
		for (int i = 0; i < 8; ++i)
			audio.PumpOffline(buf.data(), 1024);
		float e3 = 0.0f;
		{
			const auto v3 = audio.Play(blip, 1.0f, false, 1.0f,
				AudioRoute::Direct);
			for (int i = 0; i < 4; ++i) {
				audio.PumpOffline(buf.data(), 1024);
				e3 = std::max(e3, RmsOf(buf));
			}
			audio.Stop(v3);
		}
		Check(e3 > 0.05f, "ag.bus.direct-after-all-rings");
		for (int i = 0; i < 8; ++i)
			audio.PumpOffline(buf.data(), 1024);
	}

	// DIRECT: a one-shot is dead well past its 50 ms dry span (no
	// send, no tail).
	const auto voiceDirect = audio.Play(blip, 1.0f, false, 1.0f,
		AudioRoute::Direct);
	Check(voiceDirect.valid(), "ag.bus.direct-play");
	float directLate = 0.0f;
	float directEarly = 0.0f;
	for (int i = 0; i < 40; ++i) {
		audio.PumpOffline(buf.data(), 1024);
		if (i < 3)
			directEarly = std::max(directEarly, RmsOf(buf));
		if (i >= 8)
			directLate = std::max(directLate, RmsOf(buf));
	}
	std::cout << "  (direct early=" << directEarly << ")\n";
	Check(directLate < 1e-5f, "ag.bus.direct-has-no-tail");

	// The environmental target: a strong send.
	audio.SetEnvironmentReverb(EnvironmentReverb{ 0.9f, 2.0f, 0.5f });
	// (One boundary for the command to land + the smoothing to
	// engage.)
	audio.PumpOffline(buf.data(), 1024);

	// WORLDSFX: the same one-shot, routed through the shared send —
	// its tail RINGS past the dry span (the listener-in-room sound).
	const auto voiceWorld = audio.Play(blip, 1.0f, false, 1.0f,
		AudioRoute::WorldSfx);
	Check(voiceWorld.valid(), "ag.bus.world-play");
	audio.PumpOffline(buf.data(), 1024);
	std::cout << "  (world playing after 1 pump="
		<< (audio.IsVoicePlaying(voiceWorld) ? 1 : 0)
		<< " rms=" << RmsOf(buf) << ")\n";
	float worldLate = 0.0f;
	for (int i = 0; i < 39; ++i) {
		audio.PumpOffline(buf.data(), 1024);
		if (i >= 8)
			worldLate = std::max(worldLate, RmsOf(buf));
	}
	Check(worldLate > 1e-3f, "ag.bus.world-tail-outlives-dry");
	std::cout << "  (bus late rms: direct=" << directLate
		<< " world=" << worldLate << ")\n";

	// Wet to zero: the send silences (the dry path itself is never
	// attenuated — the gainer rides the WET branch only).
	audio.SetEnvironmentReverb(EnvironmentReverb{ 0.0f, 2.0f, 0.5f });
	audio.PumpOffline(buf.data(), 1024);
	const auto voiceWorld2 = audio.Play(blip, 1.0f, false, 1.0f,
		AudioRoute::WorldSfx);
	(void)voiceWorld2;
	float wet0Late = 0.0f;
	float wet0Early = 0.0f;
	for (int i = 0; i < 40; ++i) {
		audio.PumpOffline(buf.data(), 1024);
		if (i < 3)
			wet0Early = std::max(wet0Early, RmsOf(buf));
		if (i >= 8)
			wet0Late = std::max(wet0Late, RmsOf(buf));
	}
	// The dry path stays FULL (comparable to the direct route's early
	// chunks) while the tail dies like a direct voice.
	Check(wet0Early > 0.05f, "ag.bus.wet0-dry-preserved");
	Check(wet0Late < 1e-3f, "ag.bus.wet0-no-tail");

	// The DEFAULT route stays Direct (the compat contract: every
	// pre-room caller keeps its exact old behavior). The play lands at
	// the next boundary — pump once, then the voice is live AND the
	// mix carries it.
	const auto voiceDefault = audio.Play(blip, 1.0f);
	audio.PumpOffline(buf.data(), 1024);
	Check(voiceDefault.valid()
			&& audio.IsVoicePlaying(voiceDefault)
			&& RmsOf(buf) > 0.05f,
		"ag.bus.default-route-works");

	audio.Shutdown();
	Check(true, "ag.bus.shutdown");
}

} // namespace

int main() {
#ifdef _MSC_VER
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::cout << "[audiograph] begin\n";
	TestParse();
	std::cout << "[audiograph] parse done\n";
	TestInstances();
	std::cout << "[audiograph] instances done\n";
	TestReverb();
	std::cout << "[audiograph] reverb done\n";
	TestWorldBus();
	std::cout << "[audiograph] bus done\n";
	std::cout << "checks=" << g_checks << " failures=" << g_failures
		<< "\n";
	return g_failures == 0 ? 0 : 1;
}
