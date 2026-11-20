// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// P1 flat GPU multi-view leaf visibility runtime (#372,
// docs/gpu_scene_visibility_hierarchy_proposal.md). Owns the per-flight
// scratch buffers and builds the push constants for the three shaders:
//
//   gpu_scene_leaf_classify.cs.hlsl  -> per-leaf view mask
//   gpu_scene_leaf_compact.cs.hlsl   -> active-leaf work queue
//   gpu_scene_fused_cull.cs.hlsl     -> per-instance fused view mask
//
// The CPU stays authoritative for now: SelectPassedInstanceRangesFrame keeps
// owning the production path until the safe-overflow policy and the downstream
// fused-verdict consumption are wired. This runtime is the test/validation
// seam (TOURMALINE_GPU_SCENE_VISIBILITY) plus the buffer/push plumbing.
//
// Mirrors the CandidateValidator lifetime contract: Prepare runs after the
// flight's acquire fence and before recording; BeginFlight performs the
// validation readback (test mode only) on the NEXT use of that flight slot.

#include <Engine/RHI/FrameConfig.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/gpu_scene_fused_cull.h>
#include <Engine/Renderer/gpu_scene_leaf.h>
#include <Engine/Renderer/gpu_scene_leaf_classify.h>
#include <Engine/Renderer/gpu_scene_leaf_compact.h>
#include <Engine/Renderer/gpu_scene_upload.h>
#include <Engine/Renderer/gpu_visibility_frame_ref.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace tour::gpu_scene {

// Pack a CPU RelativeFrustum into the classifier's 128-byte view ABI
// (byte-identical to InstanceCullGpuView). Mirrors PackInstanceCullView.
[[nodiscard]] inline bool PackLeafClassifyView(const RelativeFrustum& view, LeafClassifyView& out) {
    if (!view.Valid())
        return false;
    out = {};
    out.origin = math::SplitDoubleFloat(view.originWorld);
    for (size_t i = 0; i < 6; ++i)
        for (int j = 0; j < 4; ++j) {
            const float v = float(view.planes[i][j]);
            if (!std::isfinite(v))
                return false;
            out.inwardPlanes[i][j] = v;
        }
    return true;
}

class GpuVisibilityRuntime {
  public:
    static constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
    static constexpr uint32_t MaxRowSlots = 4194304u; // HLSL GPU_SCENE_MAX_ROW_SLOTS
    static constexpr uint32_t MaxViews = 32u;         // HLSL GPU_SCENE_MAX_VIEWS

    // Build the per-flight buffers and the three push constants for one view
    // set. Fails closed on any capacity/snapshot/view error. The caller owns
    // the three compute dispatches and the inter-stage barriers.
    //
    // dense           BuildDenseLeaves output (dense Row<Leaf>[] in ticket order)
    // views           CPU frustums, packed to LeafClassifyView internally
    // ownerGenerations  per-instance-slot admitted generation (size >= instances)
    [[nodiscard]] bool Prepare(smvk::ResourceManager& ctx,
                               uint32_t flight,
                               const RenderDatabase& db,
                               const Addresses& addresses,
                               const DenseLeaves& dense,
                               std::span<const RelativeFrustum> views,
                               std::span<const uint32_t> ownerGenerations,
                               LeafClassifyPush& classify,
                               LeafCompactPush& compact,
                               FusedCullPush& fused,
                               bool validate = false) {
        if (flight >= Flights || dense.count == 0 || views.empty() || views.size() > MaxViews ||
            dense.count > MaxRowSlots || db.instances.Size() > MaxRowSlots ||
            db.leafMembers.Size() > MaxRowSlots || db.meshes.Size() > MaxRowSlots ||
            !addresses.leafMembers || !addresses.instances || !addresses.meshes ||
            ownerGenerations.size() < db.instances.Size())
            return false;
        auto& f = flight_[flight];
        if (f.pending)
            return false;

        // Pack the CPU frustums first; a non-finite view fails closed.
        packedViews_.assign(views.size(), LeafClassifyView{});
        for (size_t i = 0; i < views.size(); ++i)
            if (!PackLeafClassifyView(views[i], packedViews_[i]))
                return false;

        f.prepared = false;
        f.validate = validate;
        f.leafCount = dense.count;
        f.viewCount = static_cast<uint32_t>(views.size());
        f.instanceSlots = db.instances.Size();

        if (!Ensure(ctx,
                    f.denseLeaves,
                    f.denseLeavesSize,
                    size_t(dense.count) * sizeof(Row<Leaf>),
                    "gpu scene dense leaves") ||
            !Ensure(ctx,
                    f.views,
                    f.viewsSize,
                    packedViews_.size() * sizeof(LeafClassifyView),
                    "gpu scene leaf views") ||
            !Ensure(ctx,
                    f.masks,
                    f.masksSize,
                    size_t(dense.count) * sizeof(uint32_t),
                    "gpu scene leaf masks") ||
            !Ensure(ctx,
                    f.queue,
                    f.queueSize,
                    size_t(dense.count) * sizeof(LeafWorkEntry),
                    "gpu scene leaf queue") ||
            !Ensure(ctx,
                    f.verdicts,
                    f.verdictsSize,
                    size_t(db.instances.Size()) * sizeof(FusedVerdict),
                    "gpu scene fused verdicts") ||
            !Ensure(ctx,
                    f.ownerGenerations,
                    f.ownerGenerationsSize,
                    ownerGenerations.size() * sizeof(uint32_t),
                    "gpu scene owner generations"))
            return false;
        if (f.counters.empty()) {
            f.counters = ctx.CreateBuffer(
                sizeof(LeafCounters), smvk::BufferType_Storage, "gpu scene leaf counters");
            if (f.counters.empty())
                return false;
        }

        const uint64_t leaves = ctx.GetBufferDeviceAddress(f.denseLeaves);
        const uint64_t viewsBda = ctx.GetBufferDeviceAddress(f.views);
        const uint64_t masks = ctx.GetBufferDeviceAddress(f.masks);
        const uint64_t queue = ctx.GetBufferDeviceAddress(f.queue);
        const uint64_t counters = ctx.GetBufferDeviceAddress(f.counters);
        const uint64_t verdicts = ctx.GetBufferDeviceAddress(f.verdicts);
        const uint64_t owner = ctx.GetBufferDeviceAddress(f.ownerGenerations);
        if (!leaves || !viewsBda || !masks || !queue || !counters || !verdicts || !owner)
            return false;

        ctx.UploadDataToBuffer(f.denseLeaves,
                               dense.rows.data(),
                               size_t(dense.count) * sizeof(Row<Leaf>),
                               smvk::PipelineStageBit_Compute,
                               smvk::AccessFlags::ShaderRead);
        ctx.UploadDataToBuffer(f.views,
                               packedViews_.data(),
                               packedViews_.size() * sizeof(LeafClassifyView),
                               smvk::PipelineStageBit_Compute,
                               smvk::AccessFlags::ShaderRead);
        ctx.UploadDataToBuffer(f.ownerGenerations,
                               ownerGenerations.data(),
                               ownerGenerations.size() * sizeof(uint32_t),
                               smvk::PipelineStageBit_Compute,
                               smvk::AccessFlags::ShaderRead);

        // The compact shader compacts active leaves to the FRONT of the queue
        // but never clears the tail; stale tail entries must not leak into the
        // fused pass. Zero the whole queue every frame (conservative, cheap —
        // leafCount is hundreds to thousands, never per-instance).
        const size_t queueBytes = size_t(dense.count) * sizeof(LeafWorkEntry);
        if (zeroScratch_.size() < queueBytes)
            zeroScratch_.resize(queueBytes);
        std::fill(zeroScratch_.begin(), zeroScratch_.begin() + queueBytes, 0u);
        ctx.UploadDataToBuffer(f.queue,
                               zeroScratch_.data(),
                               queueBytes,
                               smvk::PipelineStageBit_Compute,
                               smvk::AccessFlags::ShaderRead);

        // The fused cull writes a verdict only for visible instances, so a
        // slot culled this frame would otherwise leak its previous frame's
        // verdict into the readback. Clear the whole verdict buffer too.
        const size_t verdictBytes = size_t(db.instances.Size()) * sizeof(FusedVerdict);
        if (zeroScratch_.size() < verdictBytes)
            zeroScratch_.resize(verdictBytes);
        std::fill(zeroScratch_.begin(), zeroScratch_.begin() + verdictBytes, 0u);
        ctx.UploadDataToBuffer(f.verdicts,
                               zeroScratch_.data(),
                               verdictBytes,
                               smvk::PipelineStageBit_Compute,
                               smvk::AccessFlags::ShaderRead);

        classify = {};
        classify.leaves = leaves;
        classify.views = viewsBda;
        classify.masks = masks;
        classify.counts = glm::uvec4(f.leafCount, f.viewCount, 0u, 0u);

        compact = {};
        compact.leaves = leaves;
        compact.masks = masks;
        compact.queue = queue;
        compact.counters = counters;
        compact.counts = glm::uvec4(f.leafCount, 0u, 1u, 0u); // z=1 resets the counter

        fused = {};
        fused.queue = queue;
        fused.members = addresses.leafMembers;
        fused.instances = addresses.instances;
        fused.ownerGenerations = owner;
        fused.views = viewsBda;
        fused.verdicts = verdicts;
        fused.meshes = addresses.meshes;
        // Conservative active-leaf bound: the fused pass walks every queue slot
        // and skips zero masks. dispatch-indirect on the counter is the
        // follow-up optimization once the counter path is validated.
        fused.counts = glm::uvec4(f.leafCount, f.viewCount, db.meshes.Size(), 0u);

        if (validate) {
            f.expected = ReferenceVisibility(db, dense, views, ownerGenerations);
        } else {
            f.expected = {};
        }
        f.prepared = true;
        return true;
    }

    // Test-mode readback + comparison. MUST run after the flight's acquire
    // fence and BEFORE that flight's buffers are reused.
    void BeginFlight(smvk::ResourceManager& ctx, uint32_t flight) {
        if (flight >= Flights)
            return;
        auto& f = flight_[flight];
        f.prepared = false;
        if (!f.pending)
            return;
        f.pending = false;
        if (!f.validate || f.expected.verdicts.empty())
            return;

        LeafCounters counters{};
        if (!ctx.DownloadDataFromBuffer(f.counters, &counters, sizeof(counters))) {
            std::fprintf(stderr, "[GpuSceneVisibility] counters readback failed\n");
            f.prepared = false;
            return;
        }
        if (counters.activeLeafCount != f.expected.activeLeafCount) {
            std::fprintf(stderr,
                         "[GpuSceneVisibility] active leaves gpu=%u cpu=%u\n",
                         counters.activeLeafCount,
                         f.expected.activeLeafCount);
        }

        std::vector<uint32_t> masks(f.leafCount);
        if (!ctx.DownloadDataFromBuffer(f.masks, masks.data(), masks.size() * sizeof(uint32_t))) {
            std::fprintf(stderr, "[GpuSceneVisibility] mask readback failed\n");
            f.prepared = false;
            return;
        }
        uint64_t maskMismatch = 0;
        for (uint32_t i = 0; i < f.leafCount; ++i) {
            if (masks[i] != f.expected.leafMasks[i]) {
                ++maskMismatch;
                if (maskMismatch <= 4)
                    std::fprintf(stderr,
                                 "[GpuSceneVisibility] mask mismatch leaf=%u gpu=%u cpu=%u\n",
                                 i,
                                 masks[i],
                                 f.expected.leafMasks[i]);
            }
        }

        std::vector<FusedVerdict> verdicts(f.instanceSlots);
        if (!ctx.DownloadDataFromBuffer(
                f.verdicts, verdicts.data(), verdicts.size() * sizeof(FusedVerdict))) {
            std::fprintf(stderr, "[GpuSceneVisibility] verdict readback failed\n");
            f.prepared = false;
            return;
        }
        uint64_t verdictMismatch = 0;
        for (uint32_t i = 0; i < f.instanceSlots; ++i) {
            const auto& a = verdicts[i].value;
            const auto& b = f.expected.verdicts[i].value;
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
                ++verdictMismatch;
                if (verdictMismatch <= 4)
                    std::fprintf(stderr,
                                 "[GpuSceneVisibility] verdict mismatch slot=%u "
                                 "gpu=(%u,%u,%u,%u) cpu=(%u,%u,%u,%u)\n",
                                 i,
                                 a.x,
                                 a.y,
                                 a.z,
                                 a.w,
                                 b.x,
                                 b.y,
                                 b.z,
                                 b.w);
            }
        }

        std::fprintf(stderr,
                     "[GpuSceneVisibility] leaves=%u active=%u maskMismatch=%llu "
                     "verdictMismatch=%llu\n",
                     f.leafCount,
                     f.expected.activeLeafCount,
                     (unsigned long long)maskMismatch,
                     (unsigned long long)verdictMismatch);
        f.prepared = false;
    }

    // Register the GPU-written outputs as render-graph compute resources; BDA
    // accesses are otherwise invisible to dependency tracking.
    [[nodiscard]] smvk::BufferHandle QueueBuffer(uint32_t flight) const {
        return flight < Flights ? smvk::BufferHandle(flight_[flight].queue) : smvk::BufferHandle{};
    }
    [[nodiscard]] smvk::BufferHandle CountersBuffer(uint32_t flight) const {
        return flight < Flights ? smvk::BufferHandle(flight_[flight].counters)
                                : smvk::BufferHandle{};
    }
    [[nodiscard]] smvk::BufferHandle VerdictBuffer(uint32_t flight) const {
        return flight < Flights ? smvk::BufferHandle(flight_[flight].verdicts)
                                : smvk::BufferHandle{};
    }

    // Device address of the fused verdict stream (the expand stage's input).
    [[nodiscard]] uint64_t FusedVerdictAddress(const smvk::ResourceManager& ctx,
                                               uint32_t flight) const {
        return flight < Flights && !flight_[flight].verdicts.empty()
                   ? ctx.GetBufferDeviceAddress(flight_[flight].verdicts)
                   : 0;
    }

    void MarkDispatched(uint32_t flight) {
        if (flight >= Flights)
            return;
        auto& f = flight_[flight];
        if (f.prepared) {
            f.pending = f.validate;
            f.prepared = false;
        }
    }

  private:
    struct Flight {
        smvk::Holder<smvk::BufferHandle> denseLeaves{}, views{}, masks{}, queue{};
        smvk::Holder<smvk::BufferHandle> counters{}, verdicts{}, ownerGenerations{};
        size_t denseLeavesSize = 0, viewsSize = 0, masksSize = 0, queueSize = 0;
        size_t verdictsSize = 0, ownerGenerationsSize = 0;
        uint32_t leafCount = 0, viewCount = 0, instanceSlots = 0;
        bool validate = false, pending = false, prepared = false;
        VisibilityReference expected{};
    };

    static bool Ensure(smvk::ResourceManager& ctx,
                       smvk::Holder<smvk::BufferHandle>& holder,
                       size_t& capacity,
                       size_t minimum,
                       const char* label) {
        if (!holder.empty() && capacity >= minimum)
            return true;
        size_t n = std::max<size_t>(256, capacity);
        while (n < minimum) {
            if (n > std::numeric_limits<size_t>::max() / 2)
                return false;
            n *= 2;
        }
        auto fresh = ctx.CreateBuffer(n, smvk::BufferType_Storage, label);
        if (fresh.empty())
            return false;
        holder = std::move(fresh);
        capacity = n;
        return true;
    }

    std::array<Flight, Flights> flight_{};
    std::vector<LeafClassifyView> packedViews_{};
    std::vector<uint8_t> zeroScratch_{};
};

} // namespace tour::gpu_scene
