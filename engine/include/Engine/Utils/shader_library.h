// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE ENGINE SHADER LIBRARY (the unified VFS-backed shader reader):
// ONE process-wide overlay serving compiled shader bytecode to every
// consumer — renderer, render-graph passes, editor, debug views —
// through the same "content/shaders/<file>.spv" path strings they
// always used (a drop-in for the old CWD readFile reads).
//
// Providers, in precedence order (lower OverlayVfs priority = first):
//   1. loose DEV overrides (MountLooseDir / the exe-relative
//      content/shaders/ tree the build syncs — a recompiled shader
//      wins immediately in dev; inert in a deployed tree that ships
//      no loose shaders);
//   2. mounted SHADER PACKAGES (MountPak / the exe-relative
//      engine-shaders.pak the build deploys — the production source,
//      a MANDATORY runtime dependency of every graphics host).
//
// Every read VALIDATES the bytecode before it reaches a module: the
// SPIR-V magic (0x07230203) on the Vulkan backend, the DXBC container
// for DXIL (DetectBytecode) — a corrupt package or a truncated blob
// is a loud, named failure at READ time, never a driver crash at
// pipeline-build time.
//
// Deployment independence: discovery is EXECUTABLE-RELATIVE (never
// CWD). Hosts call Init() before graphics initialization (the mount
// order contract); every entry is also lazily self-initializing so a
// late consumer can never read unmounted.

#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

namespace shaders {

// The compiled-bytecode format (validated on every read).
enum class Bytecode : uint8_t {
	Spirv,    // SPIR-V (the Vulkan backend's module format)
	Dxil,     // DXBC container (the DXIL twin artifacts)
	Unknown,  // neither magic — a corrupt/truncated blob
};

// Bytecode detection from the payload's magic (SPIR-V: 07 23 02 03;
// DXIL: the "DXBC" RIFF-style container magic). Cheap, allocation-free.
[[nodiscard]] Bytecode DetectBytecode(const uint8_t* data, size_t size);

// ---- lifecycle -----------------------------------------------------------
// Mounts the exe-relative providers: content/shaders/ (dev override,
// priority -10) + engine-shaders.pak (production, priority 0) when
// the files exist. Idempotent + thread-safe; a host that never calls
// it still works (every read lazily initializes).
void Init();
// Drops every mount (tests; a full graphics shutdown). Idempotent.
void Shutdown();
[[nodiscard]] bool Initialized();
// True when at least ONE shader source is mounted (the deployment
// gate's proof: the engine-shaders.pak package and/or the loose dev
// tree exist exe-relative). False = nothing can ever serve a read.
[[nodiscard]] bool HasAnySource();

// ---- explicit providers (hosts + tests) ----------------------------------
// Mount ONE shader package (a pak carrying PakAssetKind::Shader
// entries — the engine-owned engine-shaders.pak or a project shader
// pak). The file is read + verified (PakReader::Open CRCs the image)
// + served at production precedence. Returns false + err on a
// missing/corrupt file.
[[nodiscard]] bool MountPak(const std::string& pakFilePath,
	std::string& err);
// Mount ONE loose dev tree (rootDir/content/shaders/... — an
// explicitly configured development override; wins over packages).
[[nodiscard]] bool MountLooseDir(const std::string& rootDir,
	const std::string& name, std::string& err);
// Unmount one named provider (tests swap sources; hosts never need
// this in production).
[[nodiscard]] bool Unmount(const std::string& name);

// ---- the read ------------------------------------------------------------
// THE drop-in for readFile("content/shaders/x.spv"): serves through
// the overlay (dev override first, packages next) and VALIDATES the
// bytecode. Throws std::runtime_error (naming the path, the serving
// provider on success paths and every mounted provider on failures)
// — the same contract the old readFile reads had.
[[nodiscard]] std::vector<char> Read(const std::string& path);
// The non-throwing form: false + err on a missing/unvalidated blob;
// `provider` names what served it (diagnostics).
[[nodiscard]] bool TryRead(const std::string& path,
	std::vector<uint8_t>& out, std::string& provider, std::string& err);

} // namespace shaders

} // namespace smvk
