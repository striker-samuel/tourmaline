// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPUIR-style spatial leaf membership. CPU owns the leaf topology; GPU
// receives a persistent Leaf table and contiguous generational instance refs.
// No view-frustum test or GPU culling is performed in this phase.
#include <Engine/Renderer/gpu_scene_upload.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace tour::gpu_scene {

struct LeafCell {
    int64_t x = 0;
    int64_t z = 0;
    bool operator==(const LeafCell&) const = default;
    auto operator<=>(const LeafCell&) const = default;
};

struct LeafTicket {
    LeafCell cell{};
    Handle<Leaf> leaf{};
    uint32_t firstMember = InvalidIndex;
    uint32_t memberCount = 0;
    bool boundsComplete = false;
};

// 64m XZ subdivisions match Tourmaline's documented world partition
// subcells. A membership cell is chosen using instance TRANSLATION, but
// the published leaf bound expands to cover whole transformed meshes;
// crossing the tile border is not a visibility error.
inline constexpr double kLeafCellMeters = 64.0;

class LeafRegistry {
public:
    using InstanceId = Handle<Instance>;

    // On a changed instance, update only its old/new leaf. IDs contain
    // the generation: reusing a GPU slot cannot inherit stale membership.
    void Upsert(InstanceId id, const RenderDatabase& db) {
        const Instance* instance = db.instances.Get(id);
        if (!instance) { Remove(id); return; }
        const LeafCell next = CellFor(*instance);
        const uint64_t key = Pack(id);
        if (auto it = membership_.find(key); it != membership_.end()) {
            if (it->second == next) {
                dirty_.insert(next); // bounds may have moved inside cell
                return;
            }
            EraseFromCell(it->second, id);
            membership_.erase(it);
        }
        LeafState& state = leaves_[next];
        auto pos = std::lower_bound(state.members.begin(), state.members.end(),
                                    id, Less);
        if (pos == state.members.end() || *pos != id)
            state.members.insert(pos, id);
        membership_.emplace(key, next);
        dirty_.insert(next);
    }

    void Remove(InstanceId id) {
        const auto it = membership_.find(Pack(id));
        if (it == membership_.end()) return;
        EraseFromCell(it->second, id);
        membership_.erase(it);
    }

    // A geometry residency/LOD metadata change can change whether a leaf
    // is cull-safe; mesh metadata does NOT have to move its instances.
    void RefreshSafety() {
        for (const auto& [cell, state] : leaves_)
            dirty_.insert(cell);
    }

    // This runs after WorldSynchronizer's changed-instance journal and
    // before the standard per-flight Uploader. Failed allocations leave
    // their dirty leaf scheduled for retry on the next call.
    [[nodiscard]] bool Flush(RenderDatabase& db) {
        while (!dirty_.empty()) {
            const LeafCell cell = *dirty_.begin();
            auto it = leaves_.find(cell);
            if (it == leaves_.end()) {
                dirty_.erase(dirty_.begin());
                continue;
            }
            if (!PublishCell(cell, it->second, db))
                return false;
            if (it->second.members.empty())
                leaves_.erase(it);
            dirty_.erase(dirty_.begin());
        }
        return true;
    }

    void Clear(RenderDatabase& db) {
        for (auto& [cell, state] : leaves_) {
            for (auto member : state.memberRows)
                (void)db.leafMembers.Erase(member);
            if (state.leaf)
                (void)db.leaves.Erase(state.leaf);
        }
        leaves_.clear();
        membership_.clear();
        dirty_.clear();
    }

    [[nodiscard]] size_t LeafCount() const noexcept { return leaves_.size(); }
    [[nodiscard]] size_t PendingLeafCount() const noexcept { return dirty_.size(); }
    [[nodiscard]] std::vector<LeafTicket> Tickets(const RenderDatabase& db) const {
        std::vector<LeafTicket> result;
        result.reserve(leaves_.size());
        for (const auto& [cell, state] : leaves_) {
            const Leaf* row = db.leaves.Get(state.leaf);
            if (!row || row->range.y == 0) continue;
            result.push_back({cell, state.leaf, row->range.x, row->range.y,
                (row->range.z & LeafFlagBoundsComplete) != 0});
        }
        return result;
    }

    [[nodiscard]] static LeafCell CellFor(const Instance& instance) {
        const glm::dvec3 absolute = math::JoinDoubleFloat(instance.worldPosition);
        const double x = std::floor(absolute.x / kLeafCellMeters);
        const double z = std::floor(absolute.z / kLeafCellMeters);
        // Never invoke undefined double->int64 conversion on NaNs, infinities
        // or coordinates outside the 64-bit tile grid. Overflow objects
        // enter a non-cullable sentinel leaf, rather than disappearing.
        constexpr double safe = 9.0e18;
        if (!std::isfinite(x) || !std::isfinite(z) ||
            x <= -safe || x >= safe || z <= -safe || z >= safe)
            return {INT64_MIN, INT64_MIN};
        return {int64_t(x), int64_t(z)};
    }

private:
    struct LeafState {
        Handle<Leaf> leaf{};
        std::vector<InstanceId> members; // sorted by (slot,generation)
        std::vector<Handle<LeafMember>> memberRows;
    };

    static uint64_t Pack(InstanceId id) {
        return (uint64_t(id.slot) << 32) | id.generation;
    }
    static bool Less(InstanceId a, InstanceId b) {
        return Pack(a) < Pack(b);
    }
    void EraseFromCell(LeafCell cell, InstanceId id) {
        auto it = leaves_.find(cell);
        if (it == leaves_.end()) return;
        auto& members = it->second.members;
        auto pos = std::lower_bound(members.begin(), members.end(), id, Less);
        if (pos != members.end() && *pos == id) {
            members.erase(pos);
            dirty_.insert(cell);
        }
    }

    // Conservative transform of a local sphere. Frobenius norm bounds
    // arbitrary shear as well as rotation/nonuniform scale.
    static bool WorldSphere(const Instance& instance, glm::dvec3& center,
                            double& radius) {
        const glm::dvec3 p = math::JoinDoubleFloat(instance.worldPosition);
        const glm::dvec3 bx(instance.basisX), by(instance.basisY),
                          bz(instance.basisZ);
        const glm::dvec3 local(instance.localSphere);
        center = p + bx * local.x + by * local.y + bz * local.z;
        const double f = std::sqrt(glm::dot(bx,bx) + glm::dot(by,by)
                                 + glm::dot(bz,bz));
        radius = f * double(instance.localSphere.w);
        return std::isfinite(center.x) && std::isfinite(center.y)
            && std::isfinite(center.z) && std::isfinite(radius)
            && radius >= 0.0;
    }

    [[nodiscard]] bool PublishCell(LeafCell cell, LeafState& state,
                                    RenderDatabase& db) {
        if (state.members.empty()) {
            for (auto row : state.memberRows) (void)db.leafMembers.Erase(row);
            state.memberRows.clear();
            if (state.leaf) (void)db.leaves.Erase(state.leaf);
            state.leaf = {};
            return true;
        }
        if (state.members.size() > UINT32_MAX) return false;
        const size_t count = state.members.size();
        // Always allocate NEW member rows before releasing an old range.
        // This preserves the old snapshot if allocation ever fails.
        if (state.memberRows.size() != count) {
            std::vector<LeafMember> values;
            values.reserve(count);
            for (InstanceId id : state.members)
                values.push_back({glm::uvec4(id.slot, id.generation, 0u, 0u)});
            auto fresh = db.leafMembers.InsertContiguous(values);
            if (fresh.size() != count) return false;
            for (auto old : state.memberRows) (void)db.leafMembers.Erase(old);
            state.memberRows = std::move(fresh);
        } else {
            for (size_t i = 0; i < count; ++i) {
                const InstanceId id = state.members[i];
                LeafMember value{glm::uvec4(id.slot, id.generation, 0u, 0u)};
                const LeafMember* existing = db.leafMembers.Get(state.memberRows[i]);
                if (!existing || existing->ref.x != value.ref.x ||
                    existing->ref.y != value.ref.y)
                    (void)db.leafMembers.Update(state.memberRows[i], value);
            }
        }

        // Incomplete source mesh bounds MUST disable future whole-leaf
        // frustum rejection. A false negative here would drop every member.
        bool complete = true;
        bool haveBounds = false;
        glm::dvec3 mn(0.0), mx(0.0);
        for (InstanceId id : state.members) {
            const Instance* inst = db.instances.Get(id);
            if (!inst || inst->refs.x == InvalidIndex ||
                inst->refs.x >= db.meshes.Rows().size()) {
                complete = false;
                continue;
            }
            const auto& mesh = db.meshes.Rows()[inst->refs.x];
            if (!mesh.identity.y || mesh.identity.x != inst->refs.y ||
                !(mesh.value.flags & MeshFlagCompleteBounds)) {
                complete = false;
                continue;
            }
            glm::dvec3 center(0.0);
            double radius = 0.0;
            if (!WorldSphere(*inst, center, radius)) {
                complete = false;
                continue;
            }
            const glm::dvec3 lo = center - glm::dvec3(radius);
            const glm::dvec3 hi = center + glm::dvec3(radius);
            if (!haveBounds) {
                mn = lo; mx = hi; haveBounds = true;
            } else {
                mn = glm::min(mn, lo);
                mx = glm::max(mx, hi);
            }
        }
        complete &= haveBounds;
        Leaf record{};
        if (haveBounds) {
            const glm::dvec3 center = mn + (mx - mn) * 0.5;
            const glm::dvec3 extents = mx - center;
            record.worldCenter = math::SplitDoubleFloat(center);
            const glm::dvec3 restored = math::JoinDoubleFloat(record.worldCenter);
            for (int axis = 0; axis < 3; ++axis) {
                // Round up + compensate center splitting; do not quietly
                // shrink a published bound at large absolute positions.
                const double margin = std::abs(restored[axis] - center[axis])
                                    + std::max(0.001, extents[axis] * 1e-6);
                const float inflated = float(extents[axis] + margin);
                if (!std::isfinite(inflated) || inflated < 0.0f) {
                    complete = false;
                    record.halfExtents[axis] = 0.0f;
                } else {
                    record.halfExtents[axis] =
                        std::nextafter(inflated, std::numeric_limits<float>::infinity());
                }
            }
        } else if (cell.x != INT64_MIN) {
            record.worldCenter = math::SplitDoubleFloat(glm::dvec3(
                (double(cell.x) + 0.5) * kLeafCellMeters, 0.0,
                (double(cell.z) + 0.5) * kLeafCellMeters));
        }
        record.range = glm::uvec4(state.memberRows.front().slot,
                                  uint32_t(count),
                                  complete ? LeafFlagBoundsComplete : 0u, 0u);
        if (!state.leaf) {
            state.leaf = db.leaves.Insert(record);
            return bool(state.leaf);
        }
        const Leaf* old = db.leaves.Get(state.leaf);
        if (!old) return false;
        // Compare only meaningful fields, avoiding padding byte reads.
        bool changed = false;
        for (int axis = 0; axis < 4; ++axis) {
            changed |= old->range[axis] != record.range[axis];
            changed |= old->worldCenter.high[axis] != record.worldCenter.high[axis];
            changed |= old->worldCenter.low[axis] != record.worldCenter.low[axis];
            changed |= old->halfExtents[axis] != record.halfExtents[axis];
        }
        if (changed) (void)db.leaves.Update(state.leaf, record);
        return true;
    }

    std::map<LeafCell, LeafState> leaves_;
    std::map<uint64_t, LeafCell> membership_;
    std::set<LeafCell> dirty_;
};

} // namespace tour::gpu_scene
