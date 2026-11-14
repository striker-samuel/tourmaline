// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Device-free procedural-batch publication and lifetime contracts.
#include <Engine/Renderer/gpu_scene_procedural_batches.h>
#include <cstdio>
#include <limits>
using namespace tour::gpu_scene;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #x); } } while (0)

static BatchPublication Tile(ProducerHandle producer, uint64_t local, uint64_t revision) {
    BatchPublication p{};
    p.key = {producer.value, local};
    p.revision = revision;
    p.instancesBda = 4096;
    p.bufferBytes = 4000;
    p.prototypeTable = 99;
    p.count = 100;
    p.capacity = 100;
    p.stride = 40;
    p.format = BatchInstanceFormat::Placement40;
    p.bounds.min[0] = -10;
    p.bounds.max[0] = 10;
    return p;
}
int main() {
    ProceduralBatchRegistry db;
    auto a = db.RegisterProducer();
    auto b = db.RegisterProducer();
    CHECK(a && b && a.value != b.value);
    auto tile = Tile(a, 3, 1);
    CHECK(db.Publish(tile));
    CHECK(db.BatchCount() == 1);
    CHECK(!db.Publish(tile)); // same revision cannot mutate an in-flight publication
    const auto old = *db.Find(tile.key);
    auto snap = db.Snapshot();
    CHECK(snap.size() == 1 && snap[0].incarnation == old.incarnation);
    tile.revision = 2;
    tile.instancesBda = 8192;
    CHECK(db.Publish(tile));
    CHECK(db.Find(tile.key)->incarnation != old.incarnation);
    CHECK(snap[0].data.instancesBda == 4096); // immutable metadata snapshot
    CHECK(!db.Retire(tile.key, 1));
    CHECK(db.Retire(tile.key, 3));
    CHECK(!db.Find(tile.key));
    CHECK(!db.Publish(tile)); // delayed generation cannot resurrect a retired tile
    tile.revision = 4;
    CHECK(db.Publish(tile));
    auto other = Tile(b, 3, 1);
    CHECK(db.Publish(other)); // local keys are producer-scoped
    CHECK(db.RetireProducer(a));
    CHECK(db.BatchCount() == 1 && db.Find(other.key));
    CHECK(!db.Publish(Tile(a, 9, 5))); // dead producers cannot publish
    CHECK(!db.RetireProducer(a));

    auto invalid = Tile(b, 7, 1);
    invalid.count = 101;
    CHECK(!db.Publish(invalid));
    invalid = Tile(b, 7, 1);
    invalid.bufferBytes = 3999;
    CHECK(!db.Publish(invalid));
    invalid = Tile(b, 7, 1);
    invalid.bounds.min[2] = 8;
    invalid.bounds.max[2] = -8;
    CHECK(!db.Publish(invalid));
    invalid = Tile(b, 7, 1);
    invalid.format = static_cast<BatchInstanceFormat>(999);
    CHECK(!db.Publish(invalid));
    invalid = Tile(b, 7, 1);
    invalid.stride = 128;
    CHECK(!db.Publish(invalid));
    invalid = Tile(b, 7, 1);
    invalid.count = 0;
    invalid.capacity = 0;
    invalid.instancesBda = 0;
    invalid.bufferBytes = 0;
    invalid.prototypeTable = 0;
    CHECK(db.Publish(invalid)); // empty tile is published without backing resources
    CHECK(db.Find(invalid.key)->data.count == 0);
    auto full = Tile(b, 8, 1);
    full.format = BatchInstanceFormat::GpuScene128;
    full.stride = 128;
    full.bufferBytes = 12800;
    CHECK(db.Publish(full));
    CHECK(db.Snapshot().size() == 3);
    CHECK(!db.Retire({b.value, 8}, 1));
    CHECK(db.Retire({b.value, 8}, 2));
    CHECK(!db.Find({b.value, 8}));
    std::printf("ProceduralBatchRegistryTests: %s\\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
