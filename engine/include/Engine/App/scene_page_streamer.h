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
    // `backend` is the pak archive's whole-image backend: the streamer owns a
    // SHARED reference so the PakReader (and the "#world" manifest view's
    // borrowed payload span) stay alive for the session. A bare reader
    // reference would dangle once the host loader returns.
    ScenePageStreamer(World& world,
                      std::shared_ptr<const smvk::PakBackend> backend,
                      smvk::ResourceManager& ctx,
                      smvk::TextureStreamer* sceneStreamer,
                      std::shared_ptr<const std::vector<uint8_t>> imagePin,
                      const smvk::ExternalTextureResolver* externalTexture,
                      asset::WorldManifestView manifest,
                      uint32_t entryScene,
                      glm::vec3 cameraPos = {},
                      glm::vec3 viewDir = {})
        : world_(world), backend_(std::move(backend)), ctx_(ctx), sceneStreamer_(sceneStreamer),
          imagePin_(std::move(imagePin)), externalTexture_(externalTexture), manifest_(manifest),
          entryScene_(entryScene), viewDir_(viewDir), loaded_(manifest.sceneCount, nullptr) {}

    // Loads pages near the camera + unloads far pages. Returns true when the
    // page set changed (the merged scene was rebuilt + uploads flushed).
    // `viewDir` (the camera forward) biases the radius toward pages the camera
    // is facing; pass {} for an unbiased spherical radius.
    bool Tick(glm::vec3 cameraPos, glm::vec3 viewDir = {}) {
        if (viewDir != glm::vec3(0.0f))
            viewDir_ = viewDir;
        bool changed = false;
        // Unload far pages FIRST (incremental retire; never the pinned
        // entry). Doing this before the load pass keeps the dense page
        // slots stable for the newly-appended pages below.
        for (uint32_t i = 0; i < manifest_.sceneCount; ++i) {
            if (!loaded_[i] || i == entryScene_ || WithinRadius(i, cameraPos))
                continue;
            try {
                world_.UnmergePage(*loaded_[i]);
            } catch (const std::exception& e) {
                std::cerr << "[PageStreamer] unload page " << i << " failed: " << e.what() << "\n";
            }
            loaded_[i] = nullptr;
            changed = true;
        }
        // Load near pages (incremental fold; the pinned entry is already
        // loaded + folded by the host loader). Bounded per tick.
        uint32_t loadedThisTick = 0;
        for (uint32_t i = 0; i < manifest_.sceneCount; ++i) {
            if (loaded_[i] || i == entryScene_ || !WithinRadius(i, cameraPos))
                continue;
            try {
                WorldPage& page = world_.LoadPageFromPak(
                    ctx_, backend_->Reader(), i, sceneStreamer_, imagePin_, externalTexture_);
                loaded_[i] = &page;
                // Incremental fold: append this page's nodes/tables into
                // the merged scene (the page was just registered at the
                // tail of world_.pages). No full re-fold of the resident
                // pages — RebuildDerivedCaches runs once below.
                world_.MergePage(page, uint32_t(world_.pages.size()) - 1, false);
                ++loadedThisTick;
                changed = true;
            } catch (const std::exception& e) {
                std::cerr << "[PageStreamer] load page " << i << " failed: " << e.what() << "\n";
            }
            if (loadedThisTick >= kMaxLoadsPerTick)
                break;
        }
        if (changed) {
            // One O(total) derived-cache rebuild per settled batch (not
            // per page): iterator flags, draw list, LOD chains, static BVH.
            world_.RebuildDerivedCaches();
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
    // Squared distance from the camera to the scene's AABB (0 when inside),
    // plus a directional bias: when a view direction is known, a page whose
    // AABB center sits more than a radius BEHIND the camera is not loaded
    // (the camera is looking away from it).
    bool WithinRadius(uint32_t sceneIndex, glm::vec3 cameraPos) const {
        if (sceneIndex >= manifest_.sceneCount)
            return false;
        glm::vec3 mn, mx;
        manifest_.Aabb(sceneIndex, mn, mx);
        const glm::vec3 closest = glm::max(mn, glm::min(cameraPos, mx));
        const float d2 = glm::dot(cameraPos - closest, cameraPos - closest);
        if (d2 > radiusMeters_ * radiusMeters_)
            return false;
        const float viewLen2 = glm::dot(viewDir_, viewDir_);
        if (viewLen2 > 0.0f) {
            const glm::vec3 center = (mn + mx) * 0.5f;
            if (glm::dot(center - cameraPos, viewDir_) < -radiusMeters_)
                return false;
        }
        return true;
    }

    World& world_;
    std::shared_ptr<const smvk::PakBackend> backend_;
    smvk::ResourceManager& ctx_;
    smvk::TextureStreamer* sceneStreamer_;
    std::shared_ptr<const std::vector<uint8_t>> imagePin_;
    const smvk::ExternalTextureResolver* externalTexture_;
    asset::WorldManifestView manifest_; // fixed-size, offset-indexed (no copy)
    std::vector<WorldPage*> loaded_;    // sceneIndex -> registered page (flat index)
    uint32_t entryScene_ = 0;
    glm::vec3 viewDir_{0.0f}; // forward (zero = unbiased spherical radius)
    float radiusMeters_ = 256.0f;
};

} // namespace tour
