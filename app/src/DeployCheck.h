// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE DEPLOYMENT VALIDATION GATE (pre-graphics, the checklist's
// runtime contract): a project launch PROVES its deployment before
// any Vulkan/window work —
//   1. every manifest-listed pak EXISTS (an incomplete deployment is
//      a loud, actionable failure, never a half-booted window);
//   2. a CONTENT-ADDRESSED pak name (<stem>-<fnv1a64 16hex>.pak)
//      matches the file's ACTUAL bytes (the deployment contract: a
//      stale or wrong pak sitting under the expected name fails even
//      though its own CRC is internally valid);
//   3. an ENGINE SHADER SOURCE exists (the engine-shaders.pak package
//      or the loose dev tree — the mandatory dependency of every
//      graphics host).
// Integrity beyond the name check (magic/footer/CRC) is the preload's
// PakReader::Open; this gate runs FIRST, cheap-to-loud, so a broken
// deployment never reaches app initialization.

#include <string>

namespace app {

// One pak's deployment proof: exists + opens (PakReader magic/CRC) +
// the content-addressed name (when the name carries a digest) matches
// the actual bytes. A PLAIN (non-addressed) name passes on existence
// + a valid image (the legacy single-pak layout). False + err with
// the actionable reason.
[[nodiscard]] bool ValidatePakFile(const std::string& pakPath,
	std::string& err);

// The engine shader source proof: the shader library initializes
// (exe-relative discovery — the engine-shaders.pak package and/or the
// loose dev tree) and at least ONE source is mounted. False + err
// naming the expected locations.
[[nodiscard]] bool ValidateEngineShaderSource(std::string& err);

} // namespace app
