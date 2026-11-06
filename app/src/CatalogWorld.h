// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once

// Node-first game-facing World. Catalog keys only enter at LoadMesh;
// nodes hold ONE transform and at most ONE loaded mesh binding.
// This is the application layer: the engine World deliberately does not
// depend on AssetManager, ProjectRuntime or generated project headers.
#include "AssetManager.h"
#include <Engine/App/world.h>

#include <cstdint>
#include <string>

namespace app {

class CatalogWorld {
public:
	explicit CatalogWorld(tour::World& world, AssetManager& assets)
		: world_(world), assets_(assets) {}

	// Explicit resource acquisition. Caller owns the resulting handle;
	// keep it loaded while any node refers to it. Do not pass a raw
	// world row or catalog key to Node::SetMesh.
	[[nodiscard]] MeshHandle LoadMesh(uint64_t catalogKey,
		std::string& err, const MeshLoadOptions& options = {}) {
		return assets_.LoadMesh(catalogKey, err, options);
	}

	[[nodiscard]] NodeHandle AddNode(
		const glm::mat4& frame = glm::mat4(1.0f)) {
		return world_.AddNode(frame);
	}
	[[nodiscard]] bool RemoveNode(NodeHandle node) {
		return world_.RemoveNode(node);
	}
	[[nodiscard]] bool IsNodeAlive(NodeHandle node) const {
		return world_.IsGameNode(node);
	}

	class Node {
	public:
		Node(tour::World& world, AssetManager& assets, NodeHandle handle)
			: world_(world), assets_(assets), handle_(handle) {}

		// The loaded app::MeshHandle is a stable asset identity, not a
		// world mesh-row index. Resolve its CURRENT row when assigning.
		// SetMesh replaces the node's sole mesh association in place.
		// An invalid/unmapped asset leaves the old mesh untouched.
		[[nodiscard]] bool SetMesh(MeshHandle mesh) const {
			if (!world_.IsGameNode(handle_) || !assets_.MeshValid(mesh))
				return false;
			const uint32_t row = assets_.MeshWorldIndex(mesh);
			if (row == UINT32_MAX)
				return false;
			if (!tour::scene::SetMesh(world_.scene, NodeHandleSlot(handle_), row))
				return false;
			world_.RefreshRuntimeSceneCaches();
			return true;
		}

		[[nodiscard]] bool RemoveMesh() const {
			if (!world_.IsGameNode(handle_))
				return false;
			if (!tour::scene::RemoveMesh(world_.scene, NodeHandleSlot(handle_)))
				return false;
			world_.RefreshRuntimeSceneCaches();
			return true;
		}

		[[nodiscard]] bool SetLocalTransform(const glm::mat4& frame) const {
			return world_.IsGameNode(handle_)
				&& tour::scene::SetLocalTransform(world_.scene, handle_, frame);
		}

		// Inline value components: concrete signatures accept {} without
		// requiring catalog IDs or template type deduction.
		[[nodiscard]] bool SetPhysicsBody(const ScenePhysicsBody& value) const {
			return world_.IsGameNode(handle_) && tour::scene::SetPhysicsBody(
				world_.scene, NodeHandleSlot(handle_), value);
		}
		[[nodiscard]] bool SetAudioRoom(const SceneAudioRoom& value) const {
			return world_.IsGameNode(handle_) && tour::scene::SetAudioRoom(
				world_.scene, NodeHandleSlot(handle_), value);
		}
		[[nodiscard]] bool SetTriggerVolume(const SceneTriggerVolume& value) const {
			return world_.IsGameNode(handle_) && tour::scene::SetTriggerVolume(
				world_.scene, NodeHandleSlot(handle_), value);
		}

		[[nodiscard]] bool Alive() const {
			return world_.IsGameNode(handle_);
		}
	private:
		tour::World& world_;
		AssetManager& assets_;
		NodeHandle handle_;
	};

	[[nodiscard]] Node operator[](NodeHandle handle) {
		return Node(world_, assets_, handle);
	}

private:
	tour::World& world_;
	AssetManager& assets_;
};

} // namespace app
