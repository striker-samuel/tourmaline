// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Internal registry plumbing shared between registry.cpp (parse/write/
// status) and import_pipeline.cpp (the row update at publish time).
// NOT part of the public pipeline surface.

#pragma once

#include <string>

#include <project_pipeline/import_pipeline.h>

namespace smvk::project::registry {

// Replaces-or-appends the row for `projectRelativeSource`, stamped with
// the import's OWN fingerprint (the settings it actually ran under) +
// the derived counts. True on a successful registry write.
bool UpdateRow(const Project& project, const ImportResult& result,
	const std::string& projectRelativeSource,
	const std::string& originalSource,
	const std::string& importFingerprint);

} // namespace smvk::project::registry
