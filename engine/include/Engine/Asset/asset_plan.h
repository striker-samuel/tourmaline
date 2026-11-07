// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// COMPILED STATIC PLANS (Phase 6 of the consolidated content
// architecture — docs/pak_vnext.md): the PRIVATE generated constants
// that let a shipping executable load a mesh with NO runtime name
// search, path normalization, or pak TOC walk — and FAIL CLOSED when
// the deployed content's layout does not match what was compiled.
//
// THE SHAPE (the design's "compile-time physical plan"):
//   - The generated `project_plans.h` (HEADER_DIR, PRIVATE — never
//     the game-facing project.h) carries one `MeshPlanRef` constant
//     per mesh asset, keyed by the pak's STABLE SLOT ordinals (the
//     SMP8 addressing) + the [paks] mount INDEX — never a TOC
//     position, never a path join.
//   - `kPlansLayoutHash` (same file) folds EVERY assumption the
//     compiled reads make: the pak list (mount order + names), each
//     pak's slot table (slot, kind, codec flags, length, path hash —
//     in SLOT order), and the cooked mesh-plan join verbatim.
//     Content bytes are NOT part of it (the SMP8 per-page contentCrc
//     owns runtime integrity): a payload-only redeploy with an
//     unchanged layout needs NO recompile — the acceptance contract.
//   - The RUNTIME GATE (`ProjectRuntime::AttachCompiledPlans`): fold
//     the DEPLOYED paks the same way and compare — a mismatch is a
//     LOUD abort (recook + rebuild), NEVER a silent fallback to the
//     dynamic resolver.
//
// The per-pak + list folds live in the ENGINE (pak.cpp) so the
// generator (tools) and the gate (runtime) share ONE implementation —
// two drifting folds would be a silent-accept bug.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Engine/Asset/pak.h>

namespace tour::asset {

// One compiled mesh plan (the generated constants' type): the
// slot-keyed physical identity of one mesh asset family. `pakIndex`
// indexes the appmanifest's [paks] mount order; `baseSlot` is the
// family's BASE mesh entry's stable pak slot (LOD siblings derive
// through the pak's mesh plan at validate time — the plan never
// carries the join, it carries the ADDRESS); `familyPath` is the
// .geometry identity (the compiled key — a static literal, never a
// runtime-computed join).
struct MeshPlanRef {
    uint32_t pakIndex = 0;
    uint32_t baseSlot = 0;
    const char* familyPath = "";
};

// The LAYOUT fingerprint of one pak: everything a compiled read
// assumes about it — the slot table (slot, kind, codec flags,
// length, path hash — folded in SLOT order) + the cooked mesh-plan
// join blob verbatim. Content bytes are deliberately NOT folded (the
// per-page contentCrc owns them): equal fingerprints + different
// bytes = a payload-only redeploy, no recompile.
[[nodiscard]] std::string PakLayoutFingerprint(const PakReader& reader);

// The LIST fold (the generator + the runtime gate share it): the
// mount-ordered pak names + their per-pak layout fingerprints. The
// names are the MANIFEST's logical pak ids (never the
// content-addressed file names — those are the publication layer).
[[nodiscard]] std::string FoldPakLayouts(const std::string* pakNames,
                                         const std::string* pakFingerprints,
                                         size_t pakCount);

} // namespace tour::asset

namespace smvk {
using ::tour::asset::FoldPakLayouts;
using ::tour::asset::MeshPlanRef;
using ::tour::asset::PakLayoutFingerprint;
} // namespace smvk
