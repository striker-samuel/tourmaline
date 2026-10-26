// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// The project-based content pipeline (the canonical-pipeline spec):
// a PROJECT is the entry point for import (and later cook/pack).
// This extends the EXISTING project system (the project.tourproj
// marker + the editor's project-root model — editor/src/editor_project.cpp)
// into a tool-usable library: the same marker, the same conventions,
// callable from the editor, the CLI and tests without the editor TU.
//
// Directory convention (the EXISTING project rules, not a rewrite — the
// spec's Sources/Assets/DerivedData split is honored in spirit: the
// project owns everything, derived data lives under .tourmaline/):
//   <root>/project.tourproj        the marker (name + version)
//   <root>/content/models/<stem>/  imported sources + their derived
//                                  native families BESIDE them (the
//                                  importer's derive-beside contract:
//                                  .material texture refs are relative
//                                  POSIX paths beside the glTF — moving
//                                  derived files away from the source
//                                  copy breaks resolution; the editor's
//                                  import has always done it this way)
//   <root>/content/scenes/         authored .tour scenes (NEVER written
//                                  by the pipeline)
//   <root>/.tourmaline/            derived data home (the artifact
//                                  cache's directory + the import
//                                  registry + staging)
//
// Relocation: the registry stores PROJECT-RELATIVE source paths (the
// original external location rides as reimport metadata only) — moving
// the project root never breaks resolution.

#include <filesystem>
#include <string>

namespace smvk::project {

inline constexpr const char* kProjectMarker = "project.tourproj";

struct Project {
	// Absolute, lexically-normalized project root ("" = invalid).
	std::string root;
	std::string name; // the marker's name row (the dir name when absent)

	[[nodiscard]] bool Valid() const { return !root.empty(); }

	// Opens an existing project (requires the marker). Invalid + err when
	// the directory has no project.tourproj.
	static Project Open(const std::string& dir, std::string& err);

	// Creates (or adopts) a project: writes the marker + the conventional
	// tree. Refuses to overwrite an existing marker's NAME (adopting an
	// existing project is the normal path — created=false).
	static Project Init(const std::string& dir, const std::string& name,
		bool& created, std::string& err);

	// ---- project-rooted paths (all generic-string, forward slashes) ----
	// The content root: tours record sources content-relative against it
	// (ApplySceneReferences' contentRoot — see gltf.h).
	[[nodiscard]] std::string ContentRoot() const;
	// Where imported glTF sources + their derived families live.
	[[nodiscard]] std::string ImportsDir() const;
	// The derived-data home (artifact cache + registry + staging).
	[[nodiscard]] std::string DerivedDataDir() const;
	// The import registry (see registry.cpp for the row format).
	[[nodiscard]] std::string RegistryPath() const;
	// Project-relative form of an absolute path inside the project
	// ("" when outside — the belt-and-braces escape guard).
	[[nodiscard]] std::string RelativeTo(const std::string& absPath) const;
};

} // namespace smvk::project
