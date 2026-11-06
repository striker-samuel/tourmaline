// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once

// The catalog is the public mesh-spawn vocabulary. Game code passes the
// generated project::meshes::<Name> key; it never sees ephemeral World
// mesh-location indices. AssetManager owns loaded resource handles and
// World owns entity handles. Neither is a substitute for the other.
#include "AssetManager.h"

#include <Engine/App/world.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace app {

// Keep this facade alive while its spawned mesh entities need its loaded
// resources. Destroy entities before tearing down this facade/AssetManager.
// Mesh resources are acquired once per catalog key and released at teardown.
// The renderer/world, AssetManager and ProjectRuntime must outlive this object.
class CatalogWorld {
public:
	CatalogWorld(tour::World& world, AssetManager& assets)
		: world_(world), assets_(assets) {}

	CatalogWorld(const CatalogWorld&) = delete;
	CatalogWorld& operator=(const CatalogWorld&) = delete;
	CatalogWorld(CatalogWorld&&) = delete;
	CatalogWorld& operator=(CatalogWorld&&) = delete;

	~CatalogWorld() {
		for (const auto& [key, mesh] : meshes_)
			assets_.UnloadMesh(mesh);
	}

	// 'meshKey' comes from project::meshes::Stone, or a generated
	// project::MeshKey. The asset is loaded on first use and reused.
	// A missing catalog key or a parked/unmapped carrier returns an
	// empty entity handle. Inspect LastError() for the failure reason.
	[[nodiscard]] smvk::MeshEntityHandle Add(
		uint64_t meshKey, const glm::mat4& frame = glm::mat4(1.0f))
	{
		lastError_.clear();
		MeshHandle mesh{};
		auto it = meshes_.find(meshKey);
		if (it != meshes_.end()) {
			mesh = it->second;
		}
		else {
			mesh = assets_.LoadMesh(meshKey, lastError_);
			if (!mesh)
				return {};
			meshes_.emplace(meshKey, mesh);
		}
		const auto entity = assets_.SpawnMesh(mesh, frame, UINT32_MAX);
		if (!entity)
			lastError_ = "catalog mesh is not mapped in the current World";
		return entity;
	}

	[[nodiscard]] const std::string& LastError() const {
		return lastError_;
	}

	// The world owns instances; the catalog owns resource identity.
	[[nodiscard]] bool Remove(smvk::MeshEntityHandle entity) {
		return world_.DestroyMesh(entity);
	}

private:
	tour::World& world_;
	AssetManager& assets_;
	std::unordered_map<uint64_t, MeshHandle> meshes_;
	std::string lastError_;
};

} // namespace app
