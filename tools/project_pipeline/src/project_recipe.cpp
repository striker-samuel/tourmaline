// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The recipe parser (see project_recipe.h for the grammar). Pure text
// work: tokenize, validate schema, type out. Every failure carries the
// 1-based line + the offending directive; filesystem discovery and
// stage execution are NOT this file's business (recipe_resolver.cpp /
// build_pipeline.cpp own those).

#include <project_pipeline/project_recipe.h>

#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace smvk::project {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string Trim(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && IsSpace(s[b]))
		++b;
	while (e > b && IsSpace(s[e - 1]))
		--e;
	return s.substr(b, e - b);
}

// The comment start: '#' OUTSIDE a quoted value (a '#' inside quotes is
// a legitimate filename character).
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

// Splits the first whitespace-delimited token off `line`.
bool SplitFirst(const std::string& line, std::string& directive,
	std::string& rest) {
	size_t i = 0;
	while (i < line.size() && IsSpace(line[i]))
		++i;
	size_t j = i;
	while (j < line.size() && !IsSpace(line[j]))
		++j;
	if (i == j)
		return false;
	directive = line.substr(i, j - i);
	rest = Trim(line.substr(j));
	return true;
}

std::vector<std::string> SplitTokens(const std::string& s) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && IsSpace(s[i]))
			++i;
		size_t j = i;
		while (j < s.size() && !IsSpace(s[j]))
			++j;
		if (i < j)
			out.push_back(s.substr(i, j - i));
		i = j;
	}
	return out;
}

bool Fail(size_t line, const std::string& directive, const std::string& msg,
	RecipeDiagnostic& err) {
	err.line = line;
	err.directive = directive;
	err.message = msg;
	return false;
}

// Unquotes a value: '"..."' strips the pair (no escapes by design —
// keep the grammar dumb); anything after the closing quote is junk.
// Unquoted values stand as-is (they may contain spaces — quoting is
// only needed for leading quotes, '#', and trailing blanks).
bool Unquote(const std::string& raw, size_t line, const std::string& directive,
	std::string& value, RecipeDiagnostic& err) {
	if (raw.empty())
		return Fail(line, directive,
			"'" + directive + "' requires a value", err);
	if (raw.front() != '"') {
		value = raw;
		return true;
	}
	const size_t close = raw.find('"', 1);
	if (close == std::string::npos)
		return Fail(line, directive,
			"unterminated quoted value in '" + directive + "'", err);
	value = raw.substr(1, close - 1);
	const std::string tail = Trim(raw.substr(close + 1));
	if (!tail.empty())
		return Fail(line, directive,
			"unexpected text after the quoted value: '" + tail + "'", err);
	return true;
}

// A recipe path: PROJECT-RELATIVE, forward slashes (backslashes
// normalized — Windows-authored recipes), no empty/'.'/'..' segments,
// never absolute (leading slash or drive letter). Wildcards are legal
// only where the caller allows (include globs).
bool ValidatePath(const std::string& raw, bool allowWildcards,
	std::string& normalized, std::string& why) {
	std::string s = raw;
	for (char& c : s)
		if (c == '\\')
			c = '/';
	if (s.empty()) {
		why = "the path is empty";
		return false;
	}
	if (s.front() == '/') {
		why = "absolute paths are not allowed (recipe paths are"
			" project-relative): '" + raw + "'";
		return false;
	}
	if (s.size() >= 2 && s[1] == ':'
			&& std::isalpha(static_cast<unsigned char>(s[0]))) {
		why = "drive-letter paths are not allowed (recipe paths are"
			" project-relative): '" + raw + "'";
		return false;
	}
	for (size_t start = 0; start <= s.size();) {
		const size_t end = s.find('/', start);
		const std::string seg = s.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if (seg.empty()) {
			why = "empty path segment ('//' or a trailing '/') in '"
				+ raw + "'";
			return false;
		}
		if (seg == "." || seg == "..") {
			why = "escaping segments ('..' / '.') are not allowed: '"
				+ raw + "'";
			return false;
		}
		if (!allowWildcards && (seg.find('*') != std::string::npos
				|| seg.find('?') != std::string::npos)) {
			why = "wildcards are only valid in 'include' patterns: '"
				+ raw + "'";
			return false;
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	normalized = s;
	return true;
}

bool ValidPakId(const std::string& id, std::string& why) {
	if (id.empty()) {
		why = "the pak id is empty";
		return false;
	}
	for (char c : id) {
		if (c == '[' || c == ']' || c == '"' || c == '/' || c == '\\'
				|| c == '#') {
			why = "the pak id '" + id + "' contains a reserved character";
			return false;
		}
	}
	return true;
}

bool HasTourExtension(const std::string& path) {
	const size_t dot = path.rfind('.');
	return dot != std::string::npos && path.substr(dot) == ".tour";
}

// ---- dependency-cycle detection (schema: unknown targets are checked
// BEFORE this runs, so index.at is safe). On a back edge the returned
// chain is the closed loop "a -> ... -> a". ---------------------------
bool FindCycle(const std::vector<RecipePak>& paks,
	const std::map<std::string, size_t>& index, size_t v,
	std::vector<int>& color, std::vector<std::string>& chain) {
	color[v] = 1;
	chain.push_back(paks[v].id);
	for (const std::string& dep : paks[v].depends) {
		const size_t w = index.at(dep);
		if (color[w] == 1) {
			// trim the DFS-root prefix so the report is exactly the loop
			size_t from = 0;
			while (from < chain.size() && chain[from] != dep)
				++from;
			chain.erase(chain.begin(), chain.begin() + long(from));
			chain.push_back(dep); // closes the loop: a -> ... -> a
			return true;
		}
		if (color[w] == 0 && FindCycle(paks, index, w, color, chain))
			return true;
	}
	color[v] = 2;
	chain.pop_back();
	return false;
}

// A forward-referenced 'depends' (legal at parse time, resolved in the
// post-pass).
struct DependsNote {
	std::string pak;    // the declaring pak's id
	std::string target; // the declared dependency's id
	size_t line;        // the depends line
};

bool ParseText(const std::string& text, ProjectRecipe& out,
	RecipeDiagnostic& err) {
	out = ProjectRecipe{};

	// Split into physical lines ('\n'; '\r' stripped per line — CRLF
	// and LF are both grammar-legal). A leading UTF-8 BOM is tolerated.
	std::string body = text;
	if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF
			&& static_cast<unsigned char>(body[1]) == 0xBB
			&& static_cast<unsigned char>(body[2]) == 0xBF)
		body = body.substr(3);
	std::vector<std::string> raws;
	{
		size_t start = 0;
		while (start <= body.size()) {
			const size_t end = body.find('\n', start);
			if (end == std::string::npos) {
				if (start < body.size())
					raws.push_back(body.substr(start));
				break;
			}
			raws.push_back(body.substr(start, end - start));
			start = end + 1;
		}
	}

	bool sawVersion = false, sawProject = false, sawEntry = false;
	bool sawDependenciesLine = false; // per open [pak] section
	std::set<std::string> bakeSeen;
	std::vector<DependsNote> dependsNotes;
	RecipePak current; // the open [pak] section (pushed on the next open)
	bool inPak = false;

	for (size_t ln = 0; ln < raws.size(); ++ln) {
		std::string line = raws[ln];
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t hash = CommentStart(line);
		std::string content =
			Trim(hash == std::string::npos ? line : line.substr(0, hash));
		if (content.empty())
			continue;
		const size_t lineNo = ln + 1;

		// ---- a [section] header ------------------------------------
		if (content.front() == '[') {
			if (inPak) {
				out.paks.push_back(current);
				current = RecipePak{};
				inPak = false;
				sawDependenciesLine = false;
			}
			if (content.back() != ']')
				return Fail(lineNo, content,
					"malformed section header (missing ']'): '" + content
						+ "'", err);
			const std::string inner =
				Trim(content.substr(1, content.size() - 2));
			std::string head, rest;
			if (!SplitFirst(inner, head, rest) || head != "pak")
				return Fail(lineNo, content,
					"unknown section '" + (head.empty() ? inner : head)
						+ "' (grammar v1 has only [pak <id>])", err);
			std::string id;
			if (!Unquote(rest, lineNo, content, id, err))
				return false;
			std::string why;
			if (!ValidPakId(id, why))
				return Fail(lineNo, content, why, err);
			for (const RecipePak& p : out.paks)
				if (p.id == id)
					return Fail(lineNo, content,
						"duplicate pak id '" + id + "' (first declared on"
							" line " + std::to_string(p.line) + ")", err);
			current.id = id;
			current.line = lineNo;
			inPak = true;
			continue;
		}

		// ---- a directive -------------------------------------------
		std::string directive, rest;
		SplitFirst(content, directive, rest);

		// Version-first rule: the recipe opens with 'recipe <version>'.
		if (!sawVersion && directive != "recipe")
			return Fail(lineNo, directive,
				"the recipe must open with 'recipe <version>' (found '"
					+ directive + "')", err);

		if (directive == "recipe") {
			if (sawVersion)
				return Fail(lineNo, "recipe",
					"duplicate 'recipe' version line", err);
			const std::vector<std::string> toks = SplitTokens(rest);
			if (toks.size() != 1)
				return Fail(lineNo, "recipe",
					"'recipe <version>' takes exactly one number", err);
			bool digits = !toks[0].empty();
			for (char c : toks[0])
				if (!std::isdigit(static_cast<unsigned char>(c)))
					digits = false;
			if (!digits || toks[0].size() > 4)
				return Fail(lineNo, "recipe",
					"'" + toks[0] + "' is not a version number", err);
			const uint32_t v = uint32_t(std::stoul(toks[0]));
			if (v != kRecipeGrammarVersion)
				return Fail(lineNo, "recipe",
					"unsupported recipe version " + toks[0]
						+ " (this tool reads version "
						+ std::to_string(kRecipeGrammarVersion) + ")", err);
			out.version = v;
			sawVersion = true;
		}
		else if (directive == "project") {
			if (inPak)
				return Fail(lineNo, "project",
					"'project' belongs in the recipe header (before any"
						" [pak] section)", err);
			if (sawProject)
				return Fail(lineNo, "project",
					"duplicate 'project' line", err);
			std::string name;
			if (!Unquote(rest, lineNo, directive, name, err))
				return false;
			if (name.empty())
				return Fail(lineNo, "project",
					"'project' requires a name", err);
			out.projectName = name;
			sawProject = true;
		}
		else if (directive == "entry") {
			if (inPak)
				return Fail(lineNo, "entry",
					"'entry' belongs in the recipe header (before any"
						" [pak] section)", err);
			if (sawEntry)
				return Fail(lineNo, "entry",
					"duplicate 'entry' line (a recipe has one entry"
						" scene)", err);
			std::string value;
			if (!Unquote(rest, lineNo, directive, value, err))
				return false;
			std::string normalized, why;
			if (!ValidatePath(value, false, normalized, why))
				return Fail(lineNo, directive, why, err);
			if (!HasTourExtension(normalized))
				return Fail(lineNo, directive,
					"'entry' must name a .tour scene: '" + normalized
						+ "'", err);
			out.entry = normalized;
			sawEntry = true;
		}
		else if (directive == "bake") {
			if (inPak)
				return Fail(lineNo, "bake",
					"'bake' belongs in the recipe header (before any"
						" [pak] section)", err);
			const std::vector<std::string> toks = SplitTokens(rest);
			if (toks.empty() || toks.size() > 2)
				return Fail(lineNo, "bake",
					"'bake <gi|cubemaps> [on|off]' takes one or two"
						" words", err);
			if (toks[0] != "gi" && toks[0] != "cubemaps")
				return Fail(lineNo, "bake",
					"unknown bake target '" + toks[0] + "' (only gi and"
						" cubemaps exist)", err);
			bool on = true;
			if (toks.size() == 2) {
				if (toks[1] != "on" && toks[1] != "off")
					return Fail(lineNo, "bake",
						"the bake switch is 'on' or 'off' (found '"
							+ toks[1] + "')", err);
				on = toks[1] == "on";
			}
			if (!bakeSeen.insert(toks[0]).second)
				return Fail(lineNo, "bake",
					"duplicate bake line for '" + toks[0] + "'", err);
			if (toks[0] == "gi")
				out.bakeGi = on;
			else
				out.bakeCubemaps = on;
		}
		else if (directive == "output") {
			if (!inPak)
				return Fail(lineNo, "output",
					"'output' only exists inside a [pak] section", err);
			if (!current.output.empty())
				return Fail(lineNo, "output",
					"duplicate 'output' in [pak " + current.id + "]", err);
			std::string value;
			if (!Unquote(rest, lineNo, directive, value, err))
				return false;
			std::string normalized, why;
			if (!ValidatePath(value, false, normalized, why))
				return Fail(lineNo, directive, why, err);
			current.output = normalized;
		}
		else if (directive == "include") {
			if (!inPak)
				return Fail(lineNo, "include",
					"'include' only exists inside a [pak] section", err);
			std::string value;
			if (!Unquote(rest, lineNo, directive, value, err))
				return false;
			std::string normalized, why;
			if (!ValidatePath(value, true, normalized, why))
				return Fail(lineNo, directive, why, err);
			current.includes.push_back(normalized);
		}
		else if (directive == "scene") {
			if (!inPak)
				return Fail(lineNo, "scene",
					"'scene' only exists inside a [pak] section", err);
			if (!current.scene.empty())
				return Fail(lineNo, "scene",
					"duplicate 'scene' in [pak " + current.id + "]"
						" (one scene per pak in grammar v1)", err);
			std::string value;
			if (!Unquote(rest, lineNo, directive, value, err))
				return false;
			std::string normalized, why;
			if (!ValidatePath(value, false, normalized, why))
				return Fail(lineNo, directive, why, err);
			if (!HasTourExtension(normalized))
				return Fail(lineNo, directive,
					"'scene' must name a .tour scene: '" + normalized
						+ "'", err);
			current.scene = normalized;
		}
		else if (directive == "depends") {
			if (!inPak)
				return Fail(lineNo, "depends",
					"'depends' only exists inside a [pak] section", err);
			std::string id;
			if (!Unquote(rest, lineNo, directive, id, err))
				return false;
			std::string why;
			if (!ValidPakId(id, why))
				return Fail(lineNo, directive, why, err);
			if (id == current.id)
				return Fail(lineNo, directive,
					"[pak " + id + "] cannot depend on itself", err);
			for (const std::string& d : current.depends)
				if (d == id)
					return Fail(lineNo, directive,
						"duplicate 'depends " + id + "' in [pak "
							+ current.id + "]", err);
			current.depends.push_back(id);
			dependsNotes.push_back({ current.id, id, lineNo });
		}
		else if (directive == "dependencies") {
			if (!inPak)
				return Fail(lineNo, "dependencies",
					"'dependencies' only exists inside a [pak] section",
					err);
			if (sawDependenciesLine)
				return Fail(lineNo, "dependencies",
					"duplicate 'dependencies' line in [pak " + current.id
						+ "]", err);
			std::string value;
			if (!Unquote(rest, lineNo, directive, value, err))
				return false;
			if (value != "transitive" && value != "none")
				return Fail(lineNo, directive,
					"'dependencies' is 'transitive' or 'none' (found '"
						+ value + "')", err);
			current.dependenciesTransitive = value == "transitive";
			sawDependenciesLine = true;
		}
		else {
			return Fail(lineNo, directive,
				"unknown directive '" + directive + "'", err);
		}
	}
	if (inPak)
		out.paks.push_back(current);

	// ---- post-pass: schema completeness (deterministic error order) --
	if (!sawVersion)
		return Fail(0, "recipe",
			"the recipe is empty (it must open with 'recipe "
				+ std::to_string(kRecipeGrammarVersion) + "')", err);
	if (!sawEntry)
		return Fail(0, "entry",
			"the recipe has no 'entry' scene (required)", err);
	if (out.paks.empty())
		return Fail(0, "[pak]",
			"the recipe declares no [pak] sections", err);
	for (const RecipePak& p : out.paks) {
		if (p.output.empty())
			return Fail(p.line, "output",
				"[pak " + p.id + "] has no 'output' file", err);
		if (!p.HasContentSource())
			return Fail(p.line, "include",
				"[pak " + p.id + "] owns no content (add 'scene <tour>'"
					" or 'include <glob>')", err);
	}
	// unique outputs (two paks never write the same file)
	{
		std::map<std::string, std::string> outputOwner;
		for (const RecipePak& p : out.paks) {
			auto it = outputOwner.find(p.output);
			if (it != outputOwner.end())
				return Fail(p.line, "output",
					"output '" + p.output + "' is declared by both '"
						+ it->second + "' and '" + p.id + "'", err);
			outputOwner[p.output] = p.id;
		}
	}
	// unique scene ownership (a scene ships in exactly one pak) + the
	// entry must be one of the owned scenes (it has to ship somewhere)
	{
		std::map<std::string, std::string> sceneOwner;
		for (const RecipePak& p : out.paks) {
			if (p.scene.empty())
				continue;
			auto it = sceneOwner.find(p.scene);
			if (it != sceneOwner.end())
				return Fail(p.line, "scene",
					"scene '" + p.scene + "' is owned by both '"
						+ it->second + "' and '" + p.id
						+ "' (a scene ships once)", err);
			sceneOwner[p.scene] = p.id;
		}
		if (sceneOwner.find(out.entry) == sceneOwner.end())
			return Fail(0, "entry",
				"the entry scene '" + out.entry + "' is not owned by any"
					" [pak] 'scene' directive (the entry has to ship)",
				err);
	}
	// depends targets exist (forward references are legal at parse
	// time; this is where they resolve)
	for (const DependsNote& note : dependsNotes) {
		bool found = false;
		for (const RecipePak& p : out.paks)
			if (p.id == note.target) {
				found = true;
				break;
			}
		if (!found)
			return Fail(note.line, "depends",
				"[pak " + note.pak + "] depends on unknown pak '"
					+ note.target + "'", err);
	}
	// dependency cycles
	{
		std::map<std::string, size_t> index;
		for (size_t i = 0; i < out.paks.size(); ++i)
			index[out.paks[i].id] = i;
		std::vector<int> color(out.paks.size(), 0);
		for (size_t i = 0; i < out.paks.size(); ++i) {
			if (color[i] != 0)
				continue;
			std::vector<std::string> chain;
			if (FindCycle(out.paks, index, i, color, chain)) {
				std::string cyc;
				for (size_t k = 0; k < chain.size(); ++k) {
					if (k)
						cyc += " -> ";
					cyc += chain[k];
				}
				return Fail(0, "depends",
					"dependency cycle: " + cyc, err);
			}
		}
	}
	return true;
}

} // namespace

const RecipePak* ProjectRecipe::FindPak(const std::string& id) const {
	for (const RecipePak& p : paks)
		if (p.id == id)
			return &p;
	return nullptr;
}

std::string RecipeDiagnostic::Format(const std::string& file) const {
	std::ostringstream os;
	os << file;
	if (line != 0)
		os << "(" << line << ")";
	os << ": ";
	if (!directive.empty())
		os << "[" << directive << "] ";
	os << message;
	return os.str();
}

bool ParseProjectRecipeText(const std::string& text, ProjectRecipe& out,
	RecipeDiagnostic& err) {
	ProjectRecipe parsed;
	if (!ParseText(text, parsed, err))
		return false; // `out` untouched on failure — never half-populated
	out = std::move(parsed);
	return true;
}

bool ParseProjectRecipe(const std::string& path, ProjectRecipe& out,
	RecipeDiagnostic& err) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		err = RecipeDiagnostic{ 0, "",
			"cannot open the recipe: " + path };
		return false;
	}
	std::ostringstream os;
	os << in.rdbuf();
	return ParseProjectRecipeText(os.str(), out, err);
}

} // namespace smvk::project
