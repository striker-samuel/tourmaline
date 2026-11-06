// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <utility>

#include <glm/mat4x4.hpp>

#include <Engine/App/world.h>

// Game-facing spawn options. Keep asset lookup in the caller: a mesh index
// is a CURRENT World location, not a persistent asset id. Resolve a typed
// asset key again before spawning after a content topology change.
//
// Parent assignment is deliberately not exposed: current mesh and audio
// spawn implementations ignore their parent parameter. Do not offer a
// fluent operation that silently fails to parent game objects.
// This facade does not create independent transform-only nodes yet:
// World::scene must become authoritative (Epoch 6c/6d) before exposing
// long-lived free nodes or arbitrary attachments safely.
namespace tour::game {

class Spawner {
public:
	explicit Spawner(World& world) : world_(world) {}

	class MeshRequest {
	public:
		MeshRequest(World& world, uint32_t meshIndex)
			: world_(world), meshIndex_(meshIndex) {}

		[[nodiscard]] MeshRequest& At(const glm::mat4& localFrame) {
			frame_ = localFrame;
			return *this;
		}
		[[nodiscard]] MeshRequest& Material(uint32_t materialIndex) {
			material_ = materialIndex;
			return *this;
		}
		[[nodiscard]] smvk::MeshEntityHandle Spawn() const {
			return world_.SpawnMesh(meshIndex_, frame_, UINT32_MAX, material_);
		}
	private:
		World& world_;
		uint32_t meshIndex_;
		uint32_t material_ = World::kInvalidIndex;
		glm::mat4 frame_{ 1.0f };
	};

	class MeshFromNodeRequest {
	public:
		MeshFromNodeRequest(World& world, NodeId source)
			: world_(world), source_(source) {}

		[[nodiscard]] MeshFromNodeRequest& At(const glm::mat4& localFrame) {
			frame_ = localFrame;
			return *this;
		}
		[[nodiscard]] smvk::MeshEntityHandle Spawn() const {
			return world_.SpawnMeshFromNode(source_, frame_, UINT32_MAX);
		}
	private:
		World& world_;
		NodeId source_;
		glm::mat4 frame_{ 1.0f };
	};

	class AudioRequest {
	public:
		AudioRequest(World& world, const SceneAudioSource& definition)
			: world_(world), definition_(definition) {}

		[[nodiscard]] AudioRequest& At(const glm::mat4& localFrame) {
			frame_ = localFrame;
			return *this;
		}
		[[nodiscard]] smvk::AudioSourceHandle Spawn() const {
			return world_.SpawnAudioSource(frame_, UINT32_MAX, definition_);
		}
	private:
		World& world_;
		SceneAudioSource definition_;
		glm::mat4 frame_{ 1.0f };
	};

	[[nodiscard]] MeshRequest Mesh(uint32_t worldMeshIndex) const {
		return MeshRequest(world_, worldMeshIndex);
	}
	[[nodiscard]] MeshFromNodeRequest MeshFromNode(NodeId sourceNode) const {
		return MeshFromNodeRequest(world_, sourceNode);
	}
	[[nodiscard]] AudioRequest Audio(const SceneAudioSource& definition) const {
		return AudioRequest(world_, definition);
	}
	[[nodiscard]] NodeId Decal(const glm::mat4& frame, SceneDecal definition) const {
		return world_.CreateDecalAt(frame, std::move(definition));
	}
private:
	World& world_;
};

} // namespace tour::game
