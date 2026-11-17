// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Camera-driven page streaming over a partitioned-family world: loads the
// scene pages whose world AABB is within `radius` of the camera and unloads
// pages beyond it. The world manifest (the scene-list table with per-scene
// AABBs) drives the distance check; the entry scene is PINNED (never
// unloaded). The host loader creates this after loading the entry scene and
// the renderer ticks it once per frame with the current camera position.

#include <Engine/App/world.h>
#include <Engine/Asset/pak.h>
#include <Engine/RHI/resource_manager.h> // ResourceManager::FlushUploads (the complete type)

#include <glm/glm.hpp>

#include <cstdint>
#include <iostream>
#include <unordered_map>

namespace tour {

class ScenePageStreamer {
  public:
    ScenePageStreamer(World& world,
                      const asset::PakReader& reader,
                      smvk::ResourceManager& ctx,
                      smvk::TextureStreamer* sceneStreamer,
                      std::shared_ptr<const std::vector<uint8_t>> imagePin,
                      const smvk::ExternalTextureResolver* externalTexture,
                      std::vector<asset::WorldManifestScene> manifest,
                      uint32_t entryScene)
        : world_(world), reader_(reader), ctx_(ctx), sceneStreamer_(sceneStreamer),
          imagePin_(std::move(imagePin)), externalTexture_(externalTexture),
          manifest_(std::move(manifest)), entryScene_(entryScene) {}

    // Loads pages near the camera + unloads far pages. Returns true when the
    // page set changed (the merged scene was rebuilt + uploads flushed).
    bool Tick(glm::vec3 cameraPos) {
        bool changed = false;
        size_t loadedThisTick = 0;
        // Load near pages (bounded per tick to avoid a multi-page hitch).
        for (uint32_t i = 0; i < manifest_.size(); ++i) {
            if (loaded_.count(i) || !WithinRadius(i, cameraPos))
                continue;
            try {
                WorldPage& page = world_.LoadPageFromPak(
                    ctx_, reader_, i, sceneStreamer_, imagePin_, externalTexture_);
                loaded_.emplace(i, &page);
                ++loadedThisTick;
                changed = true;
            } catch (const std::exception& e) {
                std::cerr << "[PageStreamer] load page " << i << " failed: " << e.what() << "\n";
            }
            if (loadedThisTick >= kMaxLoadsPerTick)
                break;
        }
        // Unload far pages (never the pinned entry).
        for (auto it = loaded_.begin(); it != loaded_.end();) {
            const uint32_t i = it->first;
            if (i == entryScene_ || WithinRadius(i, cameraPos)) {
                ++it;
                continue;
            }
            try {
                world_.UnloadCluster(*it->second);
            } catch (const std::exception& e) {
                std::cerr << "[PageStreamer] unload page " << i << " failed: " << e.what() << "\n";
            }
            it = loaded_.erase(it);
            changed = true;
        }
        if (changed) {
            world_.ResolveAndFixup_pages();
            ctx_.FlushUploads();
        }
        return changed;
    }

    float RadiusMeters() const {
        return radiusMeters_;
    }
    void SetRadiusMeters(float r) {
        radiusMeters_ = r;
    }
    size_t LoadedCount() const {
        return loaded_.size();
    }

    static constexpr size_t kMaxLoadsPerTick = 4;

  private:
    // Squared distance from the camera to the scene's AABB (0 when inside).
    bool WithinRadius(uint32_t sceneIndex, glm::vec3 cameraPos) const {
        if (sceneIndex >= manifest_.size())
            return false;
        const asset::WorldManifestScene& s = manifest_[sceneIndex];
        const glm::vec3 closest = glm::max(s.aabbMin, glm::min(cameraPos, s.aabbMax));
        const float d2 = glm::dot(cameraPos - closest, cameraPos - closest);
        return d2 <= radiusMeters_ * radiusMeters_;
    }

    World& world_;
    const asset::PakReader& reader_;
    smvk::ResourceManager& ctx_;
    smvk::TextureStreamer* sceneStreamer_;
    std::shared_ptr<const std::vector<uint8_t>> imagePin_;
    const smvk::ExternalTextureResolver* externalTexture_;
    std::vector<asset::WorldManifestScene> manifest_;
    std::unordered_map<uint32_t, WorldPage*> loaded_; // sceneIndex -> registered page
    uint32_t entryScene_ = 0;
    float radiusMeters_ = 256.0f;
};

} // namespace tour
