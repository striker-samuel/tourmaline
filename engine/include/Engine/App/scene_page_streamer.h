// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Camera-driven page streaming over a partitioned-family world: loads the
// scene pages whose world AABB is within `radius` of the camera and unloads
// pages beyond it. The world manifest (the fixed-size scene-list table with
// per-scene AABBs) drives the distance check; the entry scene is PINNED
// (never unloaded). The host loader creates this after loading the entry
// scene and the renderer ticks it once per frame with the current camera
// position.

#include <Engine/App/world.h>
#include <Engine/Asset/pak.h>
#include <Engine/RHI/resource_manager.h> // ResourceManager::FlushUploads (the complete type)

#include <glm/glm.hpp>

#include <cstdint>
#include <iostream>
#include <vector>

namespace tour {

class ScenePageStreamer {
  public:
    // `manifest` is a WorldManifestView over the reader's "#world" blob — a
    // borrowed, fixed-size, offset-indexed table (no copy, no per-row
    // allocation). The view's bytes must outlive the streamer (they are the
    // PakReader's payload span, which the host keeps for the session).
    ScenePageStreamer(World& world,
                      const asset::PakReader& reader,
                      smvk::ResourceManager& ctx,
                      smvk::TextureStreamer* sceneStreamer,
                      std::shared_ptr<const std::vector<uint8_t>> imagePin,
                      const smvk::ExternalTextureResolver* externalTexture,
                      asset::WorldManifestView manifest,
                      uint32_t entryScene)
        : world_(world), reader_(reader), ctx_(ctx), sceneStreamer_(sceneStreamer),
          imagePin_(std::move(imagePin)), externalTexture_(externalTexture), manifest_(manifest),
          entryScene_(entryScene), loaded_(manifest.sceneCount, nullptr) {}

    // Loads pages near the camera + unloads far pages. Returns true when the
    // page set changed (the merged scene was rebuilt + uploads flushed).
    bool Tick(glm::vec3 cameraPos) {
        bool changed = false;
        uint32_t loadedThisTick = 0;
        // Load near pages (bounded per tick to avoid a multi-page hitch).
        for (uint32_t i = 0; i < manifest_.sceneCount; ++i) {
            if (loaded_[i] || !WithinRadius(i, cameraPos))
                continue;
            try {
                WorldPage& page = world_.LoadPageFromPak(
                    ctx_, reader_, i, sceneStreamer_, imagePin_, externalTexture_);
                loaded_[i] = &page;
                ++loadedThisTick;
                changed = true;
            } catch (const std::exception& e) {
                std::cerr << "[PageStreamer] load page " << i << " failed: " << e.what() << "\n";
            }
            if (loadedThisTick >= kMaxLoadsPerTick)
                break;
        }
        // Unload far pages (never the pinned entry).
        for (uint32_t i = 0; i < manifest_.sceneCount; ++i) {
            if (!loaded_[i] || i == entryScene_ || WithinRadius(i, cameraPos))
                continue;
            try {
                world_.UnloadCluster(*loaded_[i]);
            } catch (const std::exception& e) {
                std::cerr << "[PageStreamer] unload page " << i << " failed: " << e.what() << "\n";
            }
            loaded_[i] = nullptr;
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
        size_t n = 0;
        for (const WorldPage* p : loaded_)
            if (p)
                ++n;
        return n;
    }

    static constexpr uint32_t kMaxLoadsPerTick = 4;

  private:
    // Squared distance from the camera to the scene's AABB (0 when inside).
    bool WithinRadius(uint32_t sceneIndex, glm::vec3 cameraPos) const {
        if (sceneIndex >= manifest_.sceneCount)
            return false;
        glm::vec3 mn, mx;
        manifest_.Aabb(sceneIndex, mn, mx);
        const glm::vec3 closest = glm::max(mn, glm::min(cameraPos, mx));
        const float d2 = glm::dot(cameraPos - closest, cameraPos - closest);
        return d2 <= radiusMeters_ * radiusMeters_;
    }

    World& world_;
    const asset::PakReader& reader_;
    smvk::ResourceManager& ctx_;
    smvk::TextureStreamer* sceneStreamer_;
    std::shared_ptr<const std::vector<uint8_t>> imagePin_;
    const smvk::ExternalTextureResolver* externalTexture_;
    asset::WorldManifestView manifest_; // fixed-size, offset-indexed (no copy)
    std::vector<WorldPage*> loaded_;    // sceneIndex -> registered page (flat index)
    uint32_t entryScene_ = 0;
    float radiusMeters_ = 256.0f;
};

} // namespace tour
