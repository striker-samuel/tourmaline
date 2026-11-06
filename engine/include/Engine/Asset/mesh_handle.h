// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// A loaded resource identity, distinct from a compiled catalog key and
// from a World node or entity handle. AssetManager owns the lifetime;
// World nodes may borrow it while it remains loaded.
//
// Kept in an engine-visible header so World::LoadMesh and NodeRef::SetMesh
// are usable without including the application host's AssetManager.h.
namespace app {

struct MeshHandle {
	std::uint64_t value = 0;
	constexpr bool operator==(const MeshHandle&) const = default;
	[[nodiscard]] constexpr explicit operator bool() const {
		return value != 0;
	}
};

} // namespace app
