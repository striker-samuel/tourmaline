// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE WATCHED SHADER HOT-RELOAD SERVICE (the checklist's item 15,
// the source->bytes leg): one FileWatcher over a SHADER SOURCE tree
// (engine/shaders/ — *.hlsl sources + *.hlsli includes), settled
// edits recompiled IN-PROCESS through the DXC integration (the same
// flag shape cmake/CompileShaders.cmake uses), the replacement
// bytecode VALIDATED (SPIR-V magic) before it ever lands, and the
// compiled .spv written ATOMICALLY into the output tree the engine
// shader library serves as the DEV OVERRIDE — so the next
// smvk::shaders::Read returns the fresh bytes.
//
// THE LAST-KNOWN-GOOD CONTRACT: a failed compile (or a bytecode that
// fails validation) NEVER touches the existing .spv — the previous
// generation keeps serving and the diagnostic rides the Change log.
// A byte-identical recompile skips the write (no mtime churn, no
// downstream re-swaps).
//
// THE INVALIDATION RULES (CompileShaders.cmake's model):
//   - a .hlsl edit recompiles THAT file's base output + its VARIANT
//     TWINS (the baked-define permutations below);
//   - a .hlsli edit recompiles EVERYTHING (includes are coarse —
//     no per-file include graph at runtime).
//
// THE VARIANT TABLE mirrors engine/CMakeLists.txt's VARIANTS list.
// Drift degrades gracefully: a twin missing here keeps serving its
// build-compiled bytes until the next full build (never a wrong
// variant — a MISSING recompile, not a mismatched one).
//
// THE CONSUMER CONTRACT (the GPU swap, the item's second leg): the
// service only lands BYTES; the frames' shader MODULES/PIPELINES are
// immutable Vulkan objects. A consumer that wants the live swap
// calls ConsumeChanges() once per frame (cheap) and, when a
// Recompiled change touches a module it holds, rebuilds that
// module+pipeline set at the frame boundary (between submits — a
// GPU-safe synchronization point) and retires the old handles only
// after the in-flight frames finish (ResourceManager's
// DestroyShaderModule + the wait-for-retire discipline). On Failed
// changes the consumer does NOTHING (the last-known-good pipelines
// keep rendering).

#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

namespace shader_reload {

// One recompile outcome (the ConsumeChanges log).
enum class Status : uint8_t {
	Recompiled, // new validated bytes landed (or were already current)
	Failed,     // the compile/validation failed; LAST-KNOWN-GOOD kept
};

struct Change {
	std::string spvPath;  // the VFS path ("content/shaders/x.spv")
	std::string source;   // the .hlsl that drove the recompile
	Status status = Status::Recompiled;
	std::string diagnostic; // the compiler error text (Failed)
	uint64_t bytes = 0;     // the landed .spv size (Recompiled)
};

// ---- lifecycle -----------------------------------------------------------
// Watch `sourceDir` (the .hlsl/.hlsli tree) and land recompiled .spv
// under `outDir` (the build's content/shaders — the shader library's
// dev override). Idempotent; the watcher thread + the OS events ride
// Engine/Utils/file_watcher.h. False + err when the dir is missing.
[[nodiscard]] bool Start(const std::string& sourceDir,
	const std::string& outDir, std::string& err);
void Stop();
[[nodiscard]] bool Running();
[[nodiscard]] const std::string& SourceDir();
[[nodiscard]] const std::string& OutDir();

// ---- consumption ---------------------------------------------------------
// Drain SETTLED source changes + recompile them (call once per frame
// — cheap when nothing moved; a recompile is CPU work on the CALLER's
// thread, so a heavy edit costs that frame, by design: dev-only).
// Returns the change log since the last call (empty when quiet).
[[nodiscard]] std::vector<Change> ConsumeChanges(int stabilizeMs = 150);

// Recompile ONE .hlsl NOW (the watcher's exact path; also the tests'
// + hosts' explicit entry). `spvRelName` is the output file name
// (usually "<source stem>.spv"). False + out.diagnostic on failure —
// the existing .spv is never touched.
[[nodiscard]] bool Recompile(const std::string& hlslAbsPath,
	const std::string& spvRelName, Change& out);

// The variant twins of one source file (empty when none) — the
// baked-define permutations, the mirror of engine/CMakeLists.txt's
// VARIANTS entries for that file: (spv name suffix, define list).
struct Variant {
	std::string suffix;            // ".sg5" -> "<stem>.sg5.spv"
	std::vector<std::string> defines;
};
[[nodiscard]] const std::vector<Variant>& VariantsOf(
	const std::string& fileName);

} // namespace shader_reload

} // namespace smvk
