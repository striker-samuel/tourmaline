// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The appmanifest reader/writer (see appmanifest.h). Small on purpose:
// the same line-oriented conventions as the recipe parser (#
// comments, CRLF/LF, quoted strings, line-specific diagnostics), but
// no sections beyond [paks] and no discovery of anything.

#include <Engine/Asset/appmanifest.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace smvk {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// 64-bit hex ("0x1000") — the id rows' form.
bool ParseIdHex(const std::string& s, uint64_t& out) {
	out = 0;
	if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
		return false;
	if (s.size() > 18)
		return false;
	for (size_t i = 2; i < s.size(); ++i) {
		const char c = s[i];
		int v = -1;
		if (c >= '0' && c <= '9')
			v = c - '0';
		else if (c >= 'a' && c <= 'f')
			v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v = c - 'A' + 10;
		if (v < 0)
			return false;
		out = (out << 4) | uint64_t(v);
	}
	return true;
}

std::string Hex(uint64_t v) {
	static const char* k = "0123456789abcdef";
	std::string s = "0x";
	for (int shift = 60; shift >= 0; shift -= 4) {
		const char c = k[(v >> shift) & 0xF];
		if (c != '0' || s.size() > 2)
			s += c;
	}
	return s;
}

std::string Trim(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && IsSpace(s[b]))
		++b;
	while (e > b && IsSpace(s[e - 1]))
		--e;
	return s.substr(b, e - b);
}

size_t CommentStart(const std::string& s) {
	bool inQuote = false;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '"')
			inQuote = !inQuote;
		else if (s[i] == '#' && !inQuote)
			return i;
	}
	return std::string::npos;
}

// A manifest path: manifest-relative, forward slashes, no wildcards,
// no absolute/drive/'..'/empty-segment forms.
bool ValidRelativePath(const std::string& raw, std::string& why) {
	std::string s = raw;
	for (char& c : s)
		if (c == '\\')
			c = '/';
	if (s.empty()) {
		why = "the path is empty";
		return false;
	}
	if (s.front() == '/') {
		why = "absolute paths are not allowed (pak paths are"
			" manifest-relative): '" + raw + "'";
		return false;
	}
	if (s.size() >= 2 && s[1] == ':'
			&& std::isalpha(static_cast<unsigned char>(s[0]))) {
		why = "drive-letter paths are not allowed (never ship"
			" developer-machine absolutes): '" + raw + "'";
		return false;
	}
	for (size_t start = 0; start <= s.size();) {
		const size_t end = s.find('/', start);
		const std::string seg = s.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if (seg.empty()) {
			why = "empty path segment in '" + raw + "'";
			return false;
		}
		if (seg == "." || seg == "..") {
			why = "escaping segments are not allowed: '" + raw + "'";
			return false;
		}
		if (seg.find('*') != std::string::npos
				|| seg.find('?') != std::string::npos) {
			why = "wildcards are not valid here: '" + raw + "'";
			return false;
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return true;
}

} // namespace

bool ValidateAppManifest(const AppManifest& m, std::string& err) {
	if (m.version != kAppManifestVersion) {
		err = "unsupported appmanifest version "
			+ std::to_string(m.version) + " (this build reads version "
			+ std::to_string(kAppManifestVersion) + ")";
		return false;
	}
	if (m.paks.empty()) {
		err = "the manifest lists no paks";
		return false;
	}
	std::set<std::string> seen;
	for (const std::string& pak : m.paks) {
		std::string why;
		if (!ValidRelativePath(pak, why)) {
			err = "pak path '" + pak + "': " + why;
			return false;
		}
		if (!seen.insert(pak).second) {
			err = "duplicate pak entry '" + pak + "'";
			return false;
		}
	}
	std::string why;
	if (!ValidRelativePath(m.entryPak, why)) {
		err = "entry pak '" + m.entryPak + "': " + why;
		return false;
	}
	if (!seen.count(m.entryPak)) {
		err = "the entry pak '" + m.entryPak
			+ "' is not listed in [paks]";
		return false;
	}
	if (m.entryScene.empty()) {
		err = "the entry scene name is empty";
		return false;
	}
	return true;
}

bool CatalogKindFromString(const std::string& kind, uint8_t& out) {
	if (kind == "scene") { out = kCatalogKindScene; return true; }
	if (kind == "texture") { out = kCatalogKindTexture; return true; }
	if (kind == "mesh") { out = kCatalogKindMesh; return true; }
	if (kind == "sound") { out = kCatalogKindSound; return true; }
	if (kind == "shader") { out = kCatalogKindShader; return true; }
	if (kind == "gi") { out = kCatalogKindGi; return true; }
	if (kind == "audiograph") { out = kCatalogKindAudioGraph; return true; }
	if (kind == "spriteset") { out = kCatalogKindSpriteSet; return true; }
	if (kind == "statemachine") { out = kCatalogKindStateMachine; return true; }
	if (kind == "navmesh") { out = kCatalogKindNavMesh; return true; }
	if (kind == "shape") { out = kCatalogKindShape; return true; }
	return false;
}

const char* CatalogKindToString(uint8_t kind) {
	switch (kind) {
	case kCatalogKindScene: return "scene";
	case kCatalogKindTexture: return "texture";
	case kCatalogKindMesh: return "mesh";
	case kCatalogKindSound: return "sound";
	case kCatalogKindShader: return "shader";
	case kCatalogKindGi: return "gi";
	case kCatalogKindAudioGraph: return "audiograph";
	case kCatalogKindSpriteSet: return "spriteset";
	case kCatalogKindStateMachine: return "statemachine";
	case kCatalogKindNavMesh: return "navmesh";
	case kCatalogKindShape: return "shape";
	default: return "";
	}
}

uint64_t ComputeCatalogFingerprint(
	const uint64_t* ids, const uint8_t* kinds,
	const char* const* paths, size_t count) {
	// FNV-1a64 (the repo's content-addressing convention).
	uint64_t h = 1469598103934665603ULL;
	auto fold = [&h](const void* data, size_t bytes) {
		const uint8_t* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < bytes; ++i) {
			h ^= p[i];
			h *= 1099511628211ULL;
		}
	};
	auto foldU64 = [&fold](uint64_t v) {
		uint8_t b[8];
		for (int i = 0; i < 8; ++i)
			b[i] = uint8_t((v >> (8 * i)) & 0xFF);
		fold(b, 8);
	};
	for (size_t i = 0; i < count; ++i) {
		foldU64(ids[i]);
		fold(&kinds[i], 1);
		const char* p = paths[i] ? paths[i] : "";
		fold(p, std::strlen(p));
		const uint8_t sep = 0;
		fold(&sep, 1);
	}
	return h;
}

bool LoadAppManifestText(const std::string& text, AppManifest& out,
	std::string& err) {
	out = AppManifest{};
	std::string body = text;
	if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF
			&& static_cast<unsigned char>(body[1]) == 0xBB
			&& static_cast<unsigned char>(body[2]) == 0xBF)
		body = body.substr(3);
	bool inPaks = false, sawVersion = false, sawEntry = false;
	std::vector<std::string> lines;
	{
		size_t start = 0;
		while (start <= body.size()) {
			const size_t end = body.find('\n', start);
			if (end == std::string::npos) {
				if (start < body.size())
					lines.push_back(body.substr(start));
				break;
			}
			lines.push_back(body.substr(start, end - start));
			start = end + 1;
		}
	}
	for (size_t ln = 0; ln < lines.size(); ++ln) {
		std::string line = lines[ln];
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t hash = CommentStart(line);
		std::string content =
			Trim(hash == std::string::npos ? line : line.substr(0, hash));
		if (content.empty())
			continue;
		const size_t lineNo = ln + 1;
		const std::string where = "(line " + std::to_string(lineNo) + "): ";
		if (content == "[paks]") {
			inPaks = true;
			continue;
		}
		if (content.front() == '[') {
			// v2 dropped [ids] (the per-pak identity tables replaced
			// the manifest's catalog mirror): a v1 manifest dies HERE,
			// loud, naming the section — content recooks.
			err = where + "unknown section '" + content + "'"
				+ (content == "[ids]"
					? " (the [ids] table died with appmanifest 2 — the"
					" paks' own '#ids' identity tables are the catalog"
					" now; recook)"
					: "");
			return false;
		}
		if (inPaks) {
			std::string pak = content;
			if (pak.front() == '"') {
				const size_t close = pak.find('"', 1);
				if (close == std::string::npos) {
					err = where + "unterminated quoted pak path";
					return false;
				}
				pak = pak.substr(1, close - 1);
			}
			out.paks.push_back(pak);
			continue;
		}
		// header directives
		size_t sp = content.find_first_of(" \t");
		const std::string dir = sp == std::string::npos
			? content : content.substr(0, sp);
		const std::string rest = sp == std::string::npos
			? std::string() : Trim(content.substr(sp + 1));
		if (dir == "appmanifest") {
			if (sawVersion) {
				err = where + "duplicate 'appmanifest' line";
				return false;
			}
			bool digits = !rest.empty();
			for (char c : rest)
				if (!std::isdigit(static_cast<unsigned char>(c)))
					digits = false;
			if (!digits || rest.size() > 4) {
				err = where + "'" + rest + "' is not a version number";
				return false;
			}
			out.version = uint32_t(std::stoul(rest));
			sawVersion = true;
		}
		else if (dir == "entry") {
			if (sawEntry) {
				err = where + "duplicate 'entry' line";
				return false;
			}
			// entry <pak> <scene> (either may be quoted)
			std::vector<std::string> toks;
			size_t i = 0;
			while (i < rest.size()) {
				while (i < rest.size() && IsSpace(rest[i]))
					++i;
				if (i < rest.size() && rest[i] == '"') {
					const size_t close = rest.find('"', i + 1);
					if (close == std::string::npos) {
						err = where + "unterminated quoted value";
						return false;
					}
					toks.push_back(rest.substr(i + 1, close - i - 1));
					i = close + 1;
				}
				else {
					size_t j = i;
					while (j < rest.size() && !IsSpace(rest[j]))
						++j;
					if (i < j)
						toks.push_back(rest.substr(i, j - i));
					i = j;
				}
			}
			if (toks.size() != 2) {
				err = where + "'entry' is 'entry <pak-file> <scene-name>'";
				return false;
			}
			out.entryPak = toks[0];
			out.entryScene = toks[1];
			sawEntry = true;
		}
		else if (dir == "catalog") {
			// v2 dropped the `catalog` fingerprint line (the deployed
			// half of the compiled-catalog contract moved into the
			// REGISTRY's record fold — ContentRegistry::
			// RecordFingerprint, read from the paks' own tables).
			err = where + "the 'catalog' line died with appmanifest 2"
				" (the deployed fingerprint is the record registry's"
				" own fold — recook)";
			return false;
		}
		else {
			err = where + "unknown directive '" + dir + "'";
			return false;
		}
	}
	if (!sawVersion) {
		err = "the manifest has no 'appmanifest <version>' line";
		return false;
	}
	if (!sawEntry) {
		err = "the manifest has no 'entry' line";
		return false;
	}
	std::string verr;
	if (!ValidateAppManifest(out, verr)) {
		err = verr;
		return false;
	}
	return true;
}

bool LoadAppManifest(const std::string& path, AppManifest& out,
	std::string& err) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		err = "cannot open the appmanifest: " + path;
		return false;
	}
	std::ostringstream os;
	os << in.rdbuf();
	return LoadAppManifestText(os.str(), out, err);
}

bool SaveAppManifest(const std::string& path, const AppManifest& m,
	std::string& err) {
	{
		std::string verr;
		if (!ValidateAppManifest(m, verr)) {
			err = verr;
			return false;
		}
	}
	std::ostringstream os;
	os << "# Tourmaline Runtime Project\n";
	os << "appmanifest " << kAppManifestVersion << "\n";
	os << "entry " << m.entryPak << " " << m.entryScene << "\n";
	os << "\n[paks]\n";
	for (const std::string& pak : m.paks)
		os << pak << "\n";
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write the appmanifest: " + path;
		return false;
	}
	f << os.str();
	if (!f.good()) {
		err = "failed writing the appmanifest: " + path;
		return false;
	}
	return true;
}

} // namespace smvk
