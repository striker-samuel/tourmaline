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
	std::cout << "checks=" << g_checks << " failures=" << g_failures
		<< "\n";
	return g_failures == 0 ? 0 : 1;
}
