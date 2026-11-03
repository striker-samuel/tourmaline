// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Device-free GPU-scene database update model: stable identities,
// coalesced dirty-row ranges, tombstones, and all-flight propagation.
#include <Engine/Renderer/gpu_scene.h>
#include <Engine/Renderer/gpu_scene_upload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } \
    while (0)
namespace { int g_checks = 0; int g_failures = 0; }

namespace {
using namespace tour::gpu_scene;
constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
static_assert(Flights >= 2);

template <typename RowType>
struct FakeGpu {
    using Entry = Row<RowType>;
    std::array<std::vector<Entry>, Flights> mirror{};
    std::array<std::vector<std::pair<size_t, size_t>>, Flights> copies{};

    bool Upload(uint32_t f, size_t offset, const void* src, size_t bytes) {
        CHECK(offset % sizeof(Entry) == 0 && bytes % sizeof(Entry) == 0);
        auto& data = mirror[f];
        const size_t need = (offset + bytes) / sizeof(Entry);
        if (data.size() < need)
            data.resize(need);
        std::memcpy(reinterpret_cast<unsigned char*>(data.data()) + offset,
                    src, bytes);
        copies[f].emplace_back(offset, bytes);
        return true;
    }

    template <uint32_t N>
    bool Publish(Table<RowType, N>& table, uint32_t f, bool full = false) {
        return table.Publish(f, full, [&](size_t offset, const void* src,
                                          size_t bytes) {
            return Upload(f, offset, src, bytes);
        });
    }
};

void CheckDatabase() {
    Database<Flights> db;
    FakeGpu<Instance> gpu;
    Instance a{};
    a.worldPosition = tour::math::SplitDoubleFloat(
        glm::dvec3(1.0e12 + 0.125, -1.0e12, 2.0));
    a.refs.x = 3;

    const auto one = db.instances.Insert(a);
    CHECK(one.slot == 0 && one.generation == 1);
    CHECK(db.instances.Contains(one));
    CHECK(db.instances.LiveCount() == 1);
    for (uint32_t f = 0; f < Flights; ++f)
        CHECK(db.instances.PendingRows(f) == 1);

    // Publish the first flight. Nothing in any other flight is lost.
    CHECK(gpu.Publish(db.instances, 0, true));
    CHECK(db.instances.PendingRows(0) == 0);
    CHECK(gpu.mirror[0][0].identity.x == 1);
    CHECK(gpu.mirror[0][0].identity.y == 1);

    // Change the same slot repeatedly before flight 1 is reused.
    a.refs.x = 6;
    CHECK(db.instances.Update(one, a));
    a.refs.x = 7;
    CHECK(db.instances.Update(one, a));
    CHECK(db.instances.PendingRows(0) == 1);
    CHECK(db.instances.PendingRows(1) == 1);
    gpu.copies[0].clear();
    CHECK(gpu.Publish(db.instances, 0));
    CHECK(gpu.copies[0].size() == 1);
    CHECK(gpu.copies[0][0].second == sizeof(Row<Instance>));
    CHECK(gpu.mirror[0][0].value.refs.x == 7);

    // A flight which missed three edits still receives the final value.
    CHECK(gpu.Publish(db.instances, 1, true));
    CHECK(gpu.mirror[1][0].value.refs.x == 7);
    CHECK(db.instances.PendingRows(1) == 0);

    // After delete, stale handles must never refer to the new occupant.
    CHECK(db.instances.Erase(one));
    CHECK(!db.instances.Contains(one));
    CHECK(db.instances.Get(one) == nullptr);
    CHECK(!db.instances.Update(one, a));
    CHECK(!db.instances.Erase(one));
    CHECK(gpu.Publish(db.instances, 0));
    CHECK(gpu.mirror[0][0].identity.y == 0);
    CHECK(gpu.mirror[0][0].identity.x == 2);
    const auto two = db.instances.Insert(a);
    CHECK(two.slot == one.slot && two.generation == 2);
    CHECK(!db.instances.Contains(one) && db.instances.Contains(two));
    CHECK(gpu.Publish(db.instances, 0));
    CHECK(gpu.mirror[0][0].identity.y == 1);
    CHECK(gpu.mirror[0][0].identity.x == two.generation);

    // Coalesce adjacent dirty rows even when changed in reverse order.
    const auto three = db.instances.Insert({});
    const auto four = db.instances.Insert({});
    CHECK(gpu.Publish(db.instances, 0, true));
    gpu.copies[0].clear();
    CHECK(db.instances.Update(four, a));
    CHECK(db.instances.Update(three, a));
    CHECK(gpu.Publish(db.instances, 0));
    CHECK(gpu.copies[0].size() == 1);
    CHECK(gpu.copies[0][0].second == 2 * sizeof(Row<Instance>));
    CHECK(db.instances.PendingRows(0) == 0);
    gpu.copies[0].clear();
    CHECK(gpu.Publish(db.instances, 0));
    CHECK(gpu.copies[0].empty());

    // A failed writer cannot acknowledge dirty state; retry succeeds.
    a.refs.x = 99;
    CHECK(db.instances.Update(two, a));
    const bool failed = db.instances.Publish(0, false,
        [](size_t, const void*, size_t) { return false; });
    CHECK(!failed && db.instances.PendingRows(0) == 1);
    CHECK(gpu.Publish(db.instances, 0));
    CHECK(gpu.mirror[0][0].value.refs.x == 99);

    // Reallocation of a GPU mirror forces the entire current snapshot.
    CHECK(gpu.Publish(db.instances, 1, true));
    CHECK(gpu.mirror[1].size() == db.instances.Size());
    CHECK(gpu.mirror[1][two.slot].value.refs.x == 99);

    // No camera-dependent invalidation: unchanged scene = no copies.
    for (uint32_t f = 0; f < Flights; ++f) {
        CHECK(gpu.Publish(db.instances, f, true));
        gpu.copies[f].clear();
        CHECK(gpu.Publish(db.instances, f));
        CHECK(gpu.copies[f].empty());
    }

    // Mesh/LOD/submesh/leaf tables are independently versioned.
    const auto mesh = db.meshes.Insert(Mesh{0, 2, 0x3u, 0});
    const auto lod = db.lods.Insert(Lod{0, 1, 0.05f, 0});
    const auto submesh = db.submeshes.Insert(Submesh{22, 9, 1, 0xFu});
    const auto leaf = db.leaves.Insert(Leaf{});
    CHECK(db.meshes.Contains(mesh));
    CHECK(db.lods.Contains(lod));
    CHECK(db.submeshes.Contains(submesh));
    CHECK(db.leaves.Contains(leaf));
    CHECK(db.meshes.PendingRows(0) == 1);
    CHECK(db.lods.PendingRows(0) == 1);
    CHECK(db.submeshes.PendingRows(0) == 1);
    CHECK(db.leaves.PendingRows(0) == 1);
}

void CheckLargeScene() {
    Table<Instance, Flights> table;
    FakeGpu<Instance> gpu;
    std::vector<Handle<Instance>> ids;
    ids.reserve(100000);
    for (uint32_t i = 0; i < 100000; ++i) {
        Instance v{};
        v.refs.x = i;
        ids.push_back(table.Insert(v));
    }
    CHECK(table.Size() == 100000);
    CHECK(table.LiveCount() == 100000);
    CHECK(gpu.Publish(table, 0, true));
    gpu.copies[0].clear();
    Instance one = *table.Get(ids[54321]);
    one.refs.x = 77;
    CHECK(table.Update(ids[54321], one));
    CHECK(table.PendingRows(0) == 1);
    CHECK(gpu.Publish(table, 0));
    CHECK(gpu.copies[0].size() == 1);
    CHECK(gpu.copies[0][0].first == size_t(54321) * sizeof(Row<Instance>));
    CHECK(gpu.copies[0][0].second == sizeof(Row<Instance>));
}
} // namespace

int main() {
    CheckDatabase();
    CheckLargeScene();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
