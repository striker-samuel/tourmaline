// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE PROJECT RECIPE (the authored <name>.tourrecipe): the developer's
// build definition — the entry scene, the packages to produce, their
// dependencies, and the bake configuration. ONE recipe builds ONE
// project into one or more .pak archives + a generated appmanifest
// (.tapp — see the resolver/executor for the output side).
//
// This header is the PARSER ONLY: syntax + schema validation into typed
// structures, with filename/line/directive diagnostics. It performs NO
// filesystem discovery and executes NO build stage — resolution
// (globs, tours, ownership) lives in recipe_resolver.h, execution in
// build_pipeline.h.
//
// Grammar v1 (line-oriented, versioned; # comments run to end-of-line
// OUTSIDE quoted values; blank lines skipped; CRLF/LF both accepted; a
// leading UTF-8 BOM is tolerated; values may be quoted with '"' — no
// escape sequences, a quoted value may contain spaces and '#'):
//
//   recipe <version>          REQUIRED, must be the FIRST directive;
//                              only version 1 exists (kRecipeGrammarVersion)
//   project <name>            optional display name
//   entry <scene.tour>        REQUIRED, project-relative; must be some
//                              [pak]'s 'scene' (the entry has to ship)
//   bake gi [on|off]          the GI volume bake (bare = on)
//   bake cubemaps [on|off]    the cubemap capture bake (bare = on)
//
//   [pak <id>]                one section per package; ids unique
//   output <file.pak>         REQUIRED per pak; unique across paks
//   include <glob>            repeatable; project-relative glob ('**'
//                              crosses directories, '*'/'?' stay in one
//                              segment) claiming OWNED asset files
//   scene <scene.tour>        the pak's owning scene (at most one;
//                              unique across paks — a scene ships once)
//   depends <pak-id>          repeatable, ordered; a REAL dependency
//                              edge (mount order + permitted
//                              cross-pak references), never a hint
//   dependencies transitive   'none' (the default) or 'transitive'
//
// Paths are PROJECT-RELATIVE with forward slashes (Windows-authored
// backslashes are normalized): absolute paths, drive letters, '..'/
// '.' segments, and empty segments are rejected. Wildcards are legal
// ONLY in 'include'. No scripting, no environment expansion, no
// expressions — a recipe is data, never a program.

#include <cstdint>
#include <string>
#include <vector>

namespace smvk::project {

// The grammar version this build reads. Unsupported versions are
// rejected with a diagnostic (never guessed at).
inline constexpr uint32_t kRecipeGrammarVersion = 1;

// One [pak <id>] section, verbatim from the authored recipe (authored
// order preserved — resolution topo-sorts for the plan).
struct RecipePak {
	std::string id;
	std::string output;                // authored output file name
	std::string scene;                 // project-relative tour ("" = none)
	std::vector<std::string> includes; // project-relative globs, authored order
	std::vector<std::string> depends;  // pak ids, authored order
	bool dependenciesTransitive = false;
	size_t line = 0;                   // the [pak] header's 1-based line

	[[nodiscard]] bool HasContentSource() const {
		return !scene.empty() || !includes.empty();
	}
};

struct ProjectRecipe {
	uint32_t version = 0; // 0 = unparsed (a valid parse always records 1)
	std::string projectName;
	std::string entry;    // project-relative entry scene tour
	bool bakeGi = false;
	bool bakeCubemaps = false;
	std::vector<RecipePak> paks; // authored order

	[[nodiscard]] const RecipePak* FindPak(const std::string& id) const;
};

// A parse failure: WHERE (1-based line; 0 = not line-specific, e.g. a
// missing directive), WHAT (the offending directive/section token), and
// WHY. Format() renders the caller-facing single line — the parser
// works on text, so the FILENAME is the caller's contribution.
struct RecipeDiagnostic {
	size_t line = 0;
	std::string directive;
	std::string message;

	[[nodiscard]] std::string Format(const std::string& file) const;
};

// Parse a recipe file. False + err on the FIRST offending line; `out`
// is untouched on failure (parse-into-temp, publish on success is the
// caller's pattern — a failed parse never half-populates a recipe).
bool ParseProjectRecipe(const std::string& path, ProjectRecipe& out,
	RecipeDiagnostic& err);

// The same grammar over in-memory text (tests; the editor's unsaved
// buffers later). Identical diagnostics — the line numbers are the
// text's own.
bool ParseProjectRecipeText(const std::string& text, ProjectRecipe& out,
	RecipeDiagnostic& err);

} // namespace smvk::project
