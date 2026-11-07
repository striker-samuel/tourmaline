// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Environment residency provider for 64m WorldPartition subcells.
//
// A volume's authored geometry and falloff remain exact, shared and
// immutable between edits. Only cell-to-volume candidate index lists
// stream. The renderer publishes a snapshot after Commit/Retire, never
// pointers into an in-flight worker's storage.
//
// Two sources are supported:
//  * Editor/loose scenes: read immutable candidate spans from WorldEnvTiles.
//  * Cooked/paged worlds: SetRequestCallback dispatches reads; Activate
//    adopts a validated page using a generation ticket on the render thread.
// Stale completions after eviction, teleport, or scene revision are rejected.
#include <Engine/WorldPartition/WorldEnvTiles.h>
#include <Engine/WorldPartition/WorldPartition.h>
#include <Engine/WorldPartition/StreamTicketLedger.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace engine::worldpartition {

class WorldEnvTileProvider final : public IWorldStreamProvider {
public:
    using RequestCallback = std::function<void(WorldSubcellKey, uint64_t ticket)>;

    const char* ProviderName() const override { return "WorldEnvTiles"; }

    void SetWorld(WorldId world) {
        if (world == world_) return;
        world_ = world;
        Invalidate();
    }
    void SetMaxNewRequestsPerFrame(size_t cap) { maxNewRequestsPerFrame_ = cap; }
    void SetMaxResidentCells(size_t cap) { maxResidentCells_ = cap; }
    void SetRequestCallback(RequestCallback callback) { request_ = std::move(callback); }

    // Source holds global exact zone/shape records plus the immutable cell
    // candidate index. A new bake revision invalidates pending tickets.
    void SetSource(const WorldEnvTiles* source, uint64_t revision) {
        if (source == source_ && revision == sourceRevision_) return;
        source_ = source;
        sourceRevision_ = revision;
        Invalidate();
    }

    void OnStreamingInterests(const StreamingInterest* interests, size_t count) override {
        const size_t n = source_ && source_->Valid() ? source_->CellCount() : 0;
        desired_.assign(n, 0);
        priority_.assign(n, -std::numeric_limits<float>::infinity());
        if (!n || !interests) return;
        const glm::vec2 origin = source_->GridMin();
        const double tile = double(kEnvTileSize);
        for (size_t i = 0; i < count; ++i) {
            const StreamingInterest& interest = interests[i];
            if (interest.world != world_ ||
                !(interest.layerMask & StreamLayerBit(StreamLayer::Environment)) ||
                !std::isfinite(interest.position.x) || !std::isfinite(interest.position.z) ||
                !std::isfinite(interest.radiusMeters) || interest.radiusMeters < 0.0 ||
                !std::isfinite(interest.priority))
                continue;
            const double x = interest.position.x, z = interest.position.z;
            const double radius = interest.radiusMeters + tile; // prefetch margin
            // Clamp in floating point before integer conversion: teleports
            // to huge coordinates cannot overflow signed int casts.
            const auto clampIndex = [](double v, int bound) -> int {
                return int(std::clamp(v, 0.0, double(bound)));
            };
            const int loX = clampIndex(std::floor((x - radius - origin.x) / tile), int(source_->DimX()));
            const int hiX = clampIndex(std::ceil((x + radius - origin.x) / tile), int(source_->DimX()));
            const int loZ = clampIndex(std::floor((z - radius - origin.y) / tile), int(source_->DimZ()));
            const int hiZ = clampIndex(std::ceil((z + radius - origin.y) / tile), int(source_->DimZ()));
            for (int iz = loZ; iz < hiZ; ++iz)
                for (int ix = loX; ix < hiX; ++ix) {
                    const double minX = double(origin.x) + double(ix) * tile;
                    const double minZ = double(origin.y) + double(iz) * tile;
                    const double dx = std::max({minX - x, 0.0, x - (minX + tile)});
                    const double dz = std::max({minZ - z, 0.0, z - (minZ + tile)});
                    const double d2 = dx * dx + dz * dz;
                    if (d2 > radius * radius) continue;
                    const size_t idx = size_t(iz) * source_->DimX() + size_t(ix);
                    desired_[idx] = 1;
                    // Preserve the priority-first contract while allowing
                    // nearer requests to break otherwise equal priorities.
                    priority_[idx] = std::max(priority_[idx],
                        interest.priority - float(std::sqrt(d2) / (radius + 1.0)) * 1e-3f);
                }
        }
    }

    size_t GatherRequests(StreamRequestSink* sink, uint64_t frameIndex) override {
        if (!sink || !source_ || !source_->Valid()) return 0;
        size_t count = 0;
        for (size_t i = 0; i < desired_.size(); ++i) {
            if (!desired_[i]) continue;
            const uint32_t ix = uint32_t(i % source_->DimX());
            const uint32_t iz = uint32_t(i / source_->DimX());
            const WorldSubcellKey subcell = Key(ix, iz);
            StreamRequest request{};
            request.world = world_;
            request.layer = StreamLayer::Environment;
            request.cell = SubcellToCell(subcell);
            request.subcell = subcell;
            request.desired = DesiredResidency::GpuReady;
            request.priority = priority_[i];
            request.frameRequested = frameIndex;
            sink->Push(request);
            ++count;
        }
        return count;
    }

    // Called at the renderer's publication point, never on a worker.
    // Pages are accepted only for outstanding tickets from this source.
    bool Activate(WorldSubcellKey subcell, uint64_t ticket,
                  std::vector<uint32_t> candidates) {
        const size_t i = Index(subcell);
        if (i >= desired_.size() || !desired_[i]) return false;
        if (!tickets_.Matches(i, ticket)) return false;
        for (uint32_t candidate : candidates)
            if (candidate >= source_->RecordCount()) return false;
        tickets_.Accept(i, ticket);
        resident_[i] = std::move(candidates);
        PublishSnapshot();
        return true;
    }

    bool Fail(WorldSubcellKey subcell, uint64_t ticket) {
        const size_t i = Index(subcell);
        return tickets_.Fail(i, ticket, lastFrame_, 120); // corrupt page backoff
    }

    void CommitFrame(uint64_t frame) override {
        lastFrame_ = frame;
        if (!source_ || !source_->Valid()) return;
        bool changed = false;
        // Retire first, including canceled loads, before dispatching more.
        for (auto it = resident_.begin(); it != resident_.end();) {
            if (it->first < desired_.size() && desired_[it->first])
                ++it;
            else {
                it = resident_.erase(it);
                changed = true;
            }
        }
        tickets_.CancelUndesired([&](size_t i) {
            return i < desired_.size() && desired_[i];
        });

        std::vector<size_t> missing;
        for (size_t i = 0; i < desired_.size(); ++i)
            if (desired_[i] && !resident_.count(i) && tickets_.CanIssue(i, frame))
                missing.push_back(i);
        std::stable_sort(missing.begin(), missing.end(), [&](size_t a, size_t b) {
            if (priority_[a] != priority_[b]) return priority_[a] > priority_[b];
            return a < b;
        });
        const size_t remaining = resident_.size() + tickets_.PendingCount() >= maxResidentCells_
            ? 0 : maxResidentCells_ - resident_.size() - tickets_.PendingCount();
        const size_t toIssue = std::min({maxNewRequestsPerFrame_, remaining, missing.size()});
        for (size_t k = 0; k < toIssue; ++k) {
            const size_t i = missing[k];
            const uint32_t ix = uint32_t(i % source_->DimX());
            const uint32_t iz = uint32_t(i / source_->DimX());
            const WorldSubcellKey subcell = Key(ix, iz);
            if (request_) {
                const auto ticket = tickets_.Issue(i, frame);
                if (!ticket) break; // no ticket wraparound
                request_(subcell, *ticket);
            } else {
                // Scene-backed synchronous fallback. This never touches pak
                // IO and costs only a copy of the already-baked indices.
                const EnvTileCell& cell = source_->Cells()[i];
                const uint32_t* src = source_->CandidateIndices();
                std::vector<uint32_t> candidates;
                if (cell.candidateCount)
                    candidates.assign(src + cell.firstCandidate,
                        src + cell.firstCandidate + cell.candidateCount);
                resident_.emplace(i, std::move(candidates));
                changed = true;
            }
        }
        if (changed || unpublished_) PublishSnapshot();
    }

    void RetireFrame(uint64_t) override {
        // CommitFrame already drops no-longer-requested pages before GPU
        // publication. ResourceManager holds old GPU buffers until the
        // deferred in-flight frame retirement fence.
    }

    // CPU-readable logical grid for tests/editor, and compact physical
    // 4x4 root-cell pages for GPU. UINT32_MAX in page table = base.
    const EnvTileCell* Cells() const { return cells_.data(); }
    size_t CellCount() const { return cells_.size(); }
    const EnvTileCell* PackedCells() const { return packedCells_.data(); }
    size_t PackedCellCount() const { return packedCells_.size(); }
    const uint32_t* PageTable() const { return pageTable_.data(); }
    size_t PageTableCount() const { return pageTable_.size(); }
    const uint32_t* Indices() const { return indices_.data(); }
    size_t IndexCount() const { return indices_.size(); }
    uint64_t Revision() const { return revision_; }
    size_t ResidentCount() const { return resident_.size(); }
    size_t PendingCount() const { return tickets_.PendingCount(); }

private:
    WorldSubcellKey Key(uint32_t ix, uint32_t iz) const {
        const glm::vec2 o = source_->GridMin();
        const glm::dvec3 p(double(o.x) + (double(ix) + 0.5) * kEnvTileSize,
                            0.0, double(o.y) + (double(iz) + 0.5) * kEnvTileSize);
        return GetWorldSubcell(p);
    }
    size_t Index(WorldSubcellKey subcell) const {
        if (!source_ || !source_->Valid()) return std::numeric_limits<size_t>::max();
        const glm::vec2 o = source_->GridMin();
        const int64_t ox = int64_t(std::floor(double(o.x) / kEnvTileSize));
        const int64_t oz = int64_t(std::floor(double(o.y) / kEnvTileSize));
        const int64_t ix = subcell.x - ox, iz = subcell.z - oz;
        if (ix < 0 || iz < 0 || ix >= source_->DimX() || iz >= source_->DimZ())
            return std::numeric_limits<size_t>::max();
        return size_t(iz) * source_->DimX() + size_t(ix);
    }
    void PublishSnapshot() {
        if (!source_ || !source_->Valid()) {
            cells_.clear(); indices_.clear();
            packedCells_.clear(); pageTable_.clear();
        } else {
            cells_.assign(source_->CellCount(), EnvTileCell{});
            indices_.clear();
            for (const auto& [i, page] : resident_) {
                EnvTileCell& cell = cells_[i];
                cell.firstCandidate = uint32_t(indices_.size());
                cell.candidateCount = uint32_t(page.size());
                indices_.insert(indices_.end(), page.begin(), page.end());
            }
            // Compact physical pages of 4x4 64m cells; each root-cell
            // page costs 512B. The table itself is <= 64x64 uints.
            const uint32_t dimX = source_->DimX(), dimZ = source_->DimZ();
            const uint32_t rootX = (dimX + 3u) / 4u;
            const uint32_t rootZ = (dimZ + 3u) / 4u;
            pageTable_.assign(size_t(rootX)*rootZ, UINT32_MAX);
            packedCells_.clear();
            for (uint32_t rz = 0; rz < rootZ; ++rz)
                for (uint32_t rx = 0; rx < rootX; ++rx) {
                    bool live = false;
                    for (uint32_t z = rz*4; z < std::min(rz*4+4,dimZ); ++z)
                        for (uint32_t x = rx*4; x < std::min(rx*4+4,dimX); ++x)
                            live |= resident_.count(size_t(z)*dimX+x) != 0;
                    if (!live) continue;
                    pageTable_[size_t(rz)*rootX+rx] =
                        uint32_t(packedCells_.size()/16);
                    for (uint32_t z=0; z<4; ++z)
                        for (uint32_t x=0; x<4; ++x) {
                            const uint32_t gx=rx*4+x, gz=rz*4+z;
                            packedCells_.push_back((gx < dimX && gz < dimZ)
                                ? cells_[size_t(gz)*dimX+gx] : EnvTileCell{});
                        }
                }
        }
        ++revision_;
        unpublished_ = false;
    }
    void Invalidate() {
        resident_.clear();
        tickets_.Clear();
        desired_.clear();
        priority_.clear();
        cells_.clear();
        indices_.clear();
        packedCells_.clear();
        pageTable_.clear();
        unpublished_ = true;
        ++revision_;
    }
    WorldId world_ = 0;
    const WorldEnvTiles* source_ = nullptr;
    uint64_t sourceRevision_ = 0;
    uint64_t revision_ = 0, lastFrame_ = 0;
    size_t maxNewRequestsPerFrame_ = 64;
    size_t maxResidentCells_ = 4096;
    bool unpublished_ = true;
    RequestCallback request_;
    std::vector<uint8_t> desired_;
    std::vector<float> priority_;
    std::map<size_t, std::vector<uint32_t>> resident_;
    StreamTicketLedger<size_t> tickets_;
    std::vector<EnvTileCell> cells_; // logical grid, CPU only
    std::vector<EnvTileCell> packedCells_; // physical resident 4x4 root pages
    std::vector<uint32_t> pageTable_; // root coords to physical slot
    std::vector<uint32_t> indices_;
};

} // namespace engine::worldpartition
