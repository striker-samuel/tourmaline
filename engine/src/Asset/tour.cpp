// .tour ASCII scene overlay: writer/parser + cook-cluster replay.
// See Engine/Asset/tour.h for the format contract.

#include <Engine/Asset/tour.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>

namespace smvk::tour {
namespace {

std::string Quote(const std::string& s) {
	std::string o = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\')
			o.push_back('\\');
		o.push_back(c);
	}
	o.push_back('"');
	return o;
}

// Tokenize one line: whitespace split, double-quote grouping with
// backslash escapes, '//' comment starts outside quotes. An explicitly
// quoted empty string ("") yields one empty token (so cook lines with
// empty quoted values, e.g. -lmExcludeKw "" / -cubemapName "", round-trip
// instead of swallowing the next flag).
std::vector<std::string> Tokenize(const std::string& line) {
	std::vector<std::string> toks;
	std::string cur;
	bool inQ = false;
	bool esc = false;
	bool tokActive = false; // any chars or quote pairing for the
	                        // current token (preserves "")
	for (size_t i = 0; i < line.size(); ++i) {
		const char c = line[i];
		if (esc) {
			cur.push_back(c);
			tokActive = true;
			esc = false;
			continue;
		}
		if (c == '\\' && inQ) {
			esc = true;
			continue;
		}
		if (c == '"') {
			inQ = !inQ;
			tokActive = true;
			continue;
		}
		if (!inQ && c == '/' && i + 1 < line.size() && line[i + 1] == '/')
			break;
		if (!inQ && std::isspace((unsigned char)c)) {
			if (tokActive) {
				toks.push_back(cur);
				cur.clear();
				tokActive = false;
			}
			continue;
		}
		cur.push_back(c);
		tokActive = true;
	}
	if (tokActive)
		toks.push_back(cur);
	return toks;
}

bool ParseU32(const std::string& s, uint32_t& out) {
	unsigned long long v = 0;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc() || p != s.data() + s.size())
		return false;
	out = uint32_t(v);
	return true;
}

bool ParseDouble(const std::string& s, double& out) {
	try {
		size_t n = 0;
		out = std::stod(s, &n);
		return n == s.size();
	}
	catch (...) {
		return false;
	}
}

const char* FlagVal(bool b) { return b ? "1" : "0"; }

} // namespace

bool OverrideAttrFromString(const std::string& s, OverrideAttr& out) {
	if (s == "visibility" || s == "hidden" || s == "hide") {
		out = OverrideAttr::Visibility;
		return true;
	}
	if (s == "material") {
		out = OverrideAttr::Material;
		return true;
	}
	if (s == "noLightmap" || s == "no_light_map") {
		out = OverrideAttr::NoLightmap;
		return true;
	}
	if (s == "ditherCloseup" || s == "dither" || s == "dither_closeup") {
		out = OverrideAttr::DitherCloseup;
		return true;
	}
	return false;
}

const char* OverrideAttrName(OverrideAttr a) {
	switch (a) {
	case OverrideAttr::Visibility: return "visibility";
	case OverrideAttr::Material: return "material";
	case OverrideAttr::NoLightmap: return "noLightmap";
	case OverrideAttr::DitherCloseup: return "ditherCloseup";
	}
	return "?";
}

bool OverridePatternMatches(const std::string& pattern,
	const std::string& nodeName) {
	if (pattern.empty())
		return false;
	try {
		std::regex rx(pattern, std::regex::ECMAScript | std::regex::optimize);
		return std::regex_search(nodeName, rx);
	}
	catch (...) {
		return false;
	}
}

std::string DescribeOverride(const OverrideRule& r) {
	char buf[512];
	if (r.attr == OverrideAttr::Material)
		std::snprintf(buf, sizeof(buf), "override /%s/ .%s -> mat.%u%s",
			r.pattern.c_str(), OverrideAttrName(r.attr), r.material,
			r.enabled ? "" : " [off]");
	else
		std::snprintf(buf, sizeof(buf), "override /%s/ .%s = %d%s",
			r.pattern.c_str(), OverrideAttrName(r.attr), r.flag ? 1 : 0,
			r.enabled ? "" : " [off]");
	return buf;
}

std::string Fingerprint(const std::string& path) {
	std::error_code ec;
	const auto sz = std::filesystem::file_size(path, ec);
	if (ec)
		return {};
	const auto mt = std::filesystem::last_write_time(path, ec);
	if (ec)
		return {};
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%llx:%llx",
		(unsigned long long)sz,
		(unsigned long long)mt.time_since_epoch().count());
	return buf;
}

bool SaveTour(const std::string& path, const Doc& doc, std::string& err) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write: " + path;
		return false;
	}
	f << "// .tour " << doc.version << " - VulkanToy editor scene (ASCII, Maya-style)\n";
	f << "// source asset: " << doc.source << "\n";
	f << "// node/material ids are cluster-space (import-deterministic)\n";
	f << "source -p " << Quote(doc.source)
		<< " -fingerprint " << Quote(doc.fingerprint) << ";\n";
	for (const Command& c : doc.commands) {
		switch (c.verb) {
		case Verb::Rename:
			f << "rename -n " << c.node << " " << Quote(c.text);
			break;
		case Verb::SetTranslate:
			f << "setAttr -n " << c.node << " -at translate -t "
				<< c.vec.x << " " << c.vec.y << " " << c.vec.z;
			break;
		case Verb::SetRotate:
			f << "setAttr -n " << c.node << " -at rotate -t "
				<< c.vec.x << " " << c.vec.y << " " << c.vec.z;
			break;
		case Verb::SetScale:
			f << "setAttr -n " << c.node << " -at scale -t "
				<< c.vec.x << " " << c.vec.y << " " << c.vec.z;
			break;
		case Verb::AssignMaterial:
			f << "assignMaterial -n " << c.node << " -m " << c.material;
			break;
		case Verb::SetMatAttr:
			f << "setMatAttr -m " << c.material << " -at " << c.text;
			if (c.text == "baseColor" || c.text == "emissive" ||
				c.text == "layers")
				f << " -t " << c.vec4.x << " " << c.vec4.y << " "
					<< c.vec4.z << " " << c.vec4.w;
			else if (c.text == "alphaMode")
				f << " -t " << (c.number < 0.5 ? "opaque"
					: c.number < 1.5 ? "mask" : "blend");
			else
				f << " -v " << c.number;
			break;
		case Verb::SetNoLightmap:
			f << "setNoLightmap -n " << c.node << " -v " << FlagVal(c.flag);
			break;
		case Verb::SetDitherCloseup:
			f << "setDitherCloseup -n " << c.node << " -v " << FlagVal(c.flag);
			break;
		case Verb::SetVisibility:
			f << "setVisibility -n " << c.node << " -v " << FlagVal(c.flag);
			break;
		case Verb::Parent:
			f << "parent -n " << c.node << " -p " << c.node2;
			break;
		case Verb::CreateLight: {
			// -t packs position + every scalar: x y z intensity range
			// innerDeg outerDeg static01 shadow01 (9 values).
			f << "createLight -p " << c.node << " -name " << Quote(c.text)
				<< " -at " << (c.vec4.x >= 1.5f ? "spot" : "point")
				<< " -t " << c.vec.x << " " << c.vec.y << " " << c.vec.z
				<< " " << c.vec4.y << " " << c.vec4.z << " " << c.vec4.w
				<< " " << c.number << " " << (c.flag ? 1 : 0) << " " << c.node2;
			break;
		}
		case Verb::SetLightAttr: {
			f << "setLightAttr ";
			if ((c.node & kCreateLightRefBase) != 0)
				f << "-ref " << (c.node & ~kCreateLightRefBase);
			else
				f << "-n " << c.node;
			f << " -at " << c.text;
			if (c.text == "color" || c.text == "position"
				|| c.text == "rotation")
				f << " -t " << c.vec4.x << " " << c.vec4.y << " " << c.vec4.z;
			else if (c.text == "name")
				f << " -v " << Quote(c.name);
			else if (c.text == "static" || c.text == "shadow")
				f << " -v " << (c.flag ? "on" : "off");
			else
				f << " -v " << c.number;
			break;
		}
		}
		if (!c.name.empty())
			f << " // " << c.name;
		f << ";\n";
	}
	// Wide Clarisse-style overrides (ordered; later wins at replay).
	for (const OverrideRule& r : doc.overrides) {
		f << "override -pattern " << Quote(r.pattern)
			<< " -at " << OverrideAttrName(r.attr);
		if (r.attr == OverrideAttr::Material)
			f << " -m " << r.material;
		else
			f << " -v " << FlagVal(r.flag);
		f << " -enabled " << (r.enabled ? "on" : "off") << ";\n";
	}
	// Editor-only settings (editorSet): persisted for the editor, never
	// applied at cook (ApplyTourToCluster ignores doc.editorSettings).
	for (const auto& [k, v] : doc.editorSettings)
		f << "editorSet -k " << Quote(k) << " -v " << Quote(v) << ";\n";
	// Scene references (Clarisse-style): placeholder + linked source; the
	// loader (gltf_import) inlines the subtree at import time, so the
	// cooked geometry and every tour command addressing it stay
	// deterministic (cluster-space ids).
	for (const ReferenceCmd& r : doc.references) {
		f << "reference -n " << r.parent
			<< " -source " << Quote(r.source);
		if (!r.name.empty())
			f << " -name " << Quote(r.name);
		if (r.hasTranslate)
			f << " -t " << r.translate.x << " " << r.translate.y
				<< " " << r.translate.z;
		if (!r.enabled)
			f << " -enabled off";
		f << ";\n";
	}
	if (doc.hasCook) {
		const TourCook& k = doc.cook;
		f << "cook -outPak " << Quote(k.outPak)
			<< " -meshopt " << (k.meshopt ? "on" : "off")
			<< " -lods " << (k.lods ? "on" : "off")
			<< " -lightmap " << (k.lightmap ? "on" : "off")
			<< " -lmMetersPerTexel " << k.lmMetersPerTexel
			<< " -lmLobes " << k.lmLobes
			<< " -lmAtlasWidth " << k.lmAtlasWidth
			<< " -lmRays " << k.lmRays
			<< " -lmExcludeSmall " << k.lmExcludeSmall
			<< " -lmExcludeKw " << Quote(k.lmExcludeKw)
			<< " -sunAzimuth " << k.sunAzimuth
			<< " -sunElevation " << k.sunElevation
			<< " -sunIntensity " << k.sunIntensity
			<< " -skyIntensity " << k.skyIntensity
			<< " -cubemap " << (k.cubemap ? "on" : "off")
			<< " -cubemapName " << Quote(k.cubemapName)
			<< " -cubemapSize " << k.cubemapSize
			<< " -cubemapRays " << k.cubemapRays
			<< ";\n";
	}
	if (!f.good()) {
		err = "write failed: " + path;
		return false;
	}
	return true;
}

// -n <id> / -m <id> / -p <id> / -at <name> / -t <nums...> / -v <num|word>
// -pattern <regex> / -enabled <on|off> (override rules only)
// -source <path> / -name <word> (reference lines only)
// -ref <k> (setLightAttr on the k-th createLight in the doc)
struct Flags {
	bool hasN = false, hasM = false, hasP = false, hasAt = false;
	uint32_t n = 0, m = 0, p = 0;
	std::string at;
	std::vector<std::string> t; // -t values
	std::string v;              // -v value (raw)
	bool hasV = false;
	bool hasRef = false;
	uint32_t ref = 0; // createLight creation-order index
	std::string pattern;
	bool hasPattern = false;
	bool enabled = true;
	bool hasEnabled = false;
	std::string source;
	bool hasSource = false;
	std::string name;
	bool hasName = false;
};

static bool ParseFlagWord(const std::string& w, bool& out) {
	if (w == "1" || w == "on" || w == "true" || w == "yes") {
		out = true;
		return true;
	}
	if (w == "0" || w == "off" || w == "false" || w == "no") {
		out = false;
		return true;
	}
	return false;
}

static bool ParseFlags(const std::vector<std::string>& toks, size_t from,
	Flags& f, std::string& err, int lineNo) {
	for (size_t i = from; i < toks.size();) {
		const std::string& k = toks[i];
		if ((k == "-n" || k == "-m" || k == "-p") && i + 1 < toks.size()) {
			std::string idTok = toks[i + 1];
			if (!idTok.empty() && idTok.back() == ';')
				idTok.pop_back();
			uint32_t v = 0;
			if (!ParseU32(idTok, v)) {
				err = "line " + std::to_string(lineNo) + ": bad id '" + toks[i + 1] + "'";
				return false;
			}
			if (k == "-n") {
				f.hasN = true;
				f.n = v;
			}
			else if (k == "-m") {
				f.hasM = true;
				f.m = v;
			}
			else {
				f.hasP = true;
				f.p = v;
			}
			i += 2;
		}
		else if (k == "-at" && i + 1 < toks.size()) {
			f.hasAt = true;
			f.at = toks[i + 1];
			i += 2;
		}
		else if (k == "-t") {
			++i;
			auto isFlag = [](const std::string& w) {
				return w == "-n" || w == "-m" || w == "-p" ||
					w == "-at" || w == "-t" || w == "-v" ||
					w == "-pattern" || w == "-enabled" ||
					w == "-fingerprint" || w == "-ref";
			};
			while (i < toks.size() && !isFlag(toks[i])) {
				std::string v = toks[i];
				bool term = false;
				if (!v.empty() && v.back() == ';') {
					v.pop_back();
					term = true;
				}
				f.t.push_back(v);
				++i;
				if (term)
					break;
			}
		}
		else if (k == "-v" && i + 1 < toks.size()) {
			f.hasV = true;
			f.v = toks[i + 1];
			if (!f.v.empty() && f.v.back() == ';')
				f.v.pop_back();
			i += 2;
		}
		else if (k == "-ref" && i + 1 < toks.size()) {
			std::string idTok = toks[i + 1];
			if (!idTok.empty() && idTok.back() == ';')
				idTok.pop_back();
			uint32_t v = 0;
			if (!ParseU32(idTok, v)) {
				err = "line " + std::to_string(lineNo) + ": bad ref '" + toks[i + 1] + "'";
				return false;
			}
			f.hasRef = true;
			f.ref = v;
			i += 2;
		}
		else if (k == "-pattern" && i + 1 < toks.size()) {
			f.hasPattern = true;
			f.pattern = toks[i + 1];
			if (!f.pattern.empty() && f.pattern.back() == ';')
				f.pattern.pop_back();
			i += 2;
		}
		else if (k == "-enabled" && i + 1 < toks.size()) {
			std::string w = toks[i + 1];
			if (!w.empty() && w.back() == ';')
				w.pop_back();
			bool b = true;
			if (ParseFlagWord(w, b)) {
				f.hasEnabled = true;
				f.enabled = b;
			}
			i += 2;
		}
		else if (k == "-source" && i + 1 < toks.size()) {
			f.hasSource = true;
			f.source = toks[i + 1];
			if (!f.source.empty() && f.source.back() == ';')
				f.source.pop_back();
			i += 2;
		}
		else if (k == "-name" && i + 1 < toks.size()) {
			f.hasName = true;
			f.name = toks[i + 1];
			if (!f.name.empty() && f.name.back() == ';')
				f.name.pop_back();
			i += 2;
		}
		else if (k == "-fingerprint" && i + 1 < toks.size()) {
			i += 2; // handled by the source parser below
		}
		else {
			// positional (rename target name, source path, cook values...).
			// A bare ";" (space before the terminator) carries nothing.
			std::string pos = k;
			if (!pos.empty() && pos.back() == ';')
				pos.pop_back();
			if (!pos.empty())
				f.t.push_back(pos);
			++i;
		}
	}
	return true;
}

bool LoadTour(const std::string& path, Doc& doc, std::string& err,
	std::vector<std::string>* warnings) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open: " + path;
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return LoadTourFromString(ss.str(), doc, err, warnings);
}

bool LoadTourFromString(const std::string& text, Doc& doc, std::string& err,
	std::vector<std::string>* warnings) {
	doc = Doc{};
	auto warn = [&](const std::string& w) {
		if (warnings)
			warnings->push_back(w);
	};
	std::istringstream f(text);
	int lineNo = 0;
	std::string line;
	while (std::getline(f, line)) {
		++lineNo;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto toks = Tokenize(line);
		if (toks.empty())
			continue;
		// strip trailing ';' token
		if (!toks.empty() && toks.back() == ";")
			toks.pop_back();
		if (toks.empty())
			continue;
		const std::string& verb = toks[0];
		if (verb == "source") {
			// source -p "<path>" [-fingerprint "<hex>"]; (-p is a PATH
			// here, not a parent id, so parse manually, not via ParseFlags)
			for (size_t i = 1; i < toks.size(); ++i) {
				std::string tok = toks[i];
				if (!tok.empty() && tok.back() == ';')
					tok.pop_back();
				if (tok == "-p" && i + 1 < toks.size()) {
					doc.source = toks[i + 1];
					if (!doc.source.empty() && doc.source.back() == ';')
						doc.source.pop_back();
					++i;
				}
				else if (tok == "-fingerprint" && i + 1 < toks.size()) {
					doc.fingerprint = toks[i + 1];
					if (!doc.fingerprint.empty() && doc.fingerprint.back() == ';')
						doc.fingerprint.pop_back();
					++i;
				}
			}
			continue;
		}
		if (verb == "editorSet") {
			// editorSet -k "<key>" -v "<value>"; editor-only (never cooked).
			std::string key, val;
			for (size_t i = 1; i < toks.size(); ++i) {
				if (toks[i] == "-k" && i + 1 < toks.size()) {
					key = toks[i + 1];
					++i;
				}
				else if (toks[i] == "-v" && i + 1 < toks.size()) {
					val = toks[i + 1];
					++i;
				}
			}
			if (!key.empty())
				doc.editorSettings.emplace_back(key, val);
			continue;
		}
		if (verb == "cook") {
			doc.hasCook = true;			TourCook& k = doc.cook;
			for (size_t i = 1; i < toks.size(); ++i) {
				auto get = [&](const char* key, std::string& out) {
					if (toks[i] == key && i + 1 < toks.size()) {
						out = toks[i + 1];
						if (!out.empty() && out.back() == ';')
							out.pop_back();
						++i;
						return true;
					}
					return false;
				};
				std::string v;
				if (get("-outPak", v))
					k.outPak = v;
				else if (get("-meshopt", v))
					k.meshopt = (v == "on" || v == "1");
				else if (get("-lods", v))
					k.lods = (v == "on" || v == "1");
			else if (get("-lightmap", v))
				k.lightmap = (v == "on" || v == "1");
			else if (get("-lmMetersPerTexel", v))
				k.lmMetersPerTexel = std::max(std::stof(v), 1e-4f);
			else if (get("-lmDensity", v)) {
				// Legacy: texels/meter -> meters/texel.
				const float tpm = std::max(std::stof(v), 2.0f);
				k.lmMetersPerTexel = 1.0f / tpm;
			}
			else if (get("-lmLobes", v))
				k.lmLobes = uint32_t(std::stoul(v));
			else if (get("-lmAtlasWidth", v))
				k.lmAtlasWidth = uint32_t(std::stoul(v));
			else if (get("-lmBudget", v)) {
				// Legacy (removed with --lm-budget): ignored — the atlas
				// width is fixed and the height grows with content now.
			}
			else if (get("-lmRays", v))
					k.lmRays = uint32_t(std::stoul(v));
				else if (get("-lmExcludeSmall", v))
					k.lmExcludeSmall = std::stof(v);
				else if (get("-lmExcludeKw", v))
					k.lmExcludeKw = v;
				else if (get("-sunAzimuth", v))
					k.sunAzimuth = std::stof(v);
				else if (get("-sunElevation", v))
					k.sunElevation = std::stof(v);
				else if (get("-sunIntensity", v))
					k.sunIntensity = std::stof(v);
				else if (get("-skyIntensity", v))
					k.skyIntensity = std::stof(v);
				else if (get("-cubemap", v))
					k.cubemap = (v == "on" || v == "1");
				else if (get("-cubemapName", v))
					k.cubemapName = v;
				else if (get("-cubemapSize", v))
					k.cubemapSize = std::max<uint32_t>(
						uint32_t(std::stoul(v)), 4);
				else if (get("-cubemapRays", v))
					k.cubemapRays = std::max<uint32_t>(
						uint32_t(std::stoul(v)), 1);
			}
			continue;
		}
		Command c{};
		Flags fl{};
		if (!ParseFlags(toks, 1, fl, err, lineNo))
			return false;
		if (verb == "rename" && fl.hasN && !fl.t.empty()) {
			c.verb = Verb::Rename;
			c.node = fl.n;
			c.text = fl.t[0];
		}
		else if (verb == "setAttr" && fl.hasN && fl.hasAt && fl.t.size() >= 3) {
			double x = 0, y = 0, z = 0;
			if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
				!ParseDouble(fl.t[2], z)) {
				err = "line " + std::to_string(lineNo) + ": bad triple";
				return false;
			}
			c.node = fl.n;
			c.vec = glm::vec3(float(x), float(y), float(z));
			if (fl.at == "translate")
				c.verb = Verb::SetTranslate;
			else if (fl.at == "rotate")
				c.verb = Verb::SetRotate;
			else if (fl.at == "scale")
				c.verb = Verb::SetScale;
			else {
				warn("tour:" + std::to_string(lineNo) +
					": unknown setAttr '" + fl.at + "' (skipped)");
				continue;
			}
		}
		else if (verb == "assignMaterial" && fl.hasN && fl.hasM) {
			c.verb = Verb::AssignMaterial;
			c.node = fl.n;
			c.material = fl.m;
		}
		else if (verb == "setMatAttr" && fl.hasM && fl.hasAt) {
			c.verb = Verb::SetMatAttr;
			c.material = fl.m;
			c.text = fl.at;
			if (fl.at == "baseColor" || fl.at == "emissive" ||
				fl.at == "layers") {
				if (fl.t.size() < 4) {
					err = "line " + std::to_string(lineNo) + ": want 4 values";
					return false;
				}
				double v[4] = {};
				for (int k = 0; k < 4; ++k)
					if (!ParseDouble(fl.t[k], v[k])) {
						err = "line " + std::to_string(lineNo) + ": bad value";
						return false;
					}
				c.vec4 = glm::vec4(float(v[0]), float(v[1]), float(v[2]), float(v[3]));
			}
			else if (fl.at == "alphaMode") {
				const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
				c.number = (w == "mask") ? 1.0 : (w == "blend") ? 2.0 : 0.0;
			}
			else {
				const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
				if (!ParseDouble(w, c.number)) {
					err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
					return false;
				}
			}
		}
		else if (verb == "setNoLightmap" && fl.hasN && fl.hasV) {
			c.verb = Verb::SetNoLightmap;
			c.node = fl.n;
			c.flag = (fl.v == "1" || fl.v == "on" || fl.v == "true");
		}
		else if (verb == "setDitherCloseup" && fl.hasN && fl.hasV) {
			c.verb = Verb::SetDitherCloseup;
			c.node = fl.n;
			c.flag = (fl.v == "1" || fl.v == "on" || fl.v == "true");
		}
		else if (verb == "setVisibility" && fl.hasN && fl.hasV) {
			bool b = false;
			if (!ParseFlagWord(fl.v, b)) {
				err = "line " + std::to_string(lineNo) + ": bad flag '" + fl.v + "'";
				return false;
			}
			c.verb = Verb::SetVisibility;
			c.node = fl.n;
			c.flag = b;
		}
		// setHidden is the legacy alias for setVisibility (hidden flag).
		else if (verb == "setHidden" && fl.hasN && fl.hasV) {
			bool b = false;
			if (!ParseFlagWord(fl.v, b)) {
				err = "line " + std::to_string(lineNo) + ": bad flag '" + fl.v + "'";
				return false;
			}
			c.verb = Verb::SetVisibility;
			c.node = fl.n;
			c.flag = b;
		}
		else if (verb == "override" && fl.hasPattern && fl.hasAt) {
			OverrideRule r{};
			if (!OverrideAttrFromString(fl.at, r.attr)) {
				warn("tour:" + std::to_string(lineNo) +
					": unknown override attr '" + fl.at + "' (skipped)");
				continue;
			}
			r.pattern = fl.pattern;
			r.enabled = fl.hasEnabled ? fl.enabled : true;
			if (r.attr == OverrideAttr::Material) {
				if (!fl.hasM) {
					err = "line " + std::to_string(lineNo) + ": override material needs -m";
					return false;
				}
				r.material = fl.m;
			}
			else {
				if (!fl.hasV) {
					err = "line " + std::to_string(lineNo) + ": override needs -v";
					return false;
				}
				bool b = false;
				if (!ParseFlagWord(fl.v, b)) {
					err = "line " + std::to_string(lineNo) + ": bad flag '" + fl.v + "'";
					return false;
				}
				r.flag = b;
			}
			// Validate the regex now (warn-and-skip on bad patterns).
			try {
				std::regex rx(r.pattern, std::regex::ECMAScript);
				(void)rx;
			}
			catch (const std::exception& e) {
				warn("tour:" + std::to_string(lineNo) +
					": bad override regex '" + r.pattern + "' (" + e.what() + ", skipped)");
				continue;
			}
			doc.overrides.push_back(std::move(r));
			continue;
		}
		else if (verb == "reference" && fl.hasSource) {
			// Clarisse-style scene reference: placeholder under -n with a
			// linked source; the gltf_import loader inlines the subtree
			// (recursively, via each source's own tour) at scene build.
			ReferenceCmd r{};
			r.source = fl.source;
			r.parent = fl.hasN ? fl.n : 0;
			if (fl.hasName)
				r.name = fl.name;
			if (fl.t.size() >= 3) {
				double x = 0, y = 0, z = 0;
				if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
					!ParseDouble(fl.t[2], z)) {
					err = "tour:" + std::to_string(lineNo) + ": bad translate";
					return false;
				}
				r.translate = glm::vec3(float(x), float(y), float(z));
				r.hasTranslate = true;
			}
			r.enabled = fl.hasEnabled ? fl.enabled : true;
			doc.references.push_back(std::move(r));
			continue;
		}
		else if (verb == "parent" && fl.hasN && fl.hasP) {
			c.verb = Verb::Parent;
			c.node = fl.n;
			c.node2 = fl.p;
		}
		else if (verb == "createLight" && fl.hasP && fl.hasName && fl.hasAt
			&& fl.t.size() >= 9) {
			// -t packs: x y z intensity range innerDeg outerDeg
			// static01 shadow01.
			if (fl.at != "point" && fl.at != "spot") {
				err = "line " + std::to_string(lineNo) + ": createLight wants -at point|spot";
				return false;
			}
			double v[9] = {};
			for (int k = 0; k < 9; ++k)
				if (!ParseDouble(fl.t[size_t(k)], v[k])) {
					err = "line " + std::to_string(lineNo) + ": bad createLight value";
					return false;
				}
			c.verb = Verb::CreateLight;
			c.node = fl.p;
			c.text = fl.name;
			c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
			c.vec4 = glm::vec4(fl.at == "spot" ? 2.0f : 1.0f,
				float(v[3]), float(v[4]), float(v[5]));
			c.number = v[6];
			c.flag = v[7] != 0.0;
			c.node2 = v[8] != 0.0 ? 1u : 0u;
		}
		else if (verb == "setLightAttr" && (fl.hasN || fl.hasRef) && fl.hasAt) {
			static const char* kAttrs[] = { "color", "intensity", "range",
				"inner", "outer", "static", "shadow", "name", "position",
				"rotation" };
			bool known = false;
			for (const char* a : kAttrs)
				if (fl.at == a) {
					known = true;
					break;
				}
			if (!known) {
				err = "line " + std::to_string(lineNo) + ": bad light attr '" + fl.at + "'";
				return false;
			}
			c.verb = Verb::SetLightAttr;
			c.node = fl.hasRef ? (kCreateLightRefBase | fl.ref) : fl.n;
			c.text = fl.at;
			if (fl.at == "color" || fl.at == "position" || fl.at == "rotation") {
				if (fl.t.size() < 3) {
					err = "line " + std::to_string(lineNo) + ": light triple wants 3 values";
					return false;
				}
				double r = 0, g = 0, b = 0;
				if (!ParseDouble(fl.t[0], r) || !ParseDouble(fl.t[1], g) ||
					!ParseDouble(fl.t[2], b)) {
					err = "line " + std::to_string(lineNo) + ": bad light triple";
					return false;
				}
				c.vec4 = glm::vec4(float(r), float(g), float(b), 1.0f);
			}
			else if (fl.at == "name") {
				c.name = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			}
			else if (fl.at == "static" || fl.at == "shadow") {
				bool on = false;
				const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
				if (!ParseFlagWord(w, on)) {
					err = "line " + std::to_string(lineNo) + ": bad flag '" + w + "'";
					return false;
				}
				c.flag = on;
			}
			else {
				const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
				if (!ParseDouble(w, c.number)) {
					err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
					return false;
				}
			}
		}
		else {
			warn("tour:" + std::to_string(lineNo) + ": unknown verb '" +
				verb + "' (skipped)");
			continue;
		}
		doc.commands.push_back(std::move(c));
	}
	return true;
}

void TourReparent(Scene& scene, uint32_t node, uint32_t newParent) {
	if (node >= scene.hierarchy.size() || newParent >= scene.hierarchy.size() ||
		node == newParent || node == 0)
		return;
	// Cycle guard.
	for (int n = int(newParent); n >= 0 && n < int(scene.hierarchy.size());) {
		if (uint32_t(n) == node)
			return;
		n = scene.hierarchy[size_t(n)].parent;
	}
	const int oldParent = scene.hierarchy[node].parent;
	if (oldParent == int(newParent))
		return;
	// Depth guard (changedAtThisFrame has MAX_NODE_LEVEL levels).
	const int newLevel = scene.hierarchy[newParent].level + 1;
	{
		int maxDepth = 0;
		std::vector<std::pair<uint32_t, int>> stack{ { node, 0 } };
		while (!stack.empty()) {
			auto [n, d] = stack.back();
			stack.pop_back();
			maxDepth = std::max(maxDepth, d);
			for (int c = scene.hierarchy[n].firstChild; c != -1;
				c = scene.hierarchy[c].nextSibling)
				stack.emplace_back(uint32_t(c), d + 1);
		}
		if (newLevel + maxDepth >= MAX_NODE_LEVEL)
			return;
	}
	// Unlink.
	if (oldParent >= 0) {
		Hierarchy& op = scene.hierarchy[size_t(oldParent)];
		int prev = -1;
		for (int c = op.firstChild; c != -1; c = scene.hierarchy[c].nextSibling) {
			if (c == int(node)) {
				if (prev < 0)
					op.firstChild = scene.hierarchy[c].nextSibling;
				else
					scene.hierarchy[size_t(prev)].nextSibling =
					scene.hierarchy[c].nextSibling;
				break;
			}
			prev = c;
		}
		int last = -1;
		for (int c = op.firstChild; c != -1; c = scene.hierarchy[c].nextSibling)
			last = c;
		op.lastSibling = last;
	}
	// Link (append).
	Hierarchy& np = scene.hierarchy[newParent];
	scene.hierarchy[node].parent = int(newParent);
	scene.hierarchy[node].nextSibling = -1;
	if (np.firstChild < 0) {
		np.firstChild = int(node);
		np.lastSibling = int(node);
	}
	else {
		int last = (np.lastSibling >= 0) ? np.lastSibling : np.firstChild;
		for (; scene.hierarchy[size_t(last)].nextSibling >= 0;
			last = scene.hierarchy[size_t(last)].nextSibling) {
		}
		scene.hierarchy[size_t(last)].nextSibling = int(node);
		np.lastSibling = int(node);
	}
	scene.hierarchy[node].lastSibling = int(node);
	// Levels.
	{
		std::vector<uint32_t> stack{ node };
		scene.hierarchy[node].level = newLevel;
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			for (int c = scene.hierarchy[n].firstChild; c != -1;
				c = scene.hierarchy[c].nextSibling) {
				scene.hierarchy[size_t(c)].level = scene.hierarchy[n].level + 1;
				stack.push_back(uint32_t(c));
			}
		}
	}
}

static Material* CookMaterial(Cluster& cluster, uint32_t poolIdx) {
	const ClusterMaterialHandle h = cluster.materials.getHandle(poolIdx);
	if (!h.valid())
		return nullptr;
	return cluster.materials.get(h);
}

static void ApplyMatAttr(Material& mat, const Command& c) {
	const std::string& at = c.text;
	if (at == "baseColor")
		mat.values.diffuse = c.vec4;
	else if (at == "emissive") {
		mat.values.emissiveFactor = glm::vec4(c.vec4.x, c.vec4.y, c.vec4.z, 0.0f);
		if (c.vec4.w >= 0.0f)
			mat.values.params2.z = c.vec4.w; // emissiveStrength rides w
	}
	else if (at == "metallic")
		mat.values.params.y = float(c.number);
	else if (at == "roughness")
		mat.values.params.z = float(c.number);
	else if (at == "alphaCutoff")
		mat.values.params.x = float(c.number);
	else if (at == "emissiveStrength")
		mat.values.params2.z = float(c.number);
	else if (at == "normalScale")
		mat.values.params2.y = float(c.number);
	else if (at == "occlusionStrength")
		mat.values.params2.x = float(c.number);
	else if (at == "doubleSided")
		mat.doubleSided = (c.number != 0.0);
	else if (at == "castsShadows")
		mat.castsShadows = (c.number != 0.0);
	else if (at == "layers") {
		// Cluster-local pool ids (the cook domain, pre-merge): x/y/z =
		// layers 1..3 (-1 none), w = active layer count (0 = legacy).
		mat.values.layerMaterials = glm::ivec4(
			int(c.vec4.x), int(c.vec4.y), int(c.vec4.z), int(c.vec4.w));
	}
	else if (at == "alphaMode") {
		mat.alphaMode = (c.number < 0.5) ? AlphaMode::Opaque
			: (c.number < 1.5) ? AlphaMode::Mask : AlphaMode::Blend;
		mat.shadingModel = (mat.alphaMode == AlphaMode::Mask)
			? ShadingModel::Masked : ShadingModel::DefaultLit;
		mat.values.params.w =
			(mat.alphaMode == AlphaMode::Mask) ? 2.0f : 0.0f;
		mat.values.params2.w = float(int(mat.alphaMode));
	}
	mat.rebuildPipeline();
}

bool ApplyTourToCluster(Cluster& cluster, const Doc& doc,
	std::vector<std::string>* warnings, std::string* err) {
	auto warn = [&](const std::string& w) {
		if (warnings)
			warnings->push_back(w);
	};
	Scene& scene = cluster.scene;
	auto checkNode = [&](uint32_t n, const Command& c) {
		if (n >= scene.hierarchy.size()) {
			if (err)
				*err = "tour: node id out of range: " + std::to_string(n);
			return false;
		}
		if (!c.name.empty() && getNodeName(scene, int(n)) != c.name)
			warn("tour: node " + std::to_string(n) + " name is '" +
				getNodeName(scene, int(n)) + "', tour recorded '" + c.name + "'");
		return true;
	};
	// Creation-order ids for SetLightAttr refs (kCreateLightRefBase | k):
	// created light nodes have no cluster id until replayed.
	std::vector<uint32_t> createdLights;
	for (const Command& c : doc.commands) {
		switch (c.verb) {
		case Verb::Rename: {
			if (!checkNode(c.node, c))
				return false;
			const uint32_t id = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.text);
			scene.nameForNode[c.node] = id;
			break;
		}
		case Verb::SetTranslate:
		case Verb::SetRotate:
		case Verb::SetScale: {
			// Transforms compose through the editor's TRS convention; the
			// tour carries the full triple per node, so rebuild from the
			// CURRENT local matrix with only this channel replaced. To stay
			// order-independent, stash per-node triples here.
			break; // handled in the second pass below
		}
		case Verb::AssignMaterial: {
			if (!checkNode(c.node, c))
				return false;
			if (!CookMaterial(cluster, c.material)) {
				if (err)
					*err = "tour: material out of range: " +
						std::to_string(c.material);
				return false;
			}
			scene.materialForNode[c.node] = c.material;
			break;
		}
		case Verb::SetMatAttr: {
			Material* mat = CookMaterial(cluster, c.material);
			if (!mat) {
				if (err)
					*err = "tour: material out of range: " +
						std::to_string(c.material);
				return false;
			}
			ApplyMatAttr(*mat, c);
			break;
		}
		case Verb::SetNoLightmap: {
			if (!checkNode(c.node, c))
				return false;
			if (scene.noLightmap.size() != scene.hierarchy.size())
				scene.noLightmap.assign(scene.hierarchy.size(), 0);
			scene.noLightmap[c.node] = c.flag ? 1 : 0;
			break;
		}
		case Verb::SetDitherCloseup: {
			if (!checkNode(c.node, c))
				return false;
			if (scene.ditherCloseup.size() != scene.hierarchy.size())
				scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
			scene.ditherCloseup[c.node] = c.flag ? 1 : 0;
			break;
		}
		case Verb::SetVisibility: {
			if (!checkNode(c.node, c))
				return false;
			if (scene.hidden.size() != scene.hierarchy.size())
				scene.hidden.assign(scene.hierarchy.size(), 0);
			scene.hidden[c.node] = c.flag ? 1 : 0;
			break;
		}
		case Verb::Parent: {
			if (!checkNode(c.node, c))
				return false;
			if (c.node2 >= scene.hierarchy.size()) {
				if (err)
					*err = "tour: parent id out of range: " +
						std::to_string(c.node2);
				return false;
			}
			TourReparent(scene, c.node, c.node2);
			break;
		}
		case Verb::CreateLight: {
			// Editor-authored punctual light: append a node under the
			// recorded parent + a lights-pool definition. Angles ride in
			// degrees (editor convention); the runtime converts.
			if (c.node >= scene.hierarchy.size()) {
				if (err)
					*err = "tour: createLight parent out of range: " +
						std::to_string(c.node);
				return false;
			}
			const int level = scene.hierarchy[c.node].level + 1;
			const int newId = addNode(scene, int(c.node), level);
			if (newId < 0) {
				if (err)
					*err = "tour: createLight failed under parent " +
						std::to_string(c.node);
				return false;
			}
			scene.localTransform[size_t(newId)] =
				glm::translate(glm::mat4(1.0f), c.vec);
			SceneLight def{};
			def.type = c.vec4.x >= 1.5f
				? SceneLight::Type::Spot : SceneLight::Type::Point;
			def.intensity = std::max(c.vec4.y, 0.0f);
			def.range = std::max(c.vec4.z, 0.0f);
			def.innerConeAngle = glm::radians(std::clamp(c.vec4.w, 0.0f, 89.0f));
			def.outerConeAngle = glm::radians(
				std::clamp(float(c.number), 0.0f, 89.0f));
			if (c.flag)
				def.flags |= SceneLight::kSceneLightFlag_Static;
			if (c.node2 != 0)
				def.flags |= SceneLight::kSceneLightFlag_CastShadow;
			def.name = c.text;
			const uint32_t poolIdx =
				cluster.lights.create(std::move(def)).index();
			const uint32_t nameId = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.text);
			scene.nameForNode[uint32_t(newId)] = nameId;
			scene.lightForNode[uint32_t(newId)] = poolIdx;
			createdLights.push_back(uint32_t(newId));
			break;
		}
		case Verb::SetLightAttr: {
			uint32_t target = c.node;
			if ((target & kCreateLightRefBase) != 0) {
				const uint32_t k = target & ~kCreateLightRefBase;
				if (k >= createdLights.size()) {
					if (err)
						*err = "tour: setLightAttr ref out of range: " +
							std::to_string(k);
					return false;
				}
				target = createdLights[k];
			}
			else if (!checkNode(target, c)) {
				return false;
			}
			auto lit = scene.lightForNode.find(target);
			if (lit == scene.lightForNode.end()) {
				if (err)
					*err = "tour: setLightAttr on non-light node " +
						std::to_string(target);
				return false;
			}
			SceneLight* def = cluster.lights.get(
				cluster.lights.getHandle(lit->second));
			if (!def) {
				if (err)
					*err = "tour: setLightAttr bad light pool index";
				return false;
			}
			if (c.text == "color") {
				def->color = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			}
			else if (c.text == "intensity") {
				def->intensity = std::max(float(c.number), 0.0f);
			}
			else if (c.text == "range") {
				def->range = std::max(float(c.number), 0.0f);
			}
			else if (c.text == "inner") {
				def->innerConeAngle = glm::radians(
					std::clamp(float(c.number), 0.0f, 89.0f));
			}
			else if (c.text == "outer") {
				def->outerConeAngle = glm::radians(
					std::clamp(float(c.number), 0.0f, 89.0f));
			}
			else if (c.text == "static") {
				if (c.flag)
					def->flags |= SceneLight::kSceneLightFlag_Static;
				else
					def->flags &= ~SceneLight::kSceneLightFlag_Static;
			}
			else if (c.text == "shadow") {
				if (c.flag)
					def->flags |= SceneLight::kSceneLightFlag_CastShadow;
				else
					def->flags &= ~SceneLight::kSceneLightFlag_CastShadow;
			}
			else if (c.text == "name") {
				def->name = c.name;
				const uint32_t id = uint32_t(scene.nodeNames.size());
				scene.nodeNames.push_back(c.name);
				scene.nameForNode[target] = id;
			}
			else if (c.text == "position" || c.text == "rotation") {
				// Rebuild the node's local TRS with one channel replaced
				// (editor TRS convention: Rz*Ry*Rx, degrees).
				glm::vec3 skew, t, s;
				glm::vec4 persp;
				glm::quat q;
				glm::decompose(scene.localTransform[target], s, q, t,
					skew, persp);
				glm::vec3 e = glm::degrees(glm::eulerAngles(q));
				if (c.text == "position")
					t = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
				else
					e = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
				glm::mat4 m(1.0f);
				m = glm::translate(m, t);
				m = glm::rotate(m, glm::radians(e.z), glm::vec3(0, 0, 1));
				m = glm::rotate(m, glm::radians(e.y), glm::vec3(0, 1, 0));
				m = glm::rotate(m, glm::radians(e.x), glm::vec3(1, 0, 0));
				m = glm::scale(m, s);
				scene.localTransform[target] = m;
			}
			else {
				warn("tour: unknown light attr '" + c.text + "' (skipped)");
			}
			break;
		}
		}
	}
	// Transform pass: collect per-node TRS triples (tour order = last wins
	// per channel), then rebuild each touched node's local matrix.
	{
		struct Triple {
			bool hasT = false, hasR = false, hasS = false;
			glm::vec3 t{ 0 }, r{ 0 }, s{ 1 };
		};
		std::unordered_map<uint32_t, Triple> triples;
		for (const Command& c : doc.commands) {
			if (c.verb != Verb::SetTranslate && c.verb != Verb::SetRotate &&
				c.verb != Verb::SetScale)
				continue;
			if (c.node >= scene.hierarchy.size()) {
				if (err)
					*err = "tour: node id out of range: " + std::to_string(c.node);
				return false;
			}
			Triple& t = triples[c.node];
			if (c.verb == Verb::SetTranslate) {
				t.hasT = true;
				t.t = c.vec;
			}
			else if (c.verb == Verb::SetRotate) {
				t.hasR = true;
				t.r = c.vec;
			}
			else {
				t.hasS = true;
				t.s = c.vec;
			}
			if (!c.name.empty() && getNodeName(scene, int(c.node)) != c.name)
				warn("tour: node " + std::to_string(c.node) + " name mismatch");
		}
		for (auto& [node, t] : triples) {
			// Decompose current local, replace touched channels, rebuild.
			glm::vec3 skew;
			glm::vec4 persp;
			glm::vec3 curT, curS;
			glm::quat curQ;
			glm::decompose(scene.localTransform[node], curS, curQ, curT, skew, persp);
			if (t.hasT)
				curT = t.t;
			if (t.hasS)
				curS = t.s;
			glm::mat4 m(1.0f);
			m = glm::translate(m, curT);
			if (t.hasR) {
				m = glm::rotate(m, glm::radians(t.r.z), glm::vec3(0, 0, 1));
				m = glm::rotate(m, glm::radians(t.r.y), glm::vec3(0, 1, 0));
				m = glm::rotate(m, glm::radians(t.r.x), glm::vec3(1, 0, 0));
			}
			else {
				m *= glm::mat4_cast(curQ);
			}
			m = glm::scale(m, curS);
			scene.localTransform[node] = m;
			markAsChanged(scene, int(node));
		}
	}
	markAsChanged(scene, 0);
	recalculateGlobalTransforms(scene);
	// Wide override pass (Clarisse-style): regex rules in order, later
	// wins. Per-node local commands are MORE specific and win over any
	// wide rule touching the same node+attr (local wins).
	if (!doc.overrides.empty()) {
		std::unordered_set<uint32_t> localVis, localMat, localNoLM, localDither;
		for (const Command& c : doc.commands) {
			switch (c.verb) {
			case Verb::SetVisibility: localVis.insert(c.node); break;
			case Verb::AssignMaterial: localMat.insert(c.node); break;
			case Verb::SetNoLightmap: localNoLM.insert(c.node); break;
			case Verb::SetDitherCloseup: localDither.insert(c.node); break;
			default: break;
			}
		}
		for (const OverrideRule& r : doc.overrides) {
			if (!r.enabled || r.pattern.empty())
				continue;
			std::regex rx;
			try {
				rx.assign(r.pattern, std::regex::ECMAScript | std::regex::optimize);
			}
			catch (...) {
				warn(std::string("tour: bad override regex '") + r.pattern + "' (skipped)");
				continue;
			}
			if (r.attr == OverrideAttr::Material && !CookMaterial(cluster, r.material)) {
				warn("tour: override material out of range: " + std::to_string(r.material));
				continue;
			}
			for (uint32_t n = 0; n < uint32_t(scene.hierarchy.size()); ++n) {
				const std::string nm = getNodeName(scene, int(n));
				if (nm.empty())
					continue;
				bool hit = false;
				try {
					hit = std::regex_search(nm, rx);
				}
				catch (...) {
					continue;
				}
				if (!hit)
					continue;
				switch (r.attr) {
				case OverrideAttr::Visibility:
					if (localVis.contains(n))
						continue; // local wins
					if (scene.hidden.size() != scene.hierarchy.size())
						scene.hidden.assign(scene.hierarchy.size(), 0);
					scene.hidden[n] = r.flag ? 1 : 0;
					break;
				case OverrideAttr::Material:
					if (localMat.contains(n))
						continue;
					scene.materialForNode[n] = r.material;
					break;
				case OverrideAttr::NoLightmap:
					if (localNoLM.contains(n))
						continue;
					if (scene.noLightmap.size() != scene.hierarchy.size())
						scene.noLightmap.assign(scene.hierarchy.size(), 0);
					scene.noLightmap[n] = r.flag ? 1 : 0;
					break;
				case OverrideAttr::DitherCloseup:
					if (localDither.contains(n))
						continue;
					if (scene.ditherCloseup.size() != scene.hierarchy.size())
						scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
					scene.ditherCloseup[n] = r.flag ? 1 : 0;
					break;
				}
			}
		}
	}
	return true;
}

std::string Describe(const Command& c) {
	char buf[256];
	switch (c.verb) {
	case Verb::Rename:
		std::snprintf(buf, sizeof(buf), "rename #%u -> '%s'", c.node, c.text.c_str());
		break;
	case Verb::SetTranslate:
		std::snprintf(buf, sizeof(buf), "translate #%u (%.2f %.2f %.2f)",
			c.node, c.vec.x, c.vec.y, c.vec.z);
		break;
	case Verb::SetRotate:
		std::snprintf(buf, sizeof(buf), "rotate #%u (%.1f %.1f %.1f)",
			c.node, c.vec.x, c.vec.y, c.vec.z);
		break;
	case Verb::SetScale:
		std::snprintf(buf, sizeof(buf), "scale #%u (%.2f %.2f %.2f)",
			c.node, c.vec.x, c.vec.y, c.vec.z);
		break;
	case Verb::AssignMaterial:
		std::snprintf(buf, sizeof(buf), "material #%u -> mat.%u", c.node, c.material);
		break;
	case Verb::SetMatAttr:
		std::snprintf(buf, sizeof(buf), "mat.%u.%s", c.material, c.text.c_str());
		break;
	case Verb::SetNoLightmap:
		std::snprintf(buf, sizeof(buf), "noLightmap #%u = %d", c.node, c.flag ? 1 : 0);
		break;
	case Verb::SetDitherCloseup:
		std::snprintf(buf, sizeof(buf), "ditherCloseup #%u = %d", c.node, c.flag ? 1 : 0);
		break;
	case Verb::SetVisibility:
		std::snprintf(buf, sizeof(buf), "visibility #%u = %s", c.node,
			c.flag ? "hidden" : "visible");
		break;
	case Verb::Parent:
		std::snprintf(buf, sizeof(buf), "parent #%u under #%u", c.node, c.node2);
		break;
	case Verb::CreateLight:
		std::snprintf(buf, sizeof(buf), "createLight '%s' (%s) under #%u",
			c.text.c_str(), c.vec4.x >= 1.5f ? "spot" : "point", c.node);
		break;
	case Verb::SetLightAttr: {
		char tgt[32];
		if ((c.node & kCreateLightRefBase) != 0)
			std::snprintf(tgt, sizeof(tgt), "create%u", c.node & ~kCreateLightRefBase);
		else
			std::snprintf(tgt, sizeof(tgt), "#%u", c.node);
		std::snprintf(buf, sizeof(buf), "light.%s.%s", tgt, c.text.c_str());
		break;
	}
	}
	return buf;
}

} // namespace smvk::tour
