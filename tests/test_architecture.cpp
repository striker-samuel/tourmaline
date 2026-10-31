// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// architecture_tests: the namespace/RHI BOUNDARY SCANNER (todo item 8,
// section 8c - the regex early-warning form; the full API scanner that
// walks exported signatures/aliases/inherited types is the required
// follow-up, this is its enforceable v1). GPU-free, source-tree-only:
// walks the engine's PUBLIC headers and enforces the boundary rules
// that hold TODAY, so every NEW violation fails CI:
//
//   R1  no public header includes Vulkan headers (<vulkan/...>,
//       <volk.h>, vk_mem_alloc) - the include path is the sharpest
//       leak and it is clean today (stage 2 made the path PRIVATE on
//       the engine target; this guards the header side forever).
//   R2  no public header includes the private backend impl
//       (Vulkan/detail/ or anything under engine/src/) - the boundary
//       rule: smvk::detail::vulkan is never included by public
//       headers.
//   R3  no Vulkan TYPE NAME (Vk[A-Z]...) in public header CODE
//       (comments stripped) - opaque pointers + prose references are
//       the baseline; a typed Vk handle in a signature is new.
//
// The audit's CONFIRMED pre-existing findings (the P0/P1 redesign
// list, todo item 8 sections 5-6) are deliberately NOT flagged here:
// those are tracked work with owners in the todo, and this test
// exists to keep the boundary from getting WORSE while they land.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef TOURMALINE_SOURCE_ROOT
#define TOURMALINE_SOURCE_ROOT "."
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const std::string& name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

namespace fs = std::filesystem;

// The engine's public headers (engine/include/Engine/**.h). The
// boundary's public surface today; the migration (todo 8.4) moves the
// engine-vs-RHI split inside it - the RULES here stay the same.
std::vector<fs::path> PublicHeaders() {
	std::vector<fs::path> out;
	const fs::path root = fs::path(TOURMALINE_SOURCE_ROOT)
		/ "engine" / "include";
	if (!fs::exists(root)) {
		std::cerr << "architecture: no engine/include at " << root
			<< "\n";
		return out;
	}
	std::error_code ec;
	for (const fs::path& p : { root / "Engine", root / "shaders" }) {
		if (!fs::exists(p, ec))
			continue;
		for (const auto& entry : fs::recursive_directory_iterator(p, ec)) {
			if (entry.is_regular_file(ec)
					&& entry.path().extension() == ".h")
				out.push_back(entry.path());
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string ReadText(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Strip // comments, /* */ blocks, and string/char literals (a Vk name
// inside prose or a message string is NOT a typed API leak; the
// scanner wants names in CODE).
std::string StripCommentsAndStrings(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	bool inLineComment = false, inBlockComment = false;
	bool inString = false, inChar = false;
	for (size_t i = 0; i < text.size(); ++i) {
		const char c = text[i];
		const char n = i + 1 < text.size() ? text[i + 1] : '\0';
		if (inLineComment) {
			if (c == '\n') {
				inLineComment = false;
				out.push_back(c);
			}
			continue;
		}
		if (inBlockComment) {
			if (c == '*' && n == '/') {
				inBlockComment = false;
				++i;
			}
			continue;
		}
		if (inString) {
			if (c == '\\') {
				++i; // skip the escaped char
				continue;
			}
			if (c == '"')
				inString = false;
			continue;
		}
		if (inChar) {
			if (c == '\\') {
				++i;
				continue;
			}
			if (c == '\'')
				inChar = false;
			continue;
		}
		if (c == '/' && n == '/') {
			inLineComment = true;
			++i;
			continue;
		}
		if (c == '/' && n == '*') {
			inBlockComment = true;
			++i;
			continue;
		}
		if (c == '"') {
			inString = true;
			continue;
		}
		if (c == '\'') {
			inChar = true;
			continue;
		}
		out.push_back(c);
	}
	return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

int main() {
	const std::vector<fs::path> headers = PublicHeaders();
	Check(!headers.empty(), "arch.public-headers-found");
	if (headers.empty())
		return 1;

	// R1: no Vulkan includes in public headers.
	{
		std::vector<std::string> offenders;
		for (const fs::path& p : headers) {
			const std::string text = ReadText(p);
			if (Contains(text, "#include <vulkan")
					|| Contains(text, "#include <volk.h")
					|| Contains(text, "#include \"volk.h")
					|| Contains(text, "vk_mem_alloc"))
				offenders.push_back(p.generic_string());
		}
		Check(offenders.empty(), "arch.r1-no-vulkan-includes");
		for (const std::string& o : offenders)
			std::cout << "  R1 offender: " << o << "\n";
	}
	// R2: no private-backend impl includes in public headers. Scans
	// the COMMENT-STRIPPED text so prose references to the impl file
	// (the audit trail in comments) do not trip it - only actual
	// #include directives count. (Core/Limits.h's engine_limits.hlsli
	// include is the shared CPU/HLSL layout CONTRACT, deliberately not
	// the private backend - unflagged.)
	{
		std::vector<std::string> offenders;
		for (const fs::path& p : headers) {
			const std::string code =
				StripCommentsAndStrings(ReadText(p));
			size_t at = 0;
			while ((at = code.find("#include", at))
					!= std::string::npos) {
				const size_t lineEnd = code.find('\n', at);
				const std::string line = code.substr(
					at, lineEnd == std::string::npos
						? std::string::npos : lineEnd - at);
				if (Contains(line, "Vulkan/detail")
						|| Contains(line, "vulkan_impl")
						|| Contains(line, "../../src")
						|| Contains(line, "../../../src"))
					offenders.push_back(p.generic_string()
						+ ": " + line.substr(0, 72));
				at = lineEnd == std::string::npos
					? code.size() : lineEnd;
			}
		}
		Check(offenders.empty(), "arch.r2-no-impl-includes");
		for (const std::string& o : offenders)
			std::cout << "  R2 offender: " << o << "\n";
	}
	// R3: no Vulkan type names in public header CODE (comments +
	// strings stripped). The typed-handle baseline today is EMPTY -
	// the audit's opaque-pointer findings are `void*`/comments, so any
	// hit is a new leak.
	{
		std::vector<std::string> offenders;
		for (const fs::path& p : headers) {
			const std::string code =
				StripCommentsAndStrings(ReadText(p));
			// \bVk[A-Z] identifier in code (declarations, casts,
			// parameter types). The core handle macros (VK_NULL_HANDLE
			// etc.) are all-caps - excluded by [A-Z] followed by [a-z]
			// lower somewhere: match Vk[A-Z][A-Za-z]* and require the
			// name not to be part of an all-caps macro token.
			// The typed-handle baseline today is EMPTY (the audit's Vk
			// mentions are comments/opaque void*) - a vector so it can
			// grow without the zero-size-array problem when a
			// sanctioned exception ever lands.
			static const std::vector<std::string> kBaseline = {};
			size_t at = 0;
			while ((at = code.find("Vk", at)) != std::string::npos) {
				const bool wordStart =
					at == 0
					|| !(std::isalnum(unsigned char(code[at - 1]))
						|| code[at - 1] == '_');
				const bool vulkanType =
					code.size() > at + 2
					&& std::isupper(unsigned char(code[at + 2]));
				if (wordStart && vulkanType) {
					// Grab the token + baseline check.
					size_t end = at + 2;
					while (end < code.size()
							&& (std::isalnum(unsigned char(code[end]))
								|| code[end] == '_'))
						++end;
					const std::string tok = code.substr(at, end - at);
					bool known = false;
					for (const std::string& b : kBaseline)
						if (tok == b) {
							known = true;
							break;
						}
					if (!known) {
						offenders.push_back(p.generic_string()
							+ ": " + tok);
						break; // one report per file is enough
					}
				}
				at += 2;
			}
		}
		Check(offenders.empty(), "arch.r3-no-vk-types-in-code");
		for (const std::string& o : offenders)
			std::cout << "  R3 offender: " << o << "\n";
	}
	// R4: THE UPWARD DEPENDENCY RULE (the audit's section 6, as a
	// scanner guard): the public RHI headers (smvk.h + RHI/*.h) must
	// not include ENGINE-DOMAIN headers (scene/app/gi/placement/
	// audio/camera/profile/assets/worldfields/utils) - the RHI stays
	// unaware of the engine above it. BASELINE (the tracked pre-
	// existing findings, NOT new leaks):
	//   * resource_manager.h -> Engine/Renderer/spirv_reflection.h
	//     (the section 6 finding: the shader-module creation surface
	//     needs the reflection records; the P1 fix is moving the
	//     reflection types or the dependency behind an adapter).
	//   * Memory/handle.h + pool.h + string_view.h: the generational
	//     pool/handle vocabulary is shared infrastructure the RHI
	//     pools are built on (smvk-scope, deliberately allowed - the
	//     audit table moves memory.h the ARENA layer, not these).
	{
		std::vector<std::string> offenders;
		const fs::path rhiRoot = fs::path(TOURMALINE_SOURCE_ROOT)
			/ "engine" / "include" / "Engine";
		std::vector<fs::path> rhiHeaders;
		for (const char* name :
				{ "smvk.h", "RHI/resource_manager.h",
				  "RHI/swapchain.h", "RHI/DescriptorLimits.h",
				  "RHI/FrameConfig.h" }) {
			const fs::path p = rhiRoot / name;
			if (fs::exists(p))
				rhiHeaders.push_back(p);
		}
		// Engine-domain include prefixes the RHI must never pull.
		static const char* kForbidden[] = {
			"Engine/Scene/", "Engine/App/", "Engine/GI/",
			"Engine/Placement/", "Engine/Audio/", "Engine/Camera/",
			"Engine/Profile/", "Engine/WorldFields/",
			"Engine/Utils/", "Engine/ImGui/",
			"Engine/Asset/tour.h", "Engine/Asset/pak.h",
			"Engine/Asset/touranim.h",
			"Engine/Asset/appmanifest.h",
		};
		for (const fs::path& p : rhiHeaders) {
			const std::string code =
				StripCommentsAndStrings(ReadText(p));
			size_t at = 0;
			while ((at = code.find("#include", at))
					!= std::string::npos) {
				const size_t lineEnd = code.find('\n', at);
				const std::string line = code.substr(
					at, lineEnd == std::string::npos
						? std::string::npos : lineEnd - at);
				for (const char* bad : kForbidden) {
					if (Contains(line, bad))
						offenders.push_back(
							p.generic_string() + ": "
							+ line.substr(0, 72));
				}
				at = lineEnd == std::string::npos
					? code.size() : lineEnd;
			}
		}
		Check(offenders.empty(), "arch.r4-rhi-no-engine-includes");
		for (const std::string& o : offenders)
			std::cout << "  R4 offender: " << o << "\n";
	}
	// R5: THE TRANSITIVE upward dependency (section 8d's early-warning
	// form): walk the FULL include graph from the public RHI headers
	// and fail on any engine-domain header reachable TRANSITIVELY (a
	// direct include ban is R4; a chain like RHI -> allowed-middle ->
	// Engine/App/... is this rule). Same baseline/allowed vocabulary
	// as R4. Depth-capped (a diamond graph would revisit; the cap is
	// generous for header chains).
	{
		std::vector<std::string> offenders;
		const fs::path incRoot = fs::path(TOURMALINE_SOURCE_ROOT)
			/ "engine" / "include" / "Engine";
		auto engineIncludes = [](const std::string& code) {
			std::vector<std::string> out;
			std::string at(code);
			std::smatch m;
			static const std::regex re(
				"#include\\s*<Engine/([^>]+)>");
			auto begin = code.cbegin();
			while (std::regex_search(begin, code.cend(), m, re)) {
				out.push_back(m[1].str());
				begin = m.suffix().first;
			}
			return out;
		};
		// forbidden prefixes (same as R4) + the baseline edges
		// (from-file, to-include) that are tracked findings.
		static const char* kForbiddenPfx[] = {
			"Scene/", "App/", "GI/", "Placement/", "Audio/",
			"Camera/", "Profile/", "WorldFields/", "Utils/",
			"ImGui/", "Asset/tour.h", "Asset/pak.h",
			"Asset/touranim.h", "Asset/appmanifest.h",
		};
		struct BaselineEdge { const char* from; const char* to; };
		static const BaselineEdge kBaseline[] = {
			// resource_manager.h -> spirv_reflection.h (section 6,
			// the P1 adapter fix) - spirv_reflection itself only
			// pulls smvk.h, so the chain stops there.
			{ "RHI/resource_manager.h", "Renderer/spirv_reflection.h" },
		};
		auto isForbidden = [](const std::string& inc) {
			for (const char* p : kForbiddenPfx)
				if (inc.rfind(p, 0) == 0)
					return true;
			return false;
		};
		auto isBaseline = [](const std::string& from,
				const std::string& inc) {
			for (const BaselineEdge& b : kBaseline) {
				const std::string f = b.from;
				if (from == f
						&& inc.rfind(b.to, 0) == 0)
					return true;
			}
			return false;
		};
		// BFS from the RHI roots, over the STRIPPED code (comments
		// gone so commented-out includes never count). VISITED-set
		// keyed on the walked FILE (the include graph is cyclic -
		// smvk.h <-> resource_manager.h - and without the set the
		// re-walk burns the hop budget before the deep edges).
		std::vector<std::string> roots = { "smvk.h",
			"RHI/resource_manager.h", "RHI/swapchain.h",
			"RHI/DescriptorLimits.h", "RHI/FrameConfig.h" };
		std::vector<std::pair<std::string, std::string>> queue;
		std::vector<std::string> visited;
		auto wasVisited = [&visited](const std::string& inc) {
			return std::find(visited.begin(), visited.end(), inc)
				!= visited.end();
		};
		for (const std::string& r : roots) {
			const fs::path p = incRoot / r;
			if (fs::exists(p)) {
				visited.push_back(r);
				for (const std::string& inc : engineIncludes(
						StripCommentsAndStrings(ReadText(p))))
					queue.push_back({ r, inc });
			}
		}
		const size_t kMaxHops = 4096;
		size_t hops = 0;
		while (!queue.empty() && hops++ < kMaxHops) {
			const auto [from, inc] = queue.back();
			queue.pop_back();
			if (isForbidden(inc) && !isBaseline(from, inc)) {
				offenders.push_back(from + " -> " + inc);
				continue; // report, don't walk further
			}
			const fs::path next = incRoot / inc;
			std::error_code ec;
			if (!fs::exists(next, ec) || inc.rfind("Renderer/", 0) == 0)
				continue; // don't walk into Renderer/ (the gray zone)
			if (wasVisited(inc))
				continue;
			visited.push_back(inc);
			for (const std::string& deeper : engineIncludes(
					StripCommentsAndStrings(ReadText(next))))
				queue.push_back({ inc, deeper });
		}
		Check(offenders.empty(), "arch.r5-rhi-transitively-clean");
		for (const std::string& o : offenders)
			std::cout << "  R5 offender: " << o << "\n";
	}

	std::cout << "architecture_tests: " << (g_checks - g_failures)
		<< "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
