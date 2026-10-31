// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// .tourAnim runtime: the ASCII clip loader + the spline sampler + the
// skin palette build (the header - Engine/Asset/touranim.h - carries
// the full format contract; the .anim WRITER lives in
// gltf_import/asset_export.cpp, which hand-rolls the documented format
// for the derivation).

#include <Engine/Asset/touranim.h>

#include <Engine/Scene/scene.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace tour::asset {

namespace {

bool ParseOnOff(const std::string& s) { return s == "1" || s == "on" || s == "true"; }

// Quat helpers on the KEY layout: v = (x, y, z, w) - the .tourAnim
// rotate channel stores UNIT QUATERNIONS xyzw.
glm::vec4 QuatNormalize(glm::vec4 q) {
	const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return l > 1e-12f ? q / l : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

glm::vec4 QuatNlerp(glm::vec4 a, glm::vec4 b, float t) {
	// Shortest path: flip b when it disagrees with a (the loader already
	// sign-fixed neighbors, so this only guards hand-tuned files).
	if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f)
		b = -b;
	glm::vec4 r = glm::mix(a, b, t);
	return QuatNormalize(r);
}

glm::vec4 CatmullRom4(const glm::vec4& p0, const glm::vec4& p1,
	const glm::vec4& p2, const glm::vec4& p3, float t) {
	// Uniform Catmull-Rom (componentwise; rotate keys renormalize
	// after - the header contract: fine at 30Hz authoring rates).
	const float t2 = t * t;
	const float t3 = t2 * t;
	return 0.5f * (2.0f * p1
		+ (-p0 + p2) * t
		+ (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
		+ (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

} // namespace

glm::vec4 SampleTrack(const Track& track, double t) {
	const std::vector<Key>& keys = track.keys;
	if (keys.empty())
		return glm::vec4(0.0f);
	if (keys.size() == 1 || t <= keys.front().t)
		return keys.front().v;
	if (t >= keys.back().t)
		return keys.back().v;
	// Binary search the span [i, i+1] with t inside.
	size_t lo = 0, hi = keys.size() - 1;
	while (hi - lo > 1) {
		const size_t mid = (lo + hi) / 2;
		if (keys[mid].t <= t)
			lo = mid;
		else
			hi = mid;
	}
	const Key& k0 = keys[lo];
	const Key& k1 = keys[hi];
	const float span = float(k1.t - k0.t);
	const float f = span > 1e-12f ? float(t - k0.t) / span : 0.0f;
	switch (track.spline) {
	case Spline::Step:
		return k0.v;
	case Spline::CatmullRom: {
		// First/last duplicated at the ends (the uniform CR contract).
		const Key& km1 = lo > 0 ? keys[lo - 1] : k0;
		const Key& k2 = hi + 1 < keys.size() ? keys[hi + 1] : k1;
		glm::vec4 v = CatmullRom4(km1.v, k0.v, k1.v, k2.v, f);
		if (track.channel == Channel::Rotate)
			v = QuatNormalize(v);
		return v;
	}
	case Spline::Linear:
	default:
		if (track.channel == Channel::Rotate)
			return QuatNlerp(k0.v, k1.v, f);
		return glm::mix(k0.v, k1.v, f);
	}
}

void BuildSkinPalette(const Scene& scene, const Skeleton& skeleton,
	std::vector<glm::mat4>& palette) {
	palette.assign(skeleton.Count(), glm::mat4(1.0f));
	for (uint32_t i = 0; i < skeleton.Count(); ++i) {
		const uint32_t node = skeleton.joints[i];
		if (node >= scene.globalTransform.size())
			continue;
		palette[i] = scene.globalTransform[node] * skeleton.inverseBind[i];
	}
}

// ---------------------------------------------------------------------------
// .tourAnim ASCII (Maya .ma flavor): see the header's grammar comment.
// ---------------------------------------------------------------------------

bool SaveTourAnim(const std::string& path, const AnimDoc& doc, std::string& err) {
	std::ostringstream o;
	o << "// .tourAnim " << kTourAnimVersion << " - " << doc.source << "\n";
	o << "source -p \"" << doc.source << "\";\n";
	for (const Clip& clip : doc.clips) {
		o << "clip -name \"" << clip.name << "\" -duration " << clip.duration
			<< " -fps " << clip.fps << ";\n";
		for (const Track& t : clip.tracks) {
			const char* attr = (t.channel == Channel::Rotate) ? "rotate"
				: (t.channel == Channel::Scale) ? "scale" : "translate";
			const char* interp = (t.spline == Spline::Step) ? "step"
				: (t.spline == Spline::CatmullRom) ? "catmullrom" : "linear";
			o << "track -joint " << t.joint;
			if (!t.name.empty())
				o << " -name \"" << t.name << "\"";
			o << " -attr " << attr << " -interp " << interp << ";\n";
			for (const Key& k : t.keys) {
				o << "  " << k.t << "  " << k.v.x << " " << k.v.y << " "
					<< k.v.z << " " << k.v.w << ";\n";
			}
		}
	}
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot open for write: " + path;
		return false;
	}
	const std::string s = o.str();
	f.write(s.data(), std::streamsize(s.size()));
	return bool(f);
}

bool LoadTourAnim(const std::string& path, AnimDoc& doc, std::string& err,
	std::vector<std::string>* warnings) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open: " + path;
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return LoadTourAnimText(ss.str(), doc, err, warnings);
}

bool LoadTourAnimText(const std::string& text, AnimDoc& doc, std::string& err,
	std::vector<std::string>* warnings) {
	auto warn = [&](const std::string& m) {
		if (warnings)
			warnings->push_back(m);
	};
	std::istringstream in(text);
	std::string line;
	doc = AnimDoc{};
	Clip* clip = nullptr;
	Track* track = nullptr;
	uint32_t lineNo = 0;
	while (std::getline(in, line)) {
		++lineNo;
		// Strip comments ("//" to EOL) + trim.
		const size_t at = line.rfind("//");
		if (at != std::string::npos)
			line = line.substr(0, at);
		size_t b = line.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			continue;
		const size_t e = line.find_last_not_of(" \t\r\n");
		line = line.substr(b, e - b + 1);
		if (line.empty())
			continue;
		// Verb lines end with ';' (the grammar): strip the terminator so
		// the LAST flag's STRING value doesn't carry it into the token
		// compare (observed: -interp "linear;" -> "unknown -interp"
		// warn-and-fallback, silently retyping step tracks to linear -
		// numeric tokens are unaffected: istream extraction stops at
		// the ';' anyway).
		if (line.back() == ';')
			line.pop_back();

		// source -p "<path>";
		if (line.rfind("source", 0) == 0) {
			const size_t q0 = line.find('"');
			const size_t q1 = q0 == std::string::npos
				? std::string::npos : line.find('"', q0 + 1);
			if (q0 != std::string::npos && q1 != std::string::npos)
				doc.source = line.substr(q0 + 1, q1 - q0 - 1);
			track = nullptr;
			continue;
		}
		// clip -name "<name>" -duration <d> -fps <f>;
		if (line.rfind("clip", 0) == 0) {
			doc.clips.emplace_back();
			clip = &doc.clips.back();
			track = nullptr;
			std::istringstream ls(line);
			std::string verb, kv, val;
			while (ls >> kv) {
				if (kv == "-name") {
					// Quoted string that may contain spaces: read up to
					// the CLOSING quote only - never getline to EOL (that
					// eats every later flag on the line: the knight's
					// "clip -name ... -duration ... -fps ..." parsed as
					// name-only clips with duration 0, papered over by
					// the end-of-load duration clamp; the same bug on
					// track lines silently retyped every -attr rotate
					// track to translate).
					std::string name;
					std::getline(ls, name, '"'); // to the opening quote
					std::getline(ls, name, '"'); // the name
					clip->name = name;
				}
				else if (kv == "-duration")
					ls >> clip->duration;
				else if (kv == "-fps")
					ls >> clip->fps;
			}
			if (!(clip->duration > 0.0))
				warn("clip '" + clip->name + "': non-positive duration");
			continue;
		}
		// track -joint <id> [-name "<n>"] -attr <a> -interp <i>;
		if (line.rfind("track", 0) == 0) {
			if (!clip) {
				warn("track before any clip (skipped)");
				continue;
			}
			clip->tracks.emplace_back();
			track = &clip->tracks.back();
			std::istringstream ls(line);
			std::string kv, val;
			while (ls >> kv) {
				if (kv == "-joint")
					ls >> track->joint;
				else if (kv == "-name") {
					// Same quoted-read rule as the clip line: stop at the
					// closing quote so -attr/-interp still parse.
					std::string name;
					std::getline(ls, name, '"'); // to the opening quote
					std::getline(ls, name, '"'); // the name
					track->name = name;
				}
				else if (kv == "-attr") {
					ls >> val;
					if (val == "rotate")
						track->channel = Channel::Rotate;
					else if (val == "scale")
						track->channel = Channel::Scale;
					else if (val == "translate")
						track->channel = Channel::Translate;
					else
						warn("unknown -attr '" + val + "' (warn-and-skip"
							" - track dropped)");
				}
				else if (kv == "-interp") {
					ls >> val;
					if (val == "step")
						track->spline = Spline::Step;
					else if (val == "linear")
						track->spline = Spline::Linear;
					else if (val == "catmullrom")
						track->spline = Spline::CatmullRom;
					else
						warn("unknown -interp '" + val + "' (linear)");
				}
			}
			continue;
		}
		// Key line: "t v0 v1 v2 [v3];" - 4 values for rotate (unit quat
		// xyzw), 3 for translate/scale.
		if (track) {
			std::istringstream ls(line);
			double t = 0.0;
			glm::vec4 v(0.0f, 0.0f, 0.0f, 1.0f);
			if (!(ls >> t >> v.x >> v.y >> v.z)) {
				warn("malformed key line " + std::to_string(lineNo)
					+ " (skipped): " + line);
				continue;
			}
			(ls >> v.w); // optional 4th (rotate); keep w=1 default for T/S
			Key k{};
			k.t = t;
			k.v = v;
			if (track->channel == Channel::Rotate) {
				k.v = QuatNormalize(k.v);
				// Sign-fix to the predecessor: shortest-path nlerp (the
				// header contract; duplicate-t keys drop with a warn).
				if (!track->keys.empty()
					&& glm::dot(glm::vec4(track->keys.back().v), k.v) < 0.0f)
					k.v = -k.v;
			}
			if (!track->keys.empty() && !(t > track->keys.back().t)) {
				warn("non-ascending key time (dropped): "
					+ std::to_string(t));
				continue;
			}
			track->keys.push_back(k);
			continue;
		}
		warn("unrecognized line (skipped): " + line);
	}
	// Clip duration sanity: longer than the last key time warns + clamps.
	for (Clip& c : doc.clips) {
		double last = 0.0;
		for (const Track& t : c.tracks)
			if (!t.keys.empty())
				last = std::max(last, t.keys.back().t);
		if (c.duration < last) {
			warn("clip '" + c.name + "': duration " + std::to_string(c.duration)
				+ " shorter than last key " + std::to_string(last)
				+ " (clamped)");
			c.duration = last;
		}
	}
	if (doc.clips.empty()) {
		err = "no clips parsed";
		return false;
	}
	return true;
}

} // namespace tour::asset
