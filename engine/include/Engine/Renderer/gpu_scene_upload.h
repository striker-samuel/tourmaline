// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Opt-in RHI publication for the CPU database. No render-pass coupling.
// Create/destroy this BEFORE the ResourceManager that owns its buffers.

#include <Engine/RHI/FrameConfig.h>
#include <Engine/Renderer/gpu_scene.h>

#include <cstdint>
#include <memory>

namespace smvk {
class ResourceManager;
}

namespace tour::gpu_scene {

using RenderDatabase = Database<smvk::limits::MaxFramesInFlight>;

struct Addresses {
    uint64_t meshes = 0;
    uint64_t lods = 0;
    uint64_t submeshes = 0;
    uint64_t instances = 0;
    uint64_t leaves = 0;
    uint64_t leafMembers = 0;
};

class Uploader {
  public:
    Uploader();
    ~Uploader();

    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    // Call once per acquired/fenced frame flight, BEFORE recording any
    // consuming passes. UploadDataToBuffer queues transfers; the caller
    // must arrange FlushUploads + appropriate resource hazards.
    // On allocation failure returns false; dirty rows remain retryable.
    [[nodiscard]] bool Publish(smvk::ResourceManager& ctx, RenderDatabase& scene, uint32_t flight);

    [[nodiscard]] Addresses DeviceAddresses(const smvk::ResourceManager& ctx,
                                            uint32_t flight) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tour::gpu_scene
