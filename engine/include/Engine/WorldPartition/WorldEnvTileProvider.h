// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// WorldEnvTileProvider: WorldPartition-managed residency for exact static
// environment volumes. The CPU bake is immutable between scene edits.
// The provider selects 64m subcells from the shared interest stream and
// packs only their candidate references for GPU publication.
#include <Engine/WorldPartition/WorldEnvTiles.h>
#include <Engine/WorldPartition/WorldPartition.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace engine::worldpartition {

class WorldEnvTileProvider final : public IWorldStreamProvider {
public:
    const char* ProviderName() const override { return "WorldEnvTiles"; }

    // Changing the authored baked source forces a fresh publication even
    // if the resident cell set remains unchanged.
    void SetSource(const WorldEnvTiles* source, uint64_t revision) {
        if (source_ == source && sourceRevision_ == revision) return;
        source_ = source;
        sourceRevision_ = revision;
        sourceDirty_ = true;
    }

    void OnStreamingInterests(const StreamingInterest* interests, size_t count) override {
        desired_.clear();
        if (!source_ || !source_->Valid()) return;
        for (size_t i = 0; i < count; ++i)
            if ((interests[i].layerMask & StreamLayerBit(StreamLayer::Environment)) != 0u)
                desired_.push_back(interests[i]);
        desiredMask_.assign(source_->CellCount(), uint8_t(0));
        const glm::vec2 gridMin = source_->GridMin();
        const double tile = double(kEnvTileSize);
        // Conservative tile/sphere test. One extra subcell of padding
        // prevents a newly-entered cell from being unresident.
        for (const StreamingInterest& interest : desired_) {
            const double radius = std::max(0.0, interest.radiusMeters) + tile;
            const double x = interest.position.x, z = interest.position.z;
            const int loX = std::max(0, int(std::floor((x-radius-gridMin.x)/tile)));
            const int hiX = std::min(int(source_->DimX()), int(std::ceil((x+radius-gridMin.x)/tile)));
            const int loZ = std::max(0, int(std::floor((z-radius-gridMin.y)/tile)));
            const int hiZ = std::min(int(source_->DimZ()), int(std::ceil((z+radius-gridMin.y)/tile)));
            for (int iz = loZ; iz < hiZ; ++iz)
                for (int ix = loX; ix < hiX; ++ix) {
                    const double minX = double(gridMin.x) + double(ix)*tile;
                    const double minZ = double(gridMin.y) + double(iz)*tile;
                    const double dx = std::max({minX-x, 0.0, x-(minX+tile)});
                    const double dz = std::max({minZ-z, 0.0, z-(minZ+tile)});
                    if (dx*dx + dz*dz <= radius*radius)
                        desiredMask_[size_t(iz)*source_->DimX()+size_t(ix)] = 1;
                }
        }
    }

    size_t GatherRequests(StreamRequestSink* sink, uint64_t frameIndex) override {
        if (!sink || !source_ || !source_->Valid()) return 0;
        size_t count = 0;
        const glm::vec2 gridMin = source_->GridMin();
        for (uint32_t iz = 0; iz < source_->DimZ(); ++iz)
            for (uint32_t ix = 0; ix < source_->DimX(); ++ix) {
                if (!desiredMask_[size_t(iz)*source_->DimX()+ix]) continue;
                const glm::dvec3 center(double(gridMin.x)+(double(ix)+0.5)*kEnvTileSize,
                                         0, double(gridMin.y)+(double(iz)+0.5)*kEnvTileSize);
                StreamRequest request{};
                request.world = desired_.empty() ? 0 : desired_[0].world;
                request.layer = StreamLayer::Environment;
                request.cell = GetWorldCell(center);
                request.subcell = GetWorldSubcell(center);
                request.desired = DesiredResidency::GpuReady;
                request.priority = 1.0f;
                request.frameRequested = frameIndex;
                sink->Push(request);
                ++count;
            }
        return count;
    }

    void CommitFrame(uint64_t) override {
        if (!source_ || !source_->Valid()) return;
        if (!sourceDirty_ && desiredMask_ == residentMask_) return;
        residentMask_ = desiredMask_;
        cells_.assign(source_->CellCount(), EnvTileCell{});
        indices_.clear();
        const EnvTileCell* baked = source_->Cells();
        const uint32_t* allIndices = source_->CandidateIndices();
        for (size_t i = 0; i < cells_.size(); ++i) {
            if (!residentMask_[i]) continue;
            const EnvTileCell& original = baked[i];
            EnvTileCell& selected = cells_[i];
            selected.firstCandidate = uint32_t(indices_.size());
            selected.candidateCount = original.candidateCount;
            indices_.insert(indices_.end(), allIndices + original.firstCandidate,
                allIndices + original.firstCandidate + original.candidateCount);
        }
        sourceDirty_ = false;
        ++revision_;
    }

    void RetireFrame(uint64_t) override {
        // Unrequested cells were replaced by zero-candidate cells during
        // CommitFrame. GPU buffer retirement is owned by WorldRenderer.
    }

    const EnvTileCell* Cells() const { return cells_.data(); }
    size_t CellCount() const { return cells_.size(); }
    const uint32_t* Indices() const { return indices_.data(); }
    size_t IndexCount() const { return indices_.size(); }
    uint64_t Revision() const { return revision_; }
    size_t ResidentCount() const {
        return size_t(std::count(residentMask_.begin(), residentMask_.end(), uint8_t(1)));
    }

private:
    const WorldEnvTiles* source_ = nullptr;
    uint64_t sourceRevision_ = 0;
    uint64_t revision_ = 0;
    bool sourceDirty_ = false;
    std::vector<StreamingInterest> desired_;
    std::vector<uint8_t> desiredMask_, residentMask_;
    std::vector<EnvTileCell> cells_;
    std::vector<uint32_t> indices_;
};

} // namespace engine::worldpartition
