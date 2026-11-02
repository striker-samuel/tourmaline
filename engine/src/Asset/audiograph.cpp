// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/audiograph.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace tour::asset {

namespace {

// ---- tokenization ---------------------------------------------------------
// Verbs carry -key value tokens; string values are QUOTED (and may not
// contain quotes). The reader stops at the CLOSING quote so later
// flags on the same line still parse (the .tourAnim lesson).
struct Tokens {
	std::vector<std::string> items; // flat token stream (keys + values)
};

bool TokenizeLine(const std::string& line, Tokens& out) {
	out.items.clear();
	size_t i = 0;
	const size_t n = line.size();
	while (i < n) {
		while (i < n && std::isspace(static_cast<unsigned char>(line[i])))
			++i;
		if (i >= n)
			break;
		if (line[i] == '"') {
			const size_t close = line.find('"', i + 1);
			if (close == std::string::npos)
				return false; // unterminated quote
			out.items.push_back(line.substr(i + 1, close - i - 1));
			i = close + 1;
		} else {
			size_t j = i;
			while (j < n && !std::isspace(static_cast<unsigned char>(line[j])))
				++j;
			out.items.push_back(line.substr(i, j - i));
			i = j;
		}
	}
	return true;
}

// Reads the value token AFTER key at index i; advances i past it.
bool ReadString(const Tokens& t, size_t& i, std::string& out) {
	if (i + 1 >= t.items.size())
		return false;
	out = t.items[i + 1];
	i += 2;
	return true;
}

bool ReadFloat(const Tokens& t, size_t& i, float& out) {
	std::string s;
	if (!ReadString(t, i, s) || s.empty())
		return false;
	try {
		size_t pos = 0;
		out = std::stof(s, &pos);
		return pos == s.size();
	} catch (...) {
		return false;
	}
}

bool ReadDouble(const Tokens& t, size_t& i, double& out) {
	std::string s;
	if (!ReadString(t, i, s) || s.empty())
		return false;
	try {
		size_t pos = 0;
		out = std::stod(s, &pos);
		return pos == s.size();
	} catch (...) {
		return false;
	}
}

bool ReadUint(const Tokens& t, size_t& i, uint32_t& out) {
	std::string s;
	if (!ReadString(t, i, s) || s.empty())
		return false;
	try {
		size_t pos = 0;
		out = static_cast<uint32_t>(std::stoul(s, &pos));
		return pos == s.size();
	} catch (...) {
		return false;
	}
}

bool ReadBool(const Tokens& t, size_t& i, bool& out) {
	std::string s;
	if (!ReadString(t, i, s))
		return false;
	if (s == "on" || s == "true") {
		out = true;
		return true;
	}
	if (s == "off" || s == "false") {
		out = false;
		return true;
	}
	return false;
}

bool ReadNodeType(const Tokens& t, size_t& i, AudioNodeType& out) {
	std::string s;
	if (!ReadString(t, i, s))
		return false;
	if (s == "source") { out = AudioNodeType::Source; return true; }
	if (s == "gain") { out = AudioNodeType::Gain; return true; }
	if (s == "lpf") { out = AudioNodeType::LowPass; return true; }
	if (s == "hpf") { out = AudioNodeType::HighPass; return true; }
	if (s == "bpf") { out = AudioNodeType::BandPass; return true; }
	if (s == "notch") { out = AudioNodeType::Notch; return true; }
	if (s == "peak") { out = AudioNodeType::Peak; return true; }
	if (s == "loshelf") { out = AudioNodeType::LowShelf; return true; }
	if (s == "hishelf") { out = AudioNodeType::HighShelf; return true; }
	if (s == "delay") { out = AudioNodeType::Delay; return true; }
	if (s == "reverb") { out = AudioNodeType::Reverb; return true; }
	return false;
}

bool ReadParamField(const Tokens& t, size_t& i, AudioParamField& out) {
	std::string s;
	if (!ReadString(t, i, s))
		return false;
	if (s == "gain") { out = AudioParamField::Gain; return true; }
	if (s == "cutoff") { out = AudioParamField::Cutoff; return true; }
	if (s == "frequency") { out = AudioParamField::Frequency; return true; }
	if (s == "q") { out = AudioParamField::Q; return true; }
	if (s == "gainDb") { out = AudioParamField::GainDb; return true; }
	if (s == "decay") { out = AudioParamField::Decay; return true; }
	if (s == "wet") { out = AudioParamField::Wet; return true; }
	if (s == "rt60") { out = AudioParamField::Rt60; return true; }
	if (s == "damping") { out = AudioParamField::Damping; return true; }
	return false;
}

std::string At(uint32_t lineNo) {
	return "line " + std::to_string(lineNo) + ": ";
}

} // namespace

int AudioGraphFindNode(const AudioGraphDoc& doc, const std::string& name) {
	for (size_t i = 0; i < doc.nodes.size(); ++i)
		if (doc.nodes[i].name == name)
			return static_cast<int>(i);
	return -1;
}

int AudioGraphFindParam(const AudioGraphDoc& doc, const std::string& name) {
	for (size_t i = 0; i < doc.params.size(); ++i)
		if (doc.params[i].name == name)
			return static_cast<int>(i);
	return -1;
}

int AudioGraphFindCue(const AudioGraphDoc& doc, const std::string& name) {
	for (size_t i = 0; i < doc.cues.size(); ++i)
		if (doc.cues[i].name == name)
			return static_cast<int>(i);
	return -1;
}

bool AudioGraphNodeFieldValid(AudioNodeType type, AudioParamField field) {
	switch (type) {
	case AudioNodeType::Source:
		return field == AudioParamField::Gain;
	case AudioNodeType::Gain:
		return field == AudioParamField::Gain;
	case AudioNodeType::LowPass:
	case AudioNodeType::HighPass:
	case AudioNodeType::BandPass:
		// The stock biquad lpf/hpf/bpf expose cutoff + ORDER only (no
		// resonance knob — the miniaudio filter shape); the parametric
		// Q lives on notch/peak/shelf.
		return field == AudioParamField::Cutoff;
	case AudioNodeType::Notch:
		return field == AudioParamField::Frequency
			|| field == AudioParamField::Q;
	case AudioNodeType::Peak:
	case AudioNodeType::LowShelf:
	case AudioNodeType::HighShelf:
		return field == AudioParamField::Frequency
			|| field == AudioParamField::Q
			|| field == AudioParamField::GainDb;
	case AudioNodeType::Delay:
		// The delay LENGTH is static in v1 (the ma_delay layout is sized
		// at build); the feedback blend is live.
		return field == AudioParamField::Decay;
	case AudioNodeType::Reverb:
		// The delay lines are fixed at build; the mix, the decay time
		// and the comb damping are live (smoothed on the mixer thread).
		return field == AudioParamField::Wet
			|| field == AudioParamField::Rt60
			|| field == AudioParamField::Damping;
	}
	return false;
}

bool ParseAudioGraphText(const std::string& text, AudioGraphDoc& out,
	std::string& err, std::vector<std::string>* warnings) {
	auto warn = [&](const std::string& m) {
		if (warnings)
			warnings->push_back(m);
	};
	out = AudioGraphDoc{};
	err.clear();

	std::istringstream in(text);
	std::string line;
	uint32_t lineNo = 0;
	bool sawVersion = false;
	AudioCue* currentCue = nullptr;

	// Pending cue param keyframes keyed by param name (each set/ramp
	// appends; tracks are materialized at the next cue/EOF).
	std::unordered_map<std::string, std::vector<AudioCueKeyframe>> cueKeys;

	auto flushCueTracks = [&]() {
		if (!currentCue)
			return;
		// Preserve exposed-param declaration order, not the unspecified
		// iteration order of unordered_map (which differs across STL
		// implementations and changes the public cue track ordering).
		for (const AudioGraphParam& exposed : out.params) {
			auto it = cueKeys.find(exposed.name);
			if (it == cueKeys.end())
				continue;
			auto& keys = it->second;
			std::sort(keys.begin(), keys.end(),
				[](const AudioCueKeyframe& a, const AudioCueKeyframe& b) {
					return a.t < b.t;
				});
			AudioCueParamTrack track;
			track.param = exposed.name;
			track.keys = std::move(keys);
			currentCue->tracks.push_back(std::move(track));
		}
		cueKeys.clear();
	};

	while (std::getline(in, line)) {
		++lineNo;
		// Strip comments ("#" to EOL — NOT "//": sound paths and names
		// are quoted, but "#" never appears in authored values) + trim.
		const size_t hash = line.find('#');
		if (hash != std::string::npos)
			line = line.substr(0, hash);
		const size_t b = line.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			continue;
		const size_t e = line.find_last_not_of(" \t\r\n");
		line = line.substr(b, e - b + 1);

		Tokens tok;
		if (!TokenizeLine(line, tok)) {
			err = At(lineNo) + "unterminated quoted string";
			return false;
		}
		if (tok.items.empty())
			continue;
		const std::string& verb = tok.items[0];

		if (verb == "version") {
			// items = ["version", "1"] — no flag tokens on this line.
			if (tok.items.size() != 2 || tok.items[1] != "1") {
				err = At(lineNo) + "expected 'version 1' (the only format "
					"version; no compat readers by policy)";
				return false;
			}
			sawVersion = true;
			continue;
		}

		if (!sawVersion) {
			err = At(lineNo) + "the file must open with 'version 1'";
			return false;
		}

		if (verb == "node") {
			currentCue = nullptr; // a node line closes the cue block
			flushCueTracks();
			AudioGraphNode node;
			bool haveName = false, haveType = false, haveSound = false;
			size_t i = 1;
			while (i < tok.items.size()) {
				const std::string& key = tok.items[i];
				if (key == "-name") {
					if (!ReadString(tok, i, node.name)) {
						err = At(lineNo) + "-name needs a quoted value";
						return false;
					}
					haveName = true;
				} else if (key == "-type") {
					if (!ReadNodeType(tok, i, node.type)) {
						err = At(lineNo) + "unknown -type (source|gain|lpf|"
							"hpf|bpf|notch|peak|loshelf|hishelf|delay)";
						return false;
					}
					haveType = true;
				} else if (key == "-sound") {
					if (!ReadString(tok, i, node.sound) || node.sound.empty()) {
						err = At(lineNo) + "-sound needs a quoted VFS path";
						return false;
					}
					haveSound = true;
				} else if (key == "-loop") {
					if (!ReadBool(tok, i, node.loop)) {
						err = At(lineNo) + "-loop needs on|off";
						return false;
					}
				} else if (key == "-gain") {
					if (!ReadFloat(tok, i, node.gain)) {
						err = At(lineNo) + "-gain needs a number";
						return false;
					}
				} else if (key == "-cutoff") {
					if (!ReadFloat(tok, i, node.cutoff)) {
						err = At(lineNo) + "-cutoff needs a number";
						return false;
					}
				} else if (key == "-frequency") {
					if (!ReadFloat(tok, i, node.frequency)) {
						err = At(lineNo) + "-frequency needs a number";
						return false;
					}
				} else if (key == "-q") {
					if (!ReadFloat(tok, i, node.q)) {
						err = At(lineNo) + "-q needs a number";
						return false;
					}
				} else if (key == "-gainDb") {
					if (!ReadFloat(tok, i, node.gainDb)) {
						err = At(lineNo) + "-gainDb needs a number";
						return false;
					}
				} else if (key == "-order") {
					if (!ReadUint(tok, i, node.order)) {
						err = At(lineNo) + "-order needs an integer";
						return false;
					}
				} else if (key == "-delay") {
					if (!ReadFloat(tok, i, node.delaySeconds)) {
						err = At(lineNo) + "-delay needs seconds";
						return false;
					}
				} else if (key == "-decay") {
					if (!ReadFloat(tok, i, node.decay)) {
						err = At(lineNo) + "-decay needs a number";
						return false;
					}
				} else if (key == "-wet") {
					if (!ReadFloat(tok, i, node.reverbWet)) {
						err = At(lineNo) + "-wet needs a number";
						return false;
					}
				} else if (key == "-rt60") {
					if (!ReadFloat(tok, i, node.reverbRt60)) {
						err = At(lineNo) + "-rt60 needs seconds";
						return false;
					}
				} else if (key == "-damping") {
					if (!ReadFloat(tok, i, node.reverbDamping)) {
						err = At(lineNo) + "-damping needs a number";
						return false;
					}
				} else {
					err = At(lineNo) + "unknown node flag '" + key + "'";
					return false;
				}
			}
			if (!haveName || node.name.empty()) {
				err = At(lineNo) + "node needs -name";
				return false;
			}
			if (!haveType) {
				err = At(lineNo) + "node '" + node.name + "' needs -type";
				return false;
			}
			if (node.name == kAudioGraphOutput) {
				err = At(lineNo) + "'" + kAudioGraphOutput
					+ "' is a reserved edge target, not a node name";
				return false;
			}
			if (node.type == AudioNodeType::Source && !haveSound) {
				err = At(lineNo) + "source node '" + node.name
					+ "' needs -sound (a quoted VFS path)";
				return false;
			}
			if (node.type == AudioNodeType::LowPass
				|| node.type == AudioNodeType::HighPass
				|| node.type == AudioNodeType::BandPass) {
				if (node.order < 1 || node.order > 8) {
					err = At(lineNo) + "node '" + node.name
						+ "': -order must be 1..8";
					return false;
				}
			}
			if (node.type == AudioNodeType::Delay) {
				// nothing further in v1 (the length is the shape)
			}
			if (node.type == AudioNodeType::Source && node.gain < 0.0f) {
				err = At(lineNo) + "source node '" + node.name
					+ "': -gain must be >= 0";
				return false;
			}
			if (node.type == AudioNodeType::Reverb) {
				// The bounded reverb shape (the runtime clamps again —
				// the parser is the FIRST gate, the DSP the last).
				if (!(node.reverbWet >= 0.0f && node.reverbWet <= 1.0f)) {
					err = At(lineNo) + "reverb node '" + node.name
						+ "': -wet must be 0..1";
					return false;
				}
				if (!(node.reverbRt60 >= 0.15f && node.reverbRt60 <= 8.0f)) {
					err = At(lineNo) + "reverb node '" + node.name
						+ "': -rt60 must be 0.15..8 seconds";
					return false;
				}
				if (!(node.reverbDamping >= 0.0f
						&& node.reverbDamping <= 1.0f)) {
					err = At(lineNo) + "reverb node '" + node.name
						+ "': -damping must be 0..1";
					return false;
				}
			}
			if (AudioGraphFindNode(out, node.name) >= 0) {
				err = At(lineNo) + "duplicate node name '" + node.name + "'";
				return false;
			}
			out.nodes.push_back(std::move(node));
			continue;
		}

		if (verb == "connect") {
			currentCue = nullptr;
			flushCueTracks();
			std::string from, to;
			bool haveFrom = false, haveTo = false;
			size_t i = 1;
			while (i < tok.items.size()) {
				const std::string& key = tok.items[i];
				if (key == "-from") {
					if (!ReadString(tok, i, from) || from.empty()) {
						err = At(lineNo) + "-from needs a quoted node name";
						return false;
					}
					haveFrom = true;
				} else if (key == "-to") {
					if (!ReadString(tok, i, to) || to.empty()) {
						err = At(lineNo) + "-to needs a quoted node name";
						return false;
					}
					haveTo = true;
				} else {
					err = At(lineNo) + "unknown connect flag '" + key + "'";
					return false;
				}
			}
			if (!haveFrom || !haveTo) {
				err = At(lineNo) + "connect needs -from and -to";
				return false;
			}
			if (from == kAudioGraphOutput) {
				err = At(lineNo) + "'output' cannot be an edge SOURCE (it "
					"is the graph exit)";
				return false;
			}
			if (AudioGraphFindNode(out, from) < 0) {
				err = At(lineNo) + "connect -from '" + from
					+ "' names no declared node";
				return false;
			}
			if (to != kAudioGraphOutput
				&& AudioGraphFindNode(out, to) < 0) {
				err = At(lineNo) + "connect -to '" + to
					+ "' names no declared node (or 'output')";
				return false;
			}
			if (from == to) {
				err = At(lineNo) + "self-edge on node '" + from + "'";
				return false;
			}
			out.edges.push_back({ from, to });
			continue;
		}

		if (verb == "param") {
			currentCue = nullptr;
			flushCueTracks();
			AudioGraphParam p;
			bool haveName = false, haveNode = false, haveField = false;
			size_t i = 1;
			while (i < tok.items.size()) {
				const std::string& key = tok.items[i];
				if (key == "-name") {
					if (!ReadString(tok, i, p.name) || p.name.empty()) {
						err = At(lineNo) + "-name needs a quoted value";
						return false;
					}
					haveName = true;
				} else if (key == "-node") {
					if (!ReadString(tok, i, p.node) || p.node.empty()) {
						err = At(lineNo) + "-node needs a quoted node name";
						return false;
					}
					haveNode = true;
				} else if (key == "-field") {
					if (!ReadParamField(tok, i, p.field)) {
						err = At(lineNo) + "unknown -field (gain|cutoff|"
							"frequency|q|gainDb|decay)";
						return false;
					}
					haveField = true;
				} else {
					err = At(lineNo) + "unknown param flag '" + key + "'";
					return false;
				}
			}
			if (!haveName || !haveNode || !haveField) {
				err = At(lineNo)
					+ "param needs -name, -node and -field";
				return false;
			}
			const int ni = AudioGraphFindNode(out, p.node);
			if (ni < 0) {
				err = At(lineNo) + "param '" + p.name + "' -node '" + p.node
					+ "' names no declared node";
				return false;
			}
			if (!AudioGraphNodeFieldValid(out.nodes[ni].type, p.field)) {
				err = At(lineNo) + "param '" + p.name + "': field not valid "
					"for node '" + p.node + "'s type";
				return false;
			}
			if (AudioGraphFindParam(out, p.name) >= 0) {
				err = At(lineNo) + "duplicate param name '" + p.name + "'";
				return false;
			}
			out.params.push_back(std::move(p));
			continue;
		}

		if (verb == "cue") {
			flushCueTracks();
			std::string name;
			bool haveName = false;
			size_t i = 1;
			while (i < tok.items.size()) {
				const std::string& key = tok.items[i];
				if (key == "-name") {
					if (!ReadString(tok, i, name) || name.empty()) {
						err = At(lineNo) + "-name needs a quoted value";
						return false;
					}
					haveName = true;
				} else {
					err = At(lineNo) + "unknown cue flag '" + key + "'";
					return false;
				}
			}
			if (!haveName) {
				err = At(lineNo) + "cue needs -name";
				return false;
			}
			if (AudioGraphFindCue(out, name) >= 0) {
				err = At(lineNo) + "duplicate cue name '" + name + "'";
				return false;
			}
			out.cues.emplace_back();
			out.cues.back().name = name;
			currentCue = &out.cues.back();
			continue;
		}

		if (verb == "start" || verb == "stop") {
			if (!currentCue) {
				err = At(lineNo) + verb
					+ " outside any cue block (a cue line opens one)";
				return false;
			}
			AudioCueEvent ev;
			ev.type = verb == "start" ? AudioCueEvent::Type::StartSource
				: AudioCueEvent::Type::StopSource;
			bool haveNode = false, haveT = false;
			size_t i = 1;
			while (i < tok.items.size()) {
				const std::string& key = tok.items[i];
				if (key == "-node") {
					if (!ReadString(tok, i, ev.node) || ev.node.empty()) {
						err = At(lineNo) + "-node needs a quoted node name";
						return false;
					}
					haveNode = true;
				} else if (key == "-t") {
					if (!ReadDouble(tok, i, ev.t) || ev.t < 0.0) {
						err = At(lineNo) + "-t needs seconds >= 0";
						return false;
					}
					haveT = true;
				} else {
					err = At(lineNo) + "unknown " + verb + " flag '" + key
						+ "'";
					return false;
				}
			}
			if (!haveNode || !haveT) {
				err = At(lineNo) + verb + " needs -node and -t";
				return false;
			}
			const int ni = AudioGraphFindNode(out, ev.node);
			if (ni < 0) {
				err = At(lineNo) + verb + " -node '" + ev.node
					+ "' names no declared node";
				return false;
			}
			if (out.nodes[ni].type != AudioNodeType::Source) {
				err = At(lineNo) + verb + " -node '" + ev.node
					+ "' is not a source node";
				return false;
			}
			currentCue->events.push_back(std::move(ev));
			continue;
		}

		if (verb == "set" || verb == "ramp") {
			if (!currentCue) {
				err = At(lineNo) + verb
					+ " outside any cue block (a cue line opens one)";
				return false;
			}
			std::string param;
			double t = 0.0;
			float value = 0.0f;
			bool haveParam = false, haveT = false, haveValue = false;
			size_t i = 1;
			while (i < tok.items.size()) {
				const std::string& key = tok.items[i];
				if (key == "-param") {
					if (!ReadString(tok, i, param) || param.empty()) {
						err = At(lineNo) + "-param needs a quoted name";
						return false;
					}
					haveParam = true;
				} else if (key == "-t") {
					if (!ReadDouble(tok, i, t) || t < 0.0) {
						err = At(lineNo) + "-t needs seconds >= 0";
						return false;
					}
					haveT = true;
				} else if (key == "-value") {
					if (!ReadFloat(tok, i, value)) {
						err = At(lineNo) + "-value needs a number";
						return false;
					}
					haveValue = true;
				} else {
					err = At(lineNo) + "unknown " + verb + " flag '" + key
						+ "'";
					return false;
				}
			}
			if (!haveParam || !haveT || !haveValue) {
				err = At(lineNo) + verb + " needs -param, -t and -value";
				return false;
			}
			if (AudioGraphFindParam(out, param) < 0) {
				err = At(lineNo) + verb + " -param '" + param
					+ "' names no exposed param (param lines declare them)";
				return false;
			}
			AudioCueKeyframe k{};
			k.t = t;
			k.value = value;
			k.linear = verb == "ramp";
			// Keyframes are STRICTLY ascending per param — a
			// non-ascending time is an authoring error, caught at
			// insertion (the flush-time sort is only a safety net).
			auto& keys = cueKeys[param];
			if (!keys.empty() && !(k.t > keys.back().t)) {
				err = At(lineNo) + "non-ascending keyframe time on "
					"param '" + param + "'";
				return false;
			}
			keys.push_back(k);
			continue;
		}

		err = At(lineNo) + "unrecognized verb '" + verb + "'";
		return false;
	}
	flushCueTracks();

	// ---- structural validation ---------------------------------------------
	if (out.nodes.empty()) {
		err = "no nodes declared";
		return false;
	}
	if (out.cues.empty()) {
		err = "no cues declared (a graph needs at least its default cue)";
		return false;
	}

	// Every node must reach "output" (a dangling branch renders
	// nowhere; graphs are explicit about their exit).
	{
		std::unordered_map<std::string, std::vector<std::string>> adj;
		for (const auto& e : out.edges)
			adj[e.from].push_back(e.to);
		// Iterative reachability from "output" backwards: collect the
		// set of nodes with a path TO output.
		std::vector<std::string> stack;
		std::unordered_set<std::string> reachable;
		reachable.insert(kAudioGraphOutput);
		for (const auto& e : out.edges)
			if (e.to == kAudioGraphOutput)
				stack.push_back(e.from);
		while (!stack.empty()) {
			const std::string cur = stack.back();
			stack.pop_back();
			if (reachable.count(cur))
				continue;
			reachable.insert(cur);
			for (const auto& e : out.edges)
				if (e.to == cur)
					stack.push_back(e.from);
		}
		for (const auto& n : out.nodes) {
			if (!reachable.count(n.name)) {
				err = "node '" + n.name
					+ "' does not reach 'output' (every node must connect, "
					"directly or transitively, to the graph exit)";
				return false;
			}
			if (!adj.count(n.name)) {
				err = "node '" + n.name + "' has no outgoing edge";
				return false;
			}
		}
	}

	// Cycle check (feedback is INSIDE the delay node's decay; graph
	// cycles are degenerate — a filter chain that never reaches output
	// through acyclic edges).
	{
		// Kahn's algorithm over the node graph; "output" is the sink.
		std::unordered_map<std::string, int> indeg;
		std::unordered_map<std::string, std::vector<std::string>> succ;
		std::unordered_set<std::string> names;
		for (const auto& n : out.nodes) {
			indeg[n.name] = 0;
			names.insert(n.name);
		}
		for (const auto& e : out.edges) {
			succ[e.from].push_back(e.to);
			if (e.to != kAudioGraphOutput)
				++indeg[e.to];
		}
		std::vector<std::string> ready;
		for (const auto& n : out.nodes)
			if (indeg[n.name] == 0)
				ready.push_back(n.name);
		size_t visited = 0;
		while (!ready.empty()) {
			const std::string cur = ready.back();
			ready.pop_back();
			++visited;
			for (const auto& s : succ[cur]) {
				if (s == kAudioGraphOutput)
					continue;
				if (--indeg[s] == 0)
					ready.push_back(s);
			}
		}
		if (visited != out.nodes.size()) {
			err = "the node graph contains a cycle (feedback belongs INSIDE "
				"a delay node's -decay, never in the edge topology)";
			return false;
		}
	}

	// Cue sanity: sources must be referenced consistently (warnings,
	// not errors — a cue with no start events is a "silent" cue, legal
	// for pure-param interludes; duplicate start times on the same
	// source are a warn).
	for (auto& cue : out.cues) {
		std::sort(cue.events.begin(), cue.events.end(),
			[](const AudioCueEvent& a, const AudioCueEvent& b) {
				return a.t < b.t;
			});
		for (const auto& tr : cue.tracks) {
			if (tr.keys.empty())
				continue;
			for (size_t k = 1; k < tr.keys.size(); ++k) {
				if (tr.keys[k].t <= tr.keys[k - 1].t) {
					err = "cue '" + cue.name + "': param '" + tr.param
						+ "' has non-ascending keyframe times";
					return false;
				}
			}
		}
		double endT = 0.0;
		for (const auto& ev : cue.events)
			endT = std::max(endT, ev.t);
		for (const auto& tr : cue.tracks)
			if (!tr.keys.empty())
				endT = std::max(endT, tr.keys.back().t);
		cue.endT = endT;
		if (cue.events.empty() && cue.tracks.empty()) {
			warn("cue '" + cue.name + "' has no events (it stays silent)");
		}
	}

	return true;
}

} // namespace tour::asset
