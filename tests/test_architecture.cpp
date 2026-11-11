// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// architecture_tests: the namespace/RHI BOUNDARY SCANNER (todo item 8,
// section 8c - the regex early-warning form; the full API scanner that
// walks exported signatures/aliases/inherited types via AST remains the
// required follow-up, this is its enforceable v1+). GPU-free,
// source-tree-only: walks the engine's PUBLIC headers and enforces the
// boundary rules that hold TODAY, so every NEW violation fails CI:
//
//   R1 — Vulkan Include Guard: no public header includes Vulkan headers (<vulkan/...>,
//       <volk.h>, vk_mem_alloc) - the include path is the sharpest
//       leak and it is clean today (stage 2 made the path PRIVATE on
//       the engine target; this guards the header side forever).
//   R2 — Private Implementation Guard: no public header includes the private backend impl
//       (Vulkan/detail/ or anything under engine/src/). Scans with
//       string contents PRESERVED: a quoted #include "..." path is a
//       real include directive, not prose - stripping quoted strings
//       (the audit's scanner weakness) let quoted paths evade this.
//   R3 — Vulkan Type Leakage Guard: no Vulkan TYPE NAME (Vk[A-Z]...) in public header CODE
//       (comments stripped) - opaque pointers + prose references are
//       the baseline; a typed Vk handle in a signature is new.
//   R4 — Direct RHI Dependency Guard: no direct upward include from the public RHI headers
//       (smvk.h + backend.h + RHI/*) into engine-domain headers.
//   R5 — Transitive RHI Dependency Guard: no TRANSITIVE upward include from the same RHI roots -
//   the
//       full include graph is walked, INCLUDING Renderer/ headers
//       (the audit's weakness: the walk deliberately stopped at the
//       gray zone; a chain RHI -> Renderer/foo.h -> Engine/App/...
//       is now caught).
//   R6 — Broad Namespace Import Guard: no broad `using namespace ::smvk` import in ANY public
//   header
//       (the audit's finding 1: an engine namespace absorbing the
//       whole RHI namespace hides incomplete migrations and makes
//       name lookup ambiguous). BASELINE-GATED, exact both ways:
//       the landed migration stages keep exactly their existing
//       directives; a NEW or GROWN import fails, and a RETIRED one
//       fails until the baseline entry is deleted (the shrink-only
//       ratchet - the baseline IS the exception list, and it may only
//       shrink, never grow).
//   R7 — Legacy Namespace Compatibility Guard: COMPAT ALIAS CONTAINMENT (the audit's finding 4):
//   the
//       migration's `using ::tour::*` / `namespace X = ::tour...`
//       re-export blocks keep engine symbols reachable through
//       namespace smvk (smvk::World, smvk::PakReader...). They are
//       confined to the exact per-file counts below: a new compat
//       block anywhere, or a grown one, fails CI; retired aliases
//       fail until the baseline shrinks. New engine-owned types
//       declared DIRECTLY inside namespace smvk (not via alias - the
//       audit's finding 3, e.g. mesh.h's MaterialValues) are the
//       declaration-migration work in todo 8.4; detecting those
//       textually without an AST is the documented follow-up gap.
//   R8 — RHI Implementation Exposure Guard: RHI IMPLEMENTATION-STATE EXPOSURE (the audit's findings
//   2+5,
//       regex form): the concrete RHI implementation vocabulary in
//       the ENGINE-facing public headers - ResourceManager*/
//       Swapchain*/RenderGraph*/Framebuffer* pointer types,
//       LinearPtr<RHI-type> app members, gfx::State, and the App
//       RHI getters. Per-(file,token) baseline, exact both ways:
//       today's exposure is frozen; any NEW or GROWN occurrence is a
//       review event (a deliberate baseline addition), any retired
//       one shrinks the baseline. The RHI's OWN headers (smvk.h,
//       backend.h, RHI/*) are excluded - the RHI exposing the RHI
//       is not a boundary violation. The engine is ALLOWED to call
//       RHI operations; what this freezes is the app/renderer API
//       carrying implementation state. ShaderProgram (the
//       rendergraph pass-builder family) is deliberately not in the
//       token set: it is smvk-owned vocabulary throughout the
//       Rendergraph/ headers today and would baseline most of that
//       directory - it joins when the todo 8.5 renderer-interface
//       split makes the distinction meaningful.
//
// BASELINE DISCIPLINE (R6/R7/R8/R9): every baseline below is an EXACT
// count, checked in both directions. A grown count or a new file is a
// violation; a shrunk count is a STALE BASELINE failure that says
// "shrink the list" - retiring compat surface REQUIRES deleting its
// baseline row in the same commit, which is what keeps the list
// honest. The audit's weakness "known violations are excluded rather
// than gated behind a shrinking, explicit exception list" is exactly
// what this fixes: the exceptions are enumerated, they can only
// shrink, and CI refuses both growth AND silent staling.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
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
    } else {
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
    const fs::path root = fs::path(TOURMALINE_SOURCE_ROOT) / "engine" / "include";
    if (!fs::exists(root)) {
        std::cerr << "architecture: no engine/include at " << root << "\n";
        return out;
    }
    std::error_code ec;
    for (const fs::path& p : {root / "Engine", root / "shaders"}) {
        if (!fs::exists(p, ec))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(p, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".h")
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

// Strip comments but PRESERVE string/char literal contents. The
// include scanners (R1/R2/R4/R5) must see quoted #include "..."
// paths - those are real directives, and the previous full strip let
// them evade the checks (the audit's scanner weakness 5).
std::string StripCommentsKeepStrings(const std::string& text) {
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
            out.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(text[i + 1]);
                ++i; // keep the escaped char verbatim
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (inChar) {
            out.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(text[i + 1]);
                ++i;
            } else if (c == '\'') {
                inChar = false;
            }
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
            out.push_back(c);
            continue;
        }
        if (c == '\'') {
            inChar = true;
            out.push_back(c);
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

size_t CountMatches(const std::string& hay, const std::regex& re) {
    size_t n = 0;
    auto begin = hay.cbegin();
    std::smatch m;
    while (std::regex_search(begin, hay.cend(), m, re)) {
        ++n;
        begin = m.suffix().first;
    }
    return n;
}

// Every 'namespace smvk' / 'namespace smvk::sub' block in STRIPPED
// code (strings+comments gone, so braces in literals cannot break
// the matching), as [start,end) index pairs. R7 scopes its
// compat-alias scan to these regions: a using/alias re-exporting
// ::tour symbols is the compat mechanism only when it lives inside
// namespace smvk.
std::vector<std::pair<size_t, size_t>> SmvkRegions(const std::string& code) {
    std::vector<std::pair<size_t, size_t>> out;
    static const std::regex re("namespace\\s+smvk(\\s*::\\s*[A-Za-z_][A-Za-z0-9_]*)*\\s*\\{");
    auto begin = code.cbegin();
    std::smatch m;
    while (std::regex_search(begin, code.cend(), m, re)) {
        const size_t start = (size_t)std::distance(code.cbegin(), m[0].first);
        // m[0] ends with the opening '{' - brace-match to its close,
        // starting AFTER the opener (it is already counted by depth=1).
        size_t i = start + m[0].length();
        int depth = 1;
        while (i < code.size() && depth > 0) {
            if (code[i] == '{')
                ++depth;
            else if (code[i] == '}')
                --depth;
            ++i;
        }
        out.push_back({start, i});
        begin = m.suffix().first;
    }
    return out;
}

// Namespace blocks of ANY name (not just smvk), merged: R9 needs the
// COMPLEMENT (global-scope segments) of these regions.
std::vector<std::pair<size_t, size_t>> AllNamespaceRegions(const std::string& code) {
    std::vector<std::pair<size_t, size_t>> out;
    static const std::regex re("namespace\\s*([A-Za-z_][A-Za-z0-9_]*(\\s*::\\s*"
                               "[A-Za-z_][A-Za-z0-9_]*)*)?\\s*\\{");
    auto begin = code.cbegin();
    std::smatch m;
    while (std::regex_search(begin, code.cend(), m, re)) {
        const size_t start = (size_t)std::distance(code.cbegin(), m[0].first);
        size_t i = start + m[0].length();
        int depth = 1;
        while (i < code.size() && depth > 0) {
            if (code[i] == '{')
                ++depth;
            else if (code[i] == '}')
                --depth;
            ++i;
        }
        out.push_back({start, i});
        begin = m.suffix().first;
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

int main() {
    const std::vector<fs::path> headers = PublicHeaders();
    Check(!headers.empty(), "arch.public-headers-found");
    if (headers.empty())
        return 1;

    // Engine-relative paths (the baselines are written against these,
    // e.g. "App/world.h" for engine/include/Engine/App/world.h).
    const fs::path incRoot = fs::path(TOURMALINE_SOURCE_ROOT) / "engine" / "include" / "Engine";
    auto rel = [&incRoot](const fs::path& p) {
        return p.lexically_relative(incRoot).generic_string();
    };
    std::set<std::string> scanned;
    for (const fs::path& p : headers)
        scanned.insert(rel(p));

    // R1: no Vulkan includes in public headers (comments stripped so
    // a commented-out include does not count; string contents KEPT -
    // the quoted include path is the directive's payload).
    {
        std::vector<std::string> offenders;
        for (const fs::path& p : headers) {
            const std::string code = StripCommentsKeepStrings(ReadText(p));
            if (Contains(code, "#include <vulkan") || Contains(code, "#include <volk.h") ||
                Contains(code, "#include \"volk.h") || Contains(code, "vk_mem_alloc"))
                offenders.push_back(p.generic_string());
        }
        Check(offenders.empty(), "arch.r1-no-vulkan-includes (R1: Vulkan Include Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R1 offender: " << o << "\n";
    }
    // R2: no private-backend impl includes in public headers. Scans
    // the COMMENT-STRIPPED, STRING-KEEPING text so both quoted and
    // angle include paths survive (the audit's evasion fix) while
    // prose references to the impl file still do not count - only
    // actual #include directives count. (Core/Limits.h's
    // engine_limits.hlsli include is the shared CPU/HLSL layout
    // CONTRACT, deliberately not the private backend - unflagged.)
    {
        std::vector<std::string> offenders;
        for (const fs::path& p : headers) {
            const std::string code = StripCommentsKeepStrings(ReadText(p));
            size_t at = 0;
            while ((at = code.find("#include", at)) != std::string::npos) {
                const size_t lineEnd = code.find('\n', at);
                const std::string line = code.substr(
                    at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at);
                if (Contains(line, "Vulkan/detail") || Contains(line, "vulkan_impl") ||
                    Contains(line, "../../src") || Contains(line, "../../../src"))
                    offenders.push_back(p.generic_string() + ": " + line.substr(0, 72));
                at = lineEnd == std::string::npos ? code.size() : lineEnd;
            }
        }
        Check(offenders.empty(), "arch.r2-no-impl-includes (R2: Private Implementation Guard)");
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
            const std::string code = StripCommentsAndStrings(ReadText(p));
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
                    at == 0 || !(std::isalnum(static_cast<unsigned char>(code[at - 1])) ||
                                 code[at - 1] == '_');
                const bool vulkanType =
                    code.size() > at + 2 && std::isupper(static_cast<unsigned char>(code[at + 2]));
                if (wordStart && vulkanType) {
                    // Grab the token + baseline check.
                    size_t end = at + 2;
                    while (
                        end < code.size() &&
                        (std::isalnum(static_cast<unsigned char>(code[end])) || code[end] == '_'))
                        ++end;
                    const std::string tok = code.substr(at, end - at);
                    bool known = false;
                    for (const std::string& b : kBaseline)
                        if (tok == b) {
                            known = true;
                            break;
                        }
                    if (!known) {
                        offenders.push_back(p.generic_string() + ": " + tok);
                        break; // one report per file is enough
                    }
                }
                at += 2;
            }
        }
        Check(offenders.empty(), "arch.r3-no-vk-types-in-code (R3: Vulkan Type Leakage Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R3 offender: " << o << "\n";
    }
    // R4: THE UPWARD DEPENDENCY RULE (the audit's section 6, as a
    // scanner guard): the public RHI headers (smvk.h + backend.h +
    // RHI/*.h) must not include ENGINE-DOMAIN headers (scene/app/gi/
    // placement/audio/camera/profile/assets/worldfields/utils) - the
    // RHI stays unaware of the engine above it. String contents are
    // preserved so quoted #include "Engine/..." paths cannot evade
    // (angle + quoted are equivalent leaks). BASELINE (the tracked
    // pre-existing findings, NOT new leaks):
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
        const fs::path rhiRoot = fs::path(TOURMALINE_SOURCE_ROOT) / "engine" / "include" / "Engine";
        std::vector<fs::path> rhiHeaders;
        for (const char* name : {"smvk.h",
                                 "backend.h",
                                 "RHI/resource_manager.h",
                                 "RHI/swapchain.h",
                                 "RHI/DescriptorLimits.h",
                                 "RHI/FrameConfig.h"}) {
            const fs::path p = rhiRoot / name;
            if (fs::exists(p))
                rhiHeaders.push_back(p);
        }
        // Engine-domain include prefixes the RHI must never pull.
        static const char* kForbidden[] = {
            "Engine/Scene/",
            "Engine/App/",
            "Engine/GI/",
            "Engine/Placement/",
            "Engine/Audio/",
            "Engine/Camera/",
            "Engine/Profile/",
            "Engine/WorldFields/",
            "Engine/Utils/",
            "Engine/ImGui/",
            "Engine/Asset/tour.h",
            "Engine/Asset/pak.h",
            "Engine/Asset/touranim.h",
            "Engine/Asset/appmanifest.h",
        };
        for (const fs::path& p : rhiHeaders) {
            const std::string code = StripCommentsKeepStrings(ReadText(p));
            size_t at = 0;
            while ((at = code.find("#include", at)) != std::string::npos) {
                const size_t lineEnd = code.find('\n', at);
                const std::string line = code.substr(
                    at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at);
                for (const char* bad : kForbidden) {
                    if (Contains(line, bad))
                        offenders.push_back(p.generic_string() + ": " + line.substr(0, 72));
                }
                at = lineEnd == std::string::npos ? code.size() : lineEnd;
            }
        }
        Check(offenders.empty(),
              "arch.r4-rhi-no-engine-includes (R4: Direct RHI Dependency Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R4 offender: " << o << "\n";
    }
    // R5: THE TRANSITIVE upward dependency (section 8d's early-warning
    // form): walk the FULL include graph from the public RHI headers
    // and fail on any engine-domain header reachable TRANSITIVELY (a
    // direct include ban is R4; a chain like RHI -> allowed-middle ->
    // Engine/App/... is this rule). Same baseline/allowed vocabulary
    // as R4. Renderer/ headers are walked too (the audit's weakness:
    // the graph is not examined past them). Quoted Engine includes
    // are parsed as well. Depth-capped (a diamond graph would
    // revisit; the cap is generous for header chains).
    {
        std::vector<std::string> offenders;
        const fs::path incRootWalk =
            fs::path(TOURMALINE_SOURCE_ROOT) / "engine" / "include" / "Engine";
        auto engineIncludes = [](const std::string& code) {
            std::vector<std::string> out;
            std::smatch m;
            static const std::regex reAngle("#include\\s*<Engine/([^>]+)>");
            auto begin = code.cbegin();
            while (std::regex_search(begin, code.cend(), m, reAngle)) {
                out.push_back(m[1].str());
                begin = m.suffix().first;
            }
            // Quoted form: the include payload is a string literal, so
            // the caller must pass STRING-KEEPING stripped text.
            static const std::regex reQuoted("#include\\s*\"Engine/([^\"]+)\"");
            begin = code.cbegin();
            while (std::regex_search(begin, code.cend(), m, reQuoted)) {
                out.push_back(m[1].str());
                begin = m.suffix().first;
            }
            return out;
        };
        // forbidden prefixes (same as R4) + the baseline edges
        // (from-file, to-include) that are tracked findings.
        static const char* kForbiddenPfx[] = {
            "Scene/",
            "App/",
            "GI/",
            "Placement/",
            "Audio/",
            "Camera/",
            "Profile/",
            "WorldFields/",
            "Utils/",
            "ImGui/",
            "Asset/tour.h",
            "Asset/pak.h",
            "Asset/touranim.h",
            "Asset/appmanifest.h",
        };
        struct BaselineEdge {
            const char* from;
            const char* to;
        };
        static const BaselineEdge kBaseline[] = {
            // resource_manager.h -> spirv_reflection.h (section 6,
            // the P1 adapter fix) - spirv_reflection itself only
            // pulls std headers, so the chain stops there.
            {"RHI/resource_manager.h", "Renderer/spirv_reflection.h"},
        };
        auto isForbidden = [](const std::string& inc) {
            for (const char* p : kForbiddenPfx)
                if (inc.rfind(p, 0) == 0)
                    return true;
            return false;
        };
        auto isBaseline = [](const std::string& from, const std::string& inc) {
            for (const BaselineEdge& b : kBaseline) {
                const std::string f = b.from;
                if (from == f && inc.rfind(b.to, 0) == 0)
                    return true;
            }
            return false;
        };
        // BFS from the RHI roots, over the STRIPPED code (comments
        // gone so commented-out includes never count, strings KEPT so
        // quoted Engine paths count). VISITED-set keyed on the walked
        // FILE (the include graph is cyclic - smvk.h <-> resource_
        // manager.h - and without the set the re-walk burns the hop
        // budget before the deep edges).
        std::vector<std::string> roots = {"smvk.h",
                                          "backend.h",
                                          "RHI/resource_manager.h",
                                          "RHI/swapchain.h",
                                          "RHI/DescriptorLimits.h",
                                          "RHI/FrameConfig.h"};
        std::vector<std::pair<std::string, std::string>> queue;
        std::vector<std::string> visited;
        auto wasVisited = [&visited](const std::string& inc) {
            return std::find(visited.begin(), visited.end(), inc) != visited.end();
        };
        for (const std::string& r : roots) {
            const fs::path p = incRootWalk / r;
            if (fs::exists(p)) {
                visited.push_back(r);
                for (const std::string& inc : engineIncludes(StripCommentsKeepStrings(ReadText(p))))
                    queue.push_back({r, inc});
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
            const fs::path next = incRootWalk / inc;
            std::error_code ec;
            if (!fs::exists(next, ec))
                continue;
            if (wasVisited(inc))
                continue;
            visited.push_back(inc);
            for (const std::string& deeper :
                 engineIncludes(StripCommentsKeepStrings(ReadText(next))))
                queue.push_back({inc, deeper});
        }
        Check(offenders.empty(),
              "arch.r5-rhi-transitively-clean (R5: Transitive RHI Dependency Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R5 offender: " << o << "\n";
    }
    // R6: no broad `using namespace ::smvk` (or any
    // using-namespace-smvk form) in a public header. The landed
    // migration stages still carry these directives (the moved blocks
    // spell smvk types unqualified); the baseline freezes exactly
    // those. A new directive, or a second one in a listed file, is a
    // boundary regression; retiring one REQUIRES deleting its row.
    {
        struct FileCount {
            const char* file;
            int count;
        };
        // EMPTY (item 1 of the remaining-changes list: zero broad
        // namespace imports in public headers - the last ten removed,
        // their RHI names qualified or their impl TUs carry the
        // TU-local escape hatch). Any new `using namespace ...smvk`
        // in a public header fails outright. (The {nullptr, 0}
        // sentinel is the empty-list representation: zero-size
        // arrays are not C++.)
        static const FileCount kBaseline[] = {
            {nullptr, 0},
        };
        static const std::regex re("using\\s+namespace\\s*(::)?\\s*smvk"
                                   "(\\s*::\\s*[A-Za-z_][A-Za-z0-9_]*)*\\s*;");
        std::vector<std::string> offenders, stale;
        std::set<std::string> listed;
        for (const fs::path& p : headers) {
            const std::string r = rel(p);
            const std::string code = StripCommentsAndStrings(ReadText(p));
            const int actual = (int)CountMatches(code, re);
            int expected = 0;
            bool inBaseline = false;
            for (const FileCount& b : kBaseline)
                if (b.file && r == b.file) {
                    expected = b.count;
                    inBaseline = true;
                    break;
                }
            if (actual > expected)
                offenders.push_back(r + " carries " + std::to_string(actual) +
                                    " broad smvk import(s), baseline allows " +
                                    std::to_string(expected));
            else if (inBaseline && actual < expected)
                stale.push_back(r + ": import retired (actual " + std::to_string(actual) +
                                " < baseline " + std::to_string(expected) +
                                ") - DELETE the baseline row");
        }
        for (const FileCount& b : kBaseline)
            if (b.file && !scanned.count(b.file))
                stale.push_back(std::string(b.file) + ": baseline row points at a missing header"
                                                      " - DELETE it");
        Check(offenders.empty() && stale.empty(),
              "arch.r6-no-broad-smvk-imports (R6: Broad Namespace Import Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R6 offender: " << o << "\n";
        for (const std::string& o : stale)
            std::cout << "  R6 stale baseline: " << o << "\n";
    }
    // R7: COMPAT ALIAS CONTAINMENT. The migration's re-export blocks
    // (namespace smvk { using ::tour::World; ... } and the namespace
    // ALIAS form namespace tour = ::tour::asset) keep the pre-migration
    // smvk::* spellings compiling - a sanctioned TRANSITION mechanism
    // that must not outlive its consumers or spread to new files.
    // Scanned only inside brace-matched namespace-smvk regions (the
    // marker outside smvk would be an intra-engine using, not a
    // compat re-export).
    {
        struct AliasBaseline {
            const char* file;
            int usings;
            int aliases;
        };
        static const AliasBaseline kBaseline[] = {
            {"App/app.h", 1, 0},
            {"App/static_merge.h", 1, 0},
            {"App/animator.h", 6, 0},
            {"App/cluster.h", 9, 0},
            {"App/world.h", 18, 0},
            {"Asset/asset_plan.h", 3, 0},
            // 3 = Cluster + GiBinding + TextureStreamer (the streamer moved
            // to tour, audit finding 3; the smvk:: serializer surface
            // forward-uses it through the compat using-declaration).
            {"Asset/cluster_serializer.h", 3, 0},
            {"Asset/mesh_optimizer.h", 1, 0},
            {"Asset/pak.h", 29, 0},
            {"Asset/tour.h", 1, 1},     // namespace tour = ::tour::asset
            {"Asset/touranim.h", 0, 1}, // namespace touranim = ::tour::asset
            {"Asset/meshlet.h", 1, 0},  // StaticMeshUpload fwd-becomes-using (mesh.h move)
            {"Audio/audio.h", 6, 0},
            {"GI/bake.h", 17, 0},
            {"GI/bake_inputs.h", 18, 0},
            {"GI/bc6h.h", 2, 0},
            {"GI/cubemap_volume.h", 17, 0},
            {"GI/lightmap.h", 37, 0},
            {"GI/regular_volume.h", 6, 0},
            {"GI/sg.h", 21, 0},
            {"GI/triangle_bvh.h", 2, 0},
            // (The engine Memory/* headers became pure forwarding
            // shims over Core/Memory in the frame-arena extraction
            // (#251): zero compat aliases remain, so their R7 rows are
            // deleted rather than shrunk.)
            {"Placement/placement_cache.h", 7, 0},
            {"Placement/placement_domain.h",
             11,
             0}, // +1 (#126): the
                 // Painted PointCloud domain's PlacementPointCloudData —
                 // deliberate smvk::placement compat growth, review at gate.
            {"Placement/placement_draw.h", 7, 0},
            {"Placement/placement_eval.h", 6, 0},
            {"Placement/placement_gpu.h", 37, 0}, // +2 (#126): paint-mask
                                                  // + PointCloud GPU surface inputs — deliberate
                                                  // compat growth, review at gate.
            {"Placement/placement_ids.h", 12, 0},
            {"Placement/placement_ir.h", 9, 0},
            {"Placement/placement_override.h", 7, 0},
            {"Placement/placement_physics.h", 7, 0},
            {"Placement/placement_point.h", 3, 0},
            {"Placement/placement_prototype.h", 3, 0},
            {"Placement/placement_terrain.h", 21, 0},
            {"Placement/placement_tile.h", 14, 0},
            {"Profile/profile.h", 15, 0},
            {"Rendergraph/placement_pass.h",
             17,
             0}, // -2 (the #251-era core
                 // memory extraction retired two; see the R7 shrink rule)
            {"Utils/shader_library.h", 0, 1}, // namespace shaders = ::tour::shaders
            {"Renderer/geometry_streamer.h", 10, 0},
            {"Renderer/texture_streamer.h", 4, 0},
            {"Renderer/mesh.h", 35, 0}, // the data model: Material/StaticMesh/packed-vertex
                                        // families (VertexLayoutId + the layout table stay smvk::)
            {"Renderer/gpu_scene_static.h",
             1,
             1}, // StaticMeshUpload fwd-becomes-using + namespace gfx = ::tour::gfx
        };
        // using ::tour::X / using namespace ::tour (the per-name and
        // the namespace-directive import), and the namespace ALIAS
        // (namespace X = ::tour...).
        static const std::regex reUsing("using\\s*(::tour::|namespace\\s+::tour)");
        static const std::regex reNsAlias("namespace\\s+[A-Za-z_][A-Za-z0-9_]*\\s*=\\s*::tour");
        std::vector<std::string> offenders, stale;
        for (const fs::path& p : headers) {
            const std::string r = rel(p);
            const std::string code = StripCommentsAndStrings(ReadText(p));
            int usings = 0, aliases = 0;
            for (const auto& [start, end] : SmvkRegions(code)) {
                if (start >= code.size() || end > code.size() || start >= end)
                    continue;
                const std::string region = code.substr(start, end - start);
                usings += (int)CountMatches(region, reUsing);
                aliases += (int)CountMatches(region, reNsAlias);
            }
            int expU = 0, expA = 0;
            bool inBaseline = false;
            for (const AliasBaseline& b : kBaseline)
                if (r == b.file) {
                    expU = b.usings;
                    expA = b.aliases;
                    inBaseline = true;
                    break;
                }
            if (usings > expU || aliases > expA)
                offenders.push_back(r + " re-exports " + std::to_string(usings) +
                                    " tour symbol(s) (" + std::to_string(aliases) +
                                    " ns alias(es))" + " through smvk, baseline allows " +
                                    std::to_string(expU) + " / " + std::to_string(expA));
            else if (inBaseline && (usings < expU || aliases < expA))
                stale.push_back(r + ": compat aliases retired (actual " + std::to_string(usings) +
                                " / " + std::to_string(aliases) + " < baseline " +
                                std::to_string(expU) + " / " + std::to_string(expA) +
                                ") - SHRINK the baseline row");
        }
        for (const AliasBaseline& b : kBaseline)
            if (!scanned.count(b.file))
                stale.push_back(std::string(b.file) + ": baseline row points at a missing header"
                                                      " - DELETE it");
        Check(offenders.empty() && stale.empty(),
              "arch.r7-compat-alias-containment (R7: Legacy Namespace Compatibility Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R7 offender: " << o << "\n";
        for (const std::string& o : stale)
            std::cout << "  R7 stale baseline: " << o << "\n";
    }
    // R8: RHI IMPLEMENTATION-STATE EXPOSURE in the engine-facing
    // public headers (the App ownership getters + LinearPtr members,
    // ElementFrame's Framebuffer*, GfxRenderer's gfx::State, and the
    // raw RHI pointer types the renderer passes around). The RHI's own
    // headers are excluded. Every (file, token) count below is the
    // frozen current state: growth or a new site is a review event,
    // retirement shrinks the row. RHI HANDLES (TextureHandle etc.)
    // and RHI usage in general are NOT violations and deliberately
    // not in the token set - the engine is allowed to call the RHI;
    // this freezes the audit's implementation-state exposure shapes.
    {
        struct Token {
            const char* name;
            const char* re;
        };
        static const Token kTokens[] = {
            {"GetResourceManager(", "GetResourceManager\\s*\\("},
            {"GetSwapchain(", "GetSwapchain\\s*\\("},
            {"GetRendergraph(", "GetRendergraph\\s*\\("},
            {"LinearPtr<ResourceManager>", "LinearPtr<ResourceManager>"},
            {"LinearPtr<Swapchain>", "LinearPtr<Swapchain>"},
            {"LinearPtr<RenderGraph>", "LinearPtr<RenderGraph>"},
            {"gfx::State", "gfx::State"},
            {"ResourceManager*", "\\bResourceManager[ \\t]*\\*"},
            {"Swapchain*", "\\bSwapchain[ \\t]*\\*"},
            {"RenderGraph*", "\\bRenderGraph[ \\t]*\\*"},
            {"Framebuffer*", "\\bFramebuffer[ \\t]*\\*"},
        };
        struct TokenBaseline {
            const char* file;
            int token;
            int count;
        };
        static const TokenBaseline kBaseline[] = {
            {"App/app.h", 0, 1}, // GetResourceManager
            {"App/app.h", 1, 1}, // GetSwapchain
            {"App/app.h", 2, 1}, // GetRendergraph
            {"App/app.h", 3, 1}, // LinearPtr<ResourceManager>
            {"App/app.h", 4, 1}, // LinearPtr<Swapchain>
            {"App/app.h", 5, 1}, // LinearPtr<RenderGraph>
            {"App/app.h", 7, 1}, // ResourceManager*
            {"App/app.h", 8, 1}, // Swapchain*
            {"App/app.h", 9, 1}, // RenderGraph*
            {"App/cluster.h", 7, 2},
            {"App/world.h", 7, 1},
            {"App/Element.h", 10, 1}, // ElementFrame::fb
            {"ImGui/imgui_overlay.h", 7, 1},
            {"ImGui/imgui_overlay.h", 8, 1},
            {"Memory/pool.h", 7, 8}, // RHI pools (shared infra, smvk-scope)
            {"Renderer/geometry_upload.h", 7, 1},
            {"Renderer/WorldRenderer.h", 7, 1},
            {"Renderer/WorldRenderer.h", 8, 6},
            // DebugUiFrame (the host-owned UI service seam, #248): the
            // frame context hands the host service the rendergraph + the
            // swap framebuffer to append its ImGui pass - deliberate
            // exposure, reviewed at the gate.
            {"Renderer/WorldRenderer.h", 9, 1},
            {"Renderer/WorldRenderer.h", 10, 1},
            {"Renderer/GfxRenderer.h", 6, 2}, // gfx member + State()
            {"Rendergraph/mipchain.h", 9, 1},
            {"Rendergraph/renderer.h", 7, 1},
            {"Rendergraph/renderer.h", 9, 1},
            {"Rendergraph/rendergraph.h", 9, 1},
            {"WorldFields/WorldField.h", 7, 2},
            {"WorldFields/WorldFieldProvider.h", 7, 2},
        };
        constexpr int kTokenCount = (int)(sizeof(kTokens) / sizeof(kTokens[0]));
        // Precompile once.
        std::vector<std::regex> tokenRes;
        for (const Token& t : kTokens)
            tokenRes.push_back(std::regex(t.re));
        // (kBaseline is a static local: no capture needed - statics
        // are directly visible to the lambda.)
        auto baseFor = [](const std::string& file, int token, bool& listed) {
            int count = 0;
            listed = false;
            for (const TokenBaseline& b : kBaseline)
                if (token == b.token && file == b.file) {
                    count = b.count;
                    listed = true;
                    break;
                }
            return count;
        };
        std::vector<std::string> offenders, stale;
        for (const fs::path& p : headers) {
            const std::string r = rel(p);
            // The RHI's own headers are excluded: smvk.h, backend.h
            // and RHI/* exposing RHI types is not a boundary leak.
            if (r == "smvk.h" || r == "backend.h" || r.rfind("RHI/", 0) == 0)
                continue;
            const std::string code = StripCommentsAndStrings(ReadText(p));
            for (int t = 0; t < kTokenCount; ++t) {
                const int actual = (int)CountMatches(code, tokenRes[t]);
                bool listed = false;
                const int expected = baseFor(r, t, listed);
                if (actual > expected)
                    offenders.push_back(r + " token '" + kTokens[t].name + "' actual " +
                                        std::to_string(actual) + " > baseline " +
                                        std::to_string(expected));
                else if (listed && actual < expected)
                    stale.push_back(r + " token '" + kTokens[t].name + "' actual " +
                                    std::to_string(actual) + " < baseline " +
                                    std::to_string(expected) + " - SHRINK the baseline row");
            }
        }
        for (const TokenBaseline& b : kBaseline)
            if (!scanned.count(b.file))
                stale.push_back(std::string(b.file) + " token '" + kTokens[b.token].name +
                                "': baseline row points at a missing header"
                                " - DELETE it");
        Check(offenders.empty() && stale.empty(),
              "arch.r8-rhi-impl-state-exposure (R8: RHI Implementation Exposure Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R8 offender: " << o << "\n";
        for (const std::string& o : stale)
            std::cout << "  R8 stale baseline: " << o << "\n";
    }

    // R9: THE GLOBAL-NAMESPACE GATE (the "no NEW globals" guard the
    // scene_node_id.h bridge comment promises). The global-namespace
    // cleanup (todo 8.4) moved the engine's globally-visible types
    // (NodeId/scene tables/Camera/Input/BVH/DXC) into tour::(scene|
    // dxc...) and bridges them with GLOBAL compat re-exports — the
    // same transition mechanism as the smvk:: blocks in R7. This gate
    // freezes BOTH halves exactly:
    //   1. The global RE-EXPORT rows (using tour::...; / using X =
    //      tour::Y;) are confined to the audited per-file counts —
    //      a new global re-export anywhere, or a grown one, fails;
    //      retiring one fails until the baseline row shrinks.
    //   2. NO NEW global TYPE declarations: the engine's public
    //      headers declare no engine types at global scope anymore
    //      (third-party C forward declarations — SDL_Window,
    //      ktxTexture2 — are excluded by name). A new global
    //      struct/class/enum in a public header fails outright.
    {
        struct ReexportBaseline {
            const char* file;
            int count;
            int decls;
        };
        static const ReexportBaseline kBaseline[] = {
            {"App/input.h", 120, 0},
            {"Scene/scene.h", 66, 0},
            {"Scene/scene_tables.h", 60, 0},
            {"Scene/scene_payloads.h", 13, 0},
            {"Scene/scene_node_id.h", 11, 0},
            // The language-neutral C ABI surface (PR #74): C has no
            // namespaces, so the five opaque catalog types are global
            // by necessity. Reviewed exception - never grow.
            {"Asset/catalog_c.h", 0, 5},
        };
        static const std::regex reReexport(
            "using\\s+(namespace\\s+)?(\\w+\\s*=\\s*)?(::)?tour\\s*::");
        static const std::regex reDecl("(struct|class|enum)\\s+([A-Za-z_][A-Za-z0-9_]*)");
        static const char* kThirdParty[] = {"SDL_Window", "ktxTexture2"};
        std::vector<std::string> offenders, stale;
        for (const fs::path& p : headers) {
            const std::string r = rel(p);
            const std::string code = StripCommentsAndStrings(ReadText(p));
            // Global segments = the complement of the namespace blocks.
            const auto ns = AllNamespaceRegions(code);
            size_t at = 0;
            int reexports = 0;
            std::vector<std::string> decls;
            for (const auto& [start, end] : ns) {
                if (start > at) {
                    const std::string globalSeg = code.substr(at, start - at);
                    reexports += (int)CountMatches(globalSeg, reReexport);
                    auto begin = globalSeg.cbegin();
                    std::smatch m;
                    while (std::regex_search(begin, globalSeg.cend(), m, reDecl)) {
                        bool thirdParty = false;
                        for (const char* t : kThirdParty)
                            if (m[2].str() == t) {
                                thirdParty = true;
                                break;
                            }
                        if (!thirdParty)
                            decls.push_back(m[2].str());
                        begin = m.suffix().first;
                    }
                }
                at = std::max(at, end);
            }
            if (at < code.size()) {
                const std::string globalSeg = code.substr(at);
                reexports += (int)CountMatches(globalSeg, reReexport);
                auto begin = globalSeg.cbegin();
                std::smatch m;
                while (std::regex_search(begin, globalSeg.cend(), m, reDecl)) {
                    bool thirdParty = false;
                    for (const char* t : kThirdParty)
                        if (m[2].str() == t) {
                            thirdParty = true;
                            break;
                        }
                    if (!thirdParty)
                        decls.push_back(m[2].str());
                    begin = m.suffix().first;
                }
            }
            int expected = 0, expectedDecls = 0;
            bool listed = false;
            for (const ReexportBaseline& b : kBaseline)
                if (r == b.file) {
                    expected = b.count;
                    expectedDecls = b.decls;
                    listed = true;
                    break;
                }
            if (reexports > expected)
                offenders.push_back(r + " re-exports " + std::to_string(reexports) +
                                    " tour symbol(s) at GLOBAL scope, baseline allows " +
                                    std::to_string(expected));
            else if (listed && reexports < expected)
                stale.push_back(r +
                                ": global re-exports retired"
                                " (actual " +
                                std::to_string(reexports) + " < baseline " +
                                std::to_string(expected) + ") - SHRINK the baseline row");
            // Global TYPE declarations: zero by default. A pure C ABI
            // header (no namespaces in C) carries a reviewed per-file
            // allowance in the baseline - growth still fails the gate.
            if ((int)decls.size() > expectedDecls)
                for (const std::string& d : decls)
                    offenders.push_back(r + " declares engine type '" + d +
                                        "' at GLOBAL scope (the global-namespace"
                                        " cleanup is complete; new global declarations"
                                        " are a boundary violation)");
            else if (listed && (int)decls.size() < expectedDecls)
                stale.push_back(r +
                                ": global type declarations retired"
                                " (actual " +
                                std::to_string(decls.size()) + " < baseline " +
                                std::to_string(expectedDecls) + ") - SHRINK the baseline row");
        }
        for (const ReexportBaseline& b : kBaseline)
            if (!scanned.count(b.file))
                stale.push_back(std::string(b.file) + ": R9 baseline row points at a missing header"
                                                      " - DELETE it");
        Check(offenders.empty() && stale.empty(),
              "arch.r9-no-new-global-decls (R9: Global Namespace Pollution Guard)");
        for (const std::string& o : offenders)
            std::cout << "  R9 offender: " << o << "\n";
        for (const std::string& o : stale)
            std::cout << "  R9 stale baseline: " << o << "\n";
    }

    // GI-parity regression guard: the legacy direct opaque Main leg must
    // select the active GI permutation through MainLitProgramFor, never
    // hardcode the base mainProgram. Hardcoding silently dropped the baked
    // convex-volume GI (Sponza rendered the sky fallback while the GPU
    // Scene path evaluated GI_VOLUMES) - a silent visual regression with no
    // build or validation-layer failure to catch it.
    {
        const fs::path wr =
            fs::path(TOURMALINE_SOURCE_ROOT) / "engine" / "src" / "Renderer" / "WorldRenderer.cpp";
        const std::string text = ReadText(wr);
        const size_t pos = text.find("ShaderProgram& baseProg =");
        bool selectsGi = false;
        if (pos != std::string::npos) {
            const size_t semi = text.find(';', pos);
            const std::string block = text.substr(pos, semi - pos);
            selectsGi = block.find("MainLitProgramFor") != std::string::npos;
        }
        Check(selectsGi,
              "arch.gi-direct-main-selection (the direct opaque Main leg selects the "
              "active GI permutation via MainLitProgramFor)");
        if (!selectsGi)
            std::cout << "  GI offender: direct Main leg baseProg does not call "
                         "MainLitProgramFor - the active GI variant would be dropped\n";
    }

    std::cout << "architecture_tests: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
