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
//
// ASYNC MODEL (fixed-size resource bins): page deserialization runs OFF the
// render thread on a fixed number of dedicated bin threads. Each bin leases a
// unique pool shard (see Engine/Memory/pool.h); every page is hash-routed to
// a bin by its scene index, so a page's uploads (textures + mesh geometry)
// always land in ONE known shard and the bin flushes them locally
// (FlushUploadsLocal). The render thread's Tick() only unloads, enqueues
// (with a semantic priority + a fixed-size backpressure bound) and folds
// completed pages (RegisterPage + MergePage) — it never does a cross-shard
// flush while a bin is recording.

#include <Engine/App/world.h>
#include <Engine/Asset/pak.h>
#include <Engine/Memory/pool.h>                   // shard-lease API + kNumPoolShards
#include <Engine/RHI/resource_manager.h>          // ResourceManager::FlushUploads (complete type)
#include <Engine/WorldPartition/WorldPartition.h> // IWorldStreamProvider (scene provider seam)

#include <glm/glm.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tour {

// Semantic streaming priority: schedules page deserialization order within
// the fixed-size async bins. LOWER numeric value = HIGHER priority (a bin
// services the lowest-priority-value pending request first). Distinct from
// raw scheduling policy — it carries the page's INTENT so the bins can
// preempt a cheap BACKGROUND prefetch for a camera-facing HIGH page without
// any knowledge of camera math. Stable across runs (an explicit enum, not a
// float score) so two pages never tie on an epsilon.
enum class StreamingPriority : uint8_t {
    Critical = 0,   // pinned entry scene + immediate visibility; never deferred
    High = 1,       // camera-facing pages inside the detail radius
    Normal = 2,     // inside the radius, not camera-facing
    Low = 3,        // radius edge / eviction candidates
    Background = 4, // prefetch / speculatively wanted, lowest
};

class ScenePageStreamer : public engine::worldpartition::IWorldStreamProvider {
  public:
    // `backend` is the pak archive's whole-image backend: the streamer owns a
    // SHARED reference so the PakReader (and the "#world" manifest view's
    // borrowed payload span) stay alive for the session. A bare reader
    // reference would dangle once the host loader returns.
    ScenePageStreamer(World& world,
                      std::shared_ptr<const smvk::PakBackend> backend,
                      smvk::ResourceManager& ctx,
                      smvk::TextureStreamer* sceneStreamer,
                      std::shared_ptr<const asset::PakImage> imagePin,
                      const smvk::ExternalTextureResolver* externalTexture,
                      asset::WorldManifestView manifest,
                      uint32_t entryScene,
                      glm::vec3 cameraPos = {},
                      glm::vec3 viewDir = {})
        : world_(world), backend_(std::move(backend)), ctx_(ctx), sceneStreamer_(sceneStreamer),
          imagePin_(std::move(imagePin)), externalTexture_(externalTexture), manifest_(manifest),
          entryScene_(entryScene), viewDir_(viewDir), loaded_(manifest.sceneCount, nullptr),
          dispatched_(manifest.sceneCount, false) {
        // Fixed bin count, clamped to the leaseable shards (shard 0 is the
        // main thread's; bins lease [1, kNumPoolShards)).
        uint32_t n = smvk::mem::config::kStreamBinCount;
        if (n == 0)
            n = 1;
        if (n >= smvk::kNumPoolShards)
            n = smvk::kNumPoolShards - 1;
        // Lease every shard BEFORE spawning (a scarcity shrinks the bin count
        // cleanly — no thread ever sees a half-initialized bin). The Bin is
        // heap-owned (unique_ptr): its mutex/thread/condition_variable are
        // non-movable, so the vector stores stable pointers, never moves them.
        std::vector<uint32_t> leases;
        leases.reserve(n);
        for (uint32_t b = 0; b < n; ++b) {
            const uint32_t shard = smvk::AcquirePoolShardLease();
            if (shard == smvk::kNoShardLease)
                break;
            leases.push_back(shard);
        }
        bins_.reserve(leases.size());
        for (uint32_t shard : leases) {
            auto bin = std::make_unique<Bin>();
            bin->shard = shard;
            Bin* binPtr = bin.get();
            bins_.push_back(std::move(bin));
            binPtr->thread = std::thread([this, binPtr] { BinThreadMain(binPtr); });
        }
        if (bins_.empty())
            std::cerr << "[PageStreamer] no pool shard available; every shard is leased by live"
                         " pools — streaming falls back to synchronous loads\n";
    }

    ~ScenePageStreamer() {
        for (auto& bin : bins_) {
            {
                std::lock_guard<std::mutex> lock(bin->mutex);
                bin->stop = true;
            }
            bin->cvRequest.notify_all();
        }
        for (auto& bin : bins_)
            if (bin->thread.joinable())
                bin->thread.join();
    }

    // Loads pages near the camera + unloads far pages. Returns true when the
    // page set changed (the merged scene was rebuilt + uploads flushed).
    bool Tick(glm::vec3 cameraPos, glm::vec3 viewDir = {}) {
        if (viewDir != glm::vec3(0.0f))
            viewDir_ = viewDir;
        bool changed = false;

        // Unload far pages FIRST (incremental retire; never the pinned
        // entry). Doing this before the load pass keeps the dense page slots
        // stable for the newly-appended pages below.
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

        // Fold pages the bins finished deserializing (render thread only:
        // RegisterPage + MergePage mutate world_.pages / the merged scene).
        changed |= FoldCompleted(cameraPos);

        // Enqueue near pages (hash-routed, priority-ordered, bounded). Bounded
        // per tick AND by the resident-mesh budget: loading stops once the
        // next page would exceed the budget (a dense multi-district area can
        // stream in more meshes than the GPU-scene admission ceiling). The
        // skipped page stays unloaded and streams in when a far page evicts.
        uint32_t enqueuedThisTick = 0;
        for (uint32_t i = 0; i < manifest_.sceneCount; ++i) {
            if (loaded_[i] || dispatched_[i] || i == entryScene_ || !WithinRadius(i, cameraPos))
                continue;
            if (maxLoadedMeshes_ != 0 &&
                world_.meshLocations.size() + manifest_.Meshes(i) > maxLoadedMeshes_)
                continue; // budget exhausted this tick; retry after an eviction
            if (bins_.empty()) {
                // Degenerate: no shard could be leased — deserialize inline on
                // the render thread (the pre-async behavior).
                if (!LoadOneSync(i, changed))
                    continue;
            } else if (!Enqueue(i, PriorityFor(i, cameraPos))) {
                continue; // bin full (backpressure): retry next tick
            }
            ++enqueuedThisTick;
            if (enqueuedThisTick >= kMaxLoadsPerTick)
                break;
        }

        if (changed) {
            // One O(total) derived-cache rebuild per settled batch (not per
            // page): iterator flags, draw list, LOD chains, static BVH.
            world_.RebuildDerivedCaches(/*incremental=*/true);
        }
        return changed;
    }

    float RadiusMeters() const {
        return radiusMeters_;
    }
    void SetRadiusMeters(float r) {
        radiusMeters_ = r;
    }
    void SetMaxLoadedMeshes(uint32_t n) {
        maxLoadedMeshes_ = n;
    }
    // Fed each frame by the renderer (the view matrix is built in the
    // frame-uniform pass, one frame after the provider's CommitFrame runs);
    // the facing bias tolerates the one-frame lag.
    void SetViewDirection(glm::vec3 viewDir) {
        viewDir_ = viewDir;
    }
    size_t LoadedCount() const {
        size_t n = 0;
        for (const WorldPage* p : loaded_)
            if (p)
                ++n;
        return n;
    }

    // ---- IWorldStreamProvider (Scene layer) ----
    // The streamer is a first-class WorldPartition scene provider: the frame's
    // Scene-layer StreamingInterest (camera position + radius) drives district
    // residency through the shared interest/commit sequence instead of the
    // ad-hoc World::StreamPages tick. The district `.geometry` pages are the
    // canonical per-district tiles.
    const char* ProviderName() const override {
        return "SceneGeometry";
    }
    void OnStreamingInterests(const engine::worldpartition::StreamingInterest* interests,
                              size_t count) override {
        // Default (no Scene interest) leaves the streamer idle for the frame:
        // no new loads/unloads. The renderer emits a Scene interest whenever a
        // page streamer is present, so this only stalls when the interest is
        // intentionally suppressed.
        hasInterest_ = false;
        if (!interests)
            return;
        for (size_t i = 0; i < count; ++i) {
            const auto& in = interests[i];
            if (!(in.layerMask & engine::worldpartition::StreamLayerBit(
                                     engine::worldpartition::StreamLayer::Scene)))
                continue;
            pendingCameraPos_ = glm::vec3(in.position);
            pendingRadius_ = float(in.radiusMeters);
            hasInterest_ = true;
            break; // interests are priority-sorted; the first Scene interest wins
        }
    }
    void CommitFrame(uint64_t) override {
        if (!hasInterest_)
            return;
        // The forward-facing bias (viewDir_) is fed separately; commit runs
        // before the frame-uniform build, so the camera position is current.
        if (pendingRadius_ > 0.0f)
            radiusMeters_ = pendingRadius_;
        Tick(pendingCameraPos_);
    }

    static constexpr uint32_t kMaxLoadsPerTick = 4;

  private:
    struct PageRequest {
        uint32_t sceneIndex = 0;
        StreamingPriority priority = StreamingPriority::Background;
    };
    struct PageResult {
        uint32_t sceneIndex = 0;
        std::unique_ptr<WorldPage> page; // null = deserialize failed
    };
    struct Bin {
        uint32_t shard = smvk::kNoShardLease;
        std::thread thread;
        std::mutex mutex;                  // guards pending + done + stop
        std::condition_variable cvRequest; // signaled on enqueue / stop
        std::vector<PageRequest> pending;  // priority-ordered pop
        std::vector<PageResult> done;      // drained by the render thread
        bool stop = false;
    };

    // Stable page->bin affinity: a fixed multiplicative hash over the scene
    // index (never a pointer), so a page's uploads always land in ONE known
    // shard and its bin flushes deterministically.
    static uint32_t HashBin(uint32_t sceneIndex, uint32_t binCount) noexcept {
        return (sceneIndex * 0x9E3779B1u) % binCount;
    }

    bool Enqueue(uint32_t sceneIndex, StreamingPriority priority) {
        Bin& bin = *bins_[HashBin(sceneIndex, uint32_t(bins_.size()))];
        std::unique_lock<std::mutex> lock(bin.mutex);
        if (bin.pending.size() >= smvk::mem::config::kStreamMaxPendingPerBin)
            return false; // fixed-size backpressure: retry next tick
        bin.pending.push_back(PageRequest{sceneIndex, priority});
        dispatched_[sceneIndex] = true;
        lock.unlock();
        bin.cvRequest.notify_one();
        return true;
    }

    // Synchronous fallback (no shard could be leased): deserialize + register
    // + merge on the render thread, exactly the pre-async behavior.
    bool LoadOneSync(uint32_t sceneIndex, bool& changed) {
        try {
            WorldPage& page = world_.LoadPageFromPak(
                ctx_, backend_->Reader(), sceneIndex, sceneStreamer_, imagePin_, externalTexture_);
            loaded_[sceneIndex] = &page;
            world_.MergePage(page, uint32_t(world_.pages.size()) - 1, false);
            changed = true;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[PageStreamer] load page " << sceneIndex << " failed: " << e.what()
                      << "\n";
            return false;
        }
    }

    // Drains every bin's completed pages and folds them on the render thread.
    // Returns true when any page was registered + merged.
    bool FoldCompleted(glm::vec3 cameraPos) {
        bool folded = false;
        for (auto& binPtr : bins_) {
            Bin& bin = *binPtr;
            std::vector<PageResult> results;
            {
                std::lock_guard<std::mutex> lock(bin.mutex);
                results.swap(bin.done);
            }
            for (PageResult& r : results) {
                dispatched_[r.sceneIndex] = false;
                if (!r.page)
                    continue; // deserialize failed (logged in DeserializeCluster)
                // Re-check at fold time: a page wanted at enqueue may be behind
                // the camera (or over budget) by the time it finished.
                if (loaded_[r.sceneIndex] || !WithinRadius(r.sceneIndex, cameraPos))
                    continue; // discard (r.page destroys here)
                if (maxLoadedMeshes_ != 0 &&
                    world_.meshLocations.size() + manifest_.Meshes(r.sceneIndex) > maxLoadedMeshes_)
                    continue; // budget exhausted: discard, retry on eviction
                try {
                    WorldPage* pagePtr = r.page.get();
                    world_.RegisterPage(std::move(r.page));
                    world_.MergePage(*pagePtr, uint32_t(world_.pages.size()) - 1, false);
                    loaded_[r.sceneIndex] = pagePtr;
                    folded = true;
                } catch (const std::exception& e) {
                    std::cerr << "[PageStreamer] fold page " << r.sceneIndex
                              << " failed: " << e.what() << "\n";
                }
            }
        }
        return folded;
    }

    void BinThreadMain(Bin* bin) {
        smvk::SetCurrentPoolShard(bin->shard);
        for (;;) {
            PageRequest req;
            {
                std::unique_lock<std::mutex> lock(bin->mutex);
                bin->cvRequest.wait(lock, [&] { return bin->stop || !bin->pending.empty(); });
                if (bin->stop) {
                    lock.unlock();
                    smvk::ReleasePoolShardLease(bin->shard);
                    return;
                }
                // Pop the highest-priority request (lowest enum value).
                const auto best = std::min_element(bin->pending.begin(),
                                                   bin->pending.end(),
                                                   [](const PageRequest& a, const PageRequest& b) {
                                                       if (a.priority != b.priority)
                                                           return a.priority < b.priority;
                                                       return a.sceneIndex < b.sceneIndex;
                                                   });
                req = *best;
                bin->pending.erase(best);
            }

            // Deserialize on THIS thread. Serial KTX (textureMaxThreads=1) so
            // every upload (textures + mesh geometry) lands in THIS shard, and
            // localUploadDrain so a failure drains only this shard.
            std::string err;
            std::unique_ptr<WorldPage> page = smvk::DeserializeCluster(ctx_,
                                                                       backend_->Reader(),
                                                                       &err,
                                                                       req.sceneIndex,
                                                                       sceneStreamer_,
                                                                       imagePin_,
                                                                       externalTexture_,
                                                                       /*sceneNodeBudget=*/0,
                                                                       /*textureMaxThreads=*/1,
                                                                       /*localUploadDrain=*/true);
            if (!page) {
                std::cerr << "[PageStreamer] deserialize page " << req.sceneIndex
                          << " failed: " << err << "\n";
            } else {
                ctx_.FlushUploadsLocal();
            }

            {
                std::lock_guard<std::mutex> lock(bin->mutex);
                bin->done.push_back(PageResult{req.sceneIndex, std::move(page)});
            }
        }
    }

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

    // Camera-facing pages load first (High); non-facing in-radius pages are
    // Normal. (Low/Background are reserved for future prefetch tiers.)
    StreamingPriority PriorityFor(uint32_t sceneIndex, glm::vec3 cameraPos) const {
        glm::vec3 mn, mx;
        manifest_.Aabb(sceneIndex, mn, mx);
        const glm::vec3 center = (mn + mx) * 0.5f;
        const float viewLen2 = glm::dot(viewDir_, viewDir_);
        if (viewLen2 > 0.0f && glm::dot(center - cameraPos, viewDir_) > 0.0f)
            return StreamingPriority::High;
        return StreamingPriority::Normal;
    }

    World& world_;
    std::shared_ptr<const smvk::PakBackend> backend_;
    smvk::ResourceManager& ctx_;
    smvk::TextureStreamer* sceneStreamer_;
    std::shared_ptr<const asset::PakImage> imagePin_;
    const smvk::ExternalTextureResolver* externalTexture_;
    asset::WorldManifestView manifest_; // fixed-size, offset-indexed (no copy)
    std::vector<WorldPage*> loaded_;    // sceneIndex -> registered page (flat index)
    std::vector<bool> dispatched_;      // sceneIndex -> queued/in-flight on a bin
    uint32_t entryScene_ = 0;
    glm::vec3 viewDir_{0.0f}; // forward (zero = unbiased spherical radius)
    float radiusMeters_ = 256.0f;
    // Scene-layer interest snapshot (see OnStreamingInterests/CommitFrame).
    glm::vec3 pendingCameraPos_{0.0f};
    float pendingRadius_ = 0.0f;
    bool hasInterest_ = false;
    // Resident-mesh budget (0 = unbounded): the load pass stops admitting
    // pages once the merged world's mesh count would exceed this. The host
    // sets it to the GPU-scene admission ceiling (kMaxMeshes) so streaming
    // never oversubscribes the GPU-driven path.
    uint32_t maxLoadedMeshes_ = 0;
    std::vector<std::unique_ptr<Bin>> bins_; // heap-owned (Bin is non-movable)
};

} // namespace tour
