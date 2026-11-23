// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Device-free GPU-scene database update model: stable identities,
// coalesced dirty-row ranges, tombstones, and all-flight propagation.
#include <Engine/Renderer/gpu_scene.h>
#include <Engine/RHI/table.h>
#include <type_traits>
#include <Engine/Renderer/gpu_scene_skinning.h>
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
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)
// The GPU-scene interface is now an alias of the reusable smvk table.
static_assert(std::is_same_v<tour::gpu_scene::Table<tour::gpu_scene::Mesh, 3>,
                             smvk::table::Table<tour::gpu_scene::Mesh, 3>>);
static_assert(std::is_same_v<tour::gpu_scene::Row<tour::gpu_scene::Mesh>,
                             smvk::table::Row<tour::gpu_scene::Mesh>>);

namespace {
int g_checks = 0;
int g_failures = 0;
} // namespace

namespace {
using namespace tour::gpu_scene;
constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
static_assert(Flights >= 2);

template <typename RowType> struct FakeGpu {
    using Entry = Row<RowType>;
    std::array<std::vector<Entry>, Flights> mirror{};
    std::array<std::vector<std::pair<size_t, size_t>>, Flights> copies{};

    bool Upload(uint32_t f, size_t offset, const void* src, size_t bytes) {
        CHECK(offset % sizeof(Entry) == 0 && bytes % sizeof(Entry) == 0);
        auto& data = mirror[f];
        const size_t need = (offset + bytes) / sizeof(Entry);
        if (data.size() < need)
            data.resize(need);
        std::memcpy(reinterpret_cast<unsigned char*>(data.data()) + offset, src, bytes);
        copies[f].emplace_back(offset, bytes);
        return true;
    }

    template <uint32_t N> bool Publish(Table<RowType, N>& table, uint32_t f, bool full = false) {
        return table.Publish(f, full, [&](size_t offset, const void* src, size_t bytes) {
            return Upload(f, offset, src, bytes);
        });
    }
};

void CheckDatabase() {
    Database<Flights> db;
    FakeGpu<Instance> gpu;
    Instance a{};
    a.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(1.0e12 + 0.125, -1.0e12, 2.0));
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
    const bool failed =
        db.instances.Publish(0, false, [](size_t, const void*, size_t) { return false; });
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


void CheckPackedSkinWork() {
    PackedSkinWork output{};
    const std::array<SkinWorkSource, 3> sources{{
        {7u, 2u, 0u, 85u, 100u, 4u, 400u, 4u},
        {8u, 3u, 85u, 85u, 300u, 6u, 1200u, 6u},
        {9u, 1u, 0u, 20u, 900u, 3u, 3000u, 3u},
    }};
    CHECK(PackSkinWork(sources, 13u, output));
    CHECK(output.rows.size() == 3);
    CHECK(output.vertexCount == 13u);
    CHECK(output.DeformedBytes() == 13u * 5u * 4u);
    CHECK(output.rows[0].vertices[1] == 0u);
    CHECK(output.rows[1].vertices[1] == 4u);
    CHECK(output.rows[2].vertices[1] == 10u);
    CHECK(output.rows[1].identity[0] == 8u);
    CHECK(output.rows[1].identity[1] == 3u);
    CHECK(output.rows[1].palette[0] == 85u);
    CHECK(output.rows[1].palette[2] == 300u);
    CHECK(output.rows[1].vertices[0] == 1200u);

    // Invalid submissions MUST leave the previously valid work unchanged,
    // so admission can remain on the safe legacy path.
    auto bad = sources;
    bad[1].skinStreamCount = 5u;
    CHECK(!PackSkinWork(bad, 13u, output));
    CHECK(output.rows.size() == 3 && output.vertexCount == 13u);
    bad = sources;
    bad[1].instanceSlot = bad[0].instanceSlot;
    CHECK(!PackSkinWork(bad, 13u, output));
    bad = sources;
    bad[1].instanceGeneration = 0u;
    CHECK(!PackSkinWork(bad, 13u, output));
    bad = sources;
    bad[1].paletteCount = 257u;
    CHECK(!PackSkinWork(bad, 13u, output));
    bad = sources;
    bad[1].bindVertexBase = UINT32_MAX;
    CHECK(!PackSkinWork(bad, 13u, output));
    bad = sources;
    bad[1].skinStreamOffset = UINT32_MAX;
    CHECK(!PackSkinWork(bad, 13u, output));
    CHECK(!PackSkinWork(sources, 12u, output));
    CHECK(output.vertexCount == 13u);

    // Empty frame is valid and reclaims all compact work.
    CHECK(PackSkinWork(std::span<const SkinWorkSource>{}, 0u, output));
    CHECK(output.rows.empty() && output.vertexCount == 0u);
}


 // Deterministic model-based fuzzing of the shared smvk table, independent
 // of Vulkan. Print the seed and step on failure to reproduce in CI.
 struct FuzzRng {
     uint64_t state;
     uint32_t Next() {
         state ^= state >> 12;
         state ^= state << 25;
         state ^= state >> 27;
         return uint32_t((state * UINT64_C(2685821657736338717)) >> 32);
     }
 };
 
 struct alignas(16) FuzzPayload {
     uint32_t stamp = 0;
     uint32_t guard = 0;
     uint32_t reserved[2] = {};
 };
 static_assert(sizeof(FuzzPayload) == 16);
 
 void FuzzTable(uint64_t seed) {
     constexpr uint32_t N = 3;
     using T = smvk::table::Table<FuzzPayload, N>;
     using Id = T::Id;
     using Entry = T::Entry;
     FuzzRng rng{seed};
     T table;
     struct ModelSlot {
         uint32_t generation = 0;
         bool alive = false;
         uint32_t stamp = 0;
     };
     std::vector<ModelSlot> model;
     std::vector<uint32_t> freeList;
     std::vector<Id> stale;
     std::array<std::vector<Entry>, N> gpu;
     std::array<bool, N> initialized{};
     std::array<uint64_t, N> writtenBytes{};
 
     auto verify = [&](uint32_t step) {
         bool ok = table.Size() == model.size();
         uint32_t live = 0;
         for (uint32_t i = 0; i < model.size(); ++i) {
             const auto& m = model[i];
             const auto& r = table.Rows()[i];
             live += m.alive;
             ok &= r.identity.x == m.generation;
             ok &= r.identity.y == uint32_t(m.alive);
             if (m.alive) {
                 ok &= r.value.stamp == m.stamp;
                 ok &= r.value.guard == (m.stamp ^ 0x5a5a5a5au);
                 ok &= table.Contains(Id{i, m.generation});
             } else {
                 ok &= !table.Contains(Id{i, m.generation});
             }
         }
         ok &= table.LiveCount() == live;
         for (auto id : stale)
             ok &= !table.Contains(id) && table.Get(id) == nullptr;
         if (!ok) {
             std::fprintf(stderr, "fuzz seed=%llu step=%u: CPU model mismatch\\n",
                          static_cast<unsigned long long>(seed), step);
             CHECK(ok);
         }
         return ok;
     };
 
     for (uint32_t step = 0; step < 12000; ++step) {
         const uint32_t op = rng.Next() % 10;
         if (op <= 2 && table.Size() < 256) {
             FuzzPayload value{};
             value.stamp = rng.Next();
             value.guard = value.stamp ^ 0x5a5a5a5au;
             const Id id = table.Insert(value);
             uint32_t expectedSlot;
             if (!freeList.empty()) {
                 expectedSlot = freeList.back();
                 freeList.pop_back();
             } else {
                 expectedSlot = uint32_t(model.size());
                 model.push_back({1, false, 0});
             }
             bool ok = id.slot == expectedSlot && id.generation == model[expectedSlot].generation;
             model[expectedSlot].alive = true;
             model[expectedSlot].stamp = value.stamp;
             if (!ok) {
                 std::fprintf(stderr, "fuzz seed=%llu step=%u: insertion mismatch\\n",
                              static_cast<unsigned long long>(seed), step);
                 CHECK(ok);
                 return;
             }
         } else if (op <= 4 && !model.empty()) {
             const uint32_t idx = rng.Next() % uint32_t(model.size());
             const auto& m = model[idx];
             FuzzPayload value{};
             value.stamp = rng.Next();
             value.guard = value.stamp ^ 0x5a5a5a5au;
             const bool updated = table.Update(Id{idx, m.generation}, value);
             if (updated != m.alive) {
                 std::fprintf(stderr, "fuzz seed=%llu step=%u: update mismatch\\n",
                              static_cast<unsigned long long>(seed), step);
                 CHECK(updated == m.alive);
                 return;
             }
             if (updated)
                 model[idx].stamp = value.stamp;
         } else if (op == 5 && !model.empty()) {
             const uint32_t idx = rng.Next() % uint32_t(model.size());
             auto& m = model[idx];
             const Id id{idx, m.generation};
             const bool erased = table.Erase(id);
             if (erased != m.alive) {
                 std::fprintf(stderr, "fuzz seed=%llu step=%u: erase mismatch\\n",
                              static_cast<unsigned long long>(seed), step);
                 CHECK(erased == m.alive);
                 return;
             }
             if (erased) {
                 stale.push_back(id);
                 m.alive = false;
                 ++m.generation;
                 freeList.push_back(idx);
             }
         } else if (op == 6 && !stale.empty()) {
             const Id id = stale[rng.Next() % uint32_t(stale.size())];
             FuzzPayload value{};
             CHECK(!table.Contains(id));
             CHECK(!table.Update(id, value));
             CHECK(!table.Erase(id));
         } else {
             const uint32_t f = rng.Next() % N;
             const bool full = !initialized[f] || (rng.Next() % 37 == 0);
             // Inject a failed writer. If multiple ranges exist, also allow
             // failure after an earlier range succeeded: replay must converge.
             const bool injectFailure = rng.Next() % 9 == 0;
             const uint32_t failAfter = rng.Next() % 3;
             uint32_t writeCalls = 0;
             bool wrote = false;
             const bool result = table.Publish(f, full, [&](size_t offset,
                                                           const void* bytes,
                                                           size_t count) {
                 if (injectFailure && writeCalls++ == failAfter)
                     return false;
                 if (offset % sizeof(Entry) || count % sizeof(Entry) ||
                     offset + count > table.Size() * sizeof(Entry))
                     return false;
                 auto& mirror = gpu[f];
                 if (mirror.size() < table.Size())
                     mirror.resize(table.Size());
                 if (count)
                     std::memcpy(reinterpret_cast<unsigned char*>(mirror.data()) + offset,
                                 bytes, count);
                 writtenBytes[f] += count;
                 wrote = true;
                 return true;
             });
             if (result) {
                 initialized[f] = true;
                 const auto& mirror = gpu[f];
                 bool equal = mirror.size() == table.Rows().size();
                 if (equal) {
                     for (uint32_t i = 0; i < model.size(); ++i) {
                         const auto& actual = mirror[i];
                         const auto& wanted = table.Rows()[i];
                         equal &= actual.identity.x == wanted.identity.x &&
                                  actual.identity.y == wanted.identity.y;
                         if (wanted.identity.y)
                             equal &= actual.value.stamp == wanted.value.stamp &&
                                      actual.value.guard == wanted.value.guard;
                     }
                 }
                 if (!equal) {
                     std::fprintf(stderr, "fuzz seed=%llu step=%u flight=%u: GPU mirror mismatch\\n",
                                  static_cast<unsigned long long>(seed), step, f);
                     CHECK(equal);
                     return;
                 }
                 if (!full && !wrote)
                     CHECK(table.PendingRows(f) == 0);
             }
         }
         if (!verify(step))
             return;
     }
     // Converge every flight and assert a subsequent no-op publication writes nothing.
     for (uint32_t f = 0; f < N; ++f) {
         CHECK(table.Publish(f, !initialized[f], [&](size_t offset, const void* bytes,
                                                      size_t count) {
             auto& mirror = gpu[f];
             mirror.resize(table.Size());
             if (count)
                 std::memcpy(reinterpret_cast<unsigned char*>(mirror.data()) + offset,
                             bytes, count);
             return true;
         }));
         CHECK(table.PendingRows(f) == 0);
         uint32_t writes = 0;
         CHECK(table.Publish(f, false, [&](size_t, const void*, size_t) {
             ++writes;
             return true;
         }));
         CHECK(writes == 0);
         CHECK(gpu[f].size() == table.Size());
         for (uint32_t i = 0; i < model.size(); ++i) {
             CHECK(gpu[f][i].identity.x == model[i].generation);
             CHECK(gpu[f][i].identity.y == uint32_t(model[i].alive));
             if (model[i].alive)
                 CHECK(gpu[f][i].value.stamp == model[i].stamp);
         }
     }
 }
 
 void CheckTableFuzz() {
     // Fixed seeds are reproducible. Add the printed seed/step from any
     // failure to this corpus before fixing a regression.
     for (uint64_t seed : {UINT64_C(1), UINT64_C(0xC0FFEE),
                           UINT64_C(0xDEADBEEF), UINT64_C(0x9E3779B97F4A7C15)})
         FuzzTable(seed);
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
    CheckPackedSkinWork();
    CheckLargeScene();
    CheckTableFuzz();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
