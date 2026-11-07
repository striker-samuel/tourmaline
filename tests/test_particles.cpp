// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// particles_tests: CPU-side tests for the GPU particle system builder
// (Engine/Rendergraph/particles.h). No GPU required: the struct ABI mirrors
// (C++ <-> particles.hlsli, sizes asserted on the C++ side), the free-list
// init image, the sim dispatch math and the emitter spawn-quota arithmetic
// (float-exact CPU mirror of the emit CS carry loop) are all pure code.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <Engine/Particles/source_graph.h>
#include <Engine/Rendergraph/particles.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL " << name << "\n";
    }
}

// ---------------------------------------------------------------------------
// T1: struct ABI mirrors (particles.hlsli). The HLSL side cannot
// static_assert (DXC 2018 mode rejects it at global scope), so the C++
// sizes/offsets are the contract.
// ---------------------------------------------------------------------------
void TestStructAbi() {
    Check(sizeof(smvk::MeshParticleGpu) == 96, "pt.mesh.abi-particle");
    Check(sizeof(smvk::MeshParticleEmitterGpu) == 128, "pt.mesh.abi-emitter");
    Check(sizeof(smvk::MeshParticlesEmitPush) == 48, "pt.mesh.abi-emit-push");
    Check(sizeof(smvk::MeshParticlesSimPush) == 48, "pt.mesh.abi-sim-push");
    Check(sizeof(smvk::MeshParticlesCullPush) == 208, "pt.mesh.abi-cull-push");
    Check(sizeof(smvk::MeshParticlesVsPush) == 32, "pt.mesh.abi-vs-push");
    Check(sizeof(smvk::ParticleGpu) == 64, "pt.abi.particle-size");
    Check(sizeof(smvk::ParticleGpu) % 16 == 0, "pt.abi.particle-align");
    Check(sizeof(smvk::ParticleEmitterGpu) == 128, "pt.abi.emitter-size");
    Check(sizeof(smvk::ParticleEmitterGpu) % 16 == 0, "pt.abi.emitter-align");
    Check(offsetof(smvk::ParticleEmitterGpu, tail) == 112, "pt.abi.emitter-tail-offset");

    // Default rows must carry a zero GPU carry (uploaded rows leave
    // scheduling to the emitterState buffer).
    smvk::ParticleEmitterGpu def{};
    Check(def.tail.x == 0.0f, "pt.abi.emitter-default-carry-zero");

    // Push layouts (must match particles_emit/sim/draw.hlsl mirrors).
    Check(sizeof(smvk::ParticlesEmitPush) == 48, "pt.abi.emit-push-size");
    Check(sizeof(smvk::ParticlesSimPush) == 64, "pt.abi.sim-push-size");
    Check(offsetof(smvk::ParticlesSimPush, cameraX) == 48, "pt.abi.sim-push-camera");
    Check(sizeof(smvk::ParticlesDrawPush) == 80, "pt.abi.draw-push-size");
    Check(offsetof(smvk::ParticlesDrawPush, fpTileCountX) == 48, "pt.abi.draw-push-tilecountx");
    Check(offsetof(smvk::ParticlesDrawPush, maxParticles) == 64, "pt.abi.draw-push-maxparticles");
    Check(offsetof(smvk::ParticlesEmitPush, frameSeed) == 44, "pt.abi.emit-push-frameseed");
}

// ---------------------------------------------------------------------------
// T2: free-list init image (Particles::Create primes the stack from this;
// the emit CS pops slots[top-1], the sim CS pushes at slots[top]).
// ---------------------------------------------------------------------------
void TestFreeListInit() {
    {
        const auto data = smvk::Particles::BuildFreeListInitData(8);
        Check(data.size() == 9, "pt.freelist.size");
        Check(data[0] == 8, "pt.freelist.count-full");
        bool slotsOk = data.size() == 9;
        for (uint32_t i = 0; i < 8; ++i)
            slotsOk = slotsOk && data[i + 1] == i;
        Check(slotsOk, "pt.freelist.identity-slots");
    }
    {
        const auto data = smvk::Particles::BuildFreeListInitData(1);
        Check(data.size() == 2 && data[0] == 1 && data[1] == 0, "pt.freelist.single-slot");
    }
}

// ---------------------------------------------------------------------------
// T3: sim dispatch group count (ceil; the CS guards the tail).
// ---------------------------------------------------------------------------
void TestSimGroups() {
    using smvk::Particles;
    Check(Particles::SimGroupsFor(0, 64) == 0, "pt.groups.zero");
    Check(Particles::SimGroupsFor(1, 64) == 1, "pt.groups.one-slot");
    Check(Particles::SimGroupsFor(64, 64) == 1, "pt.groups.exact");
    Check(Particles::SimGroupsFor(65, 64) == 2, "pt.groups.tail");
    Check(Particles::SimGroupsFor(65536, 64) == 1024, "pt.groups.pool");
    // Thread-count guard: a zero/invalid threads argument falls back to 64
    // rather than dividing by zero.
    Check(Particles::SimGroupsFor(128, 0) == 2, "pt.groups.thread-guard");
}

// ---------------------------------------------------------------------------
// T4: spawn quota arithmetic — float-exact CPU mirror of the emit CS
// (carry += rate*dt; quota = floor(carry); carry -= quota). The GPU owns the
// carry; this pins the schedule semantics: no burst, no lost particles.
// ---------------------------------------------------------------------------
void TestSpawnQuota() {
    using smvk::Particles;
    // Exact whole frame: 4/s over a quarter second spawns exactly 1.
    float carry = 0.0f;
    const uint32_t q = Particles::SpawnQuotaForFrame(4.0f, 0.25f, carry, &carry);
    Check(q == 1 && carry == 0.0f, "pt.quota.whole");

    // Fractional rate: 60/s at 60 fps — each frame accumulates 60*dt ~ 1.0
    // (float drift can wobble the floor boundary), so over 60 frames the
    // total must be rate * seconds within +/- 1. No burst, no loss.
    {
        float carry = 0.0f;
        uint32_t spawned = 0;
        const float dt = 1.0f / 60.0f;
        for (int f = 0; f < 60; ++f)
            spawned += Particles::SpawnQuotaForFrame(60.0f, dt, carry, &carry);
        Check(spawned >= 59 && spawned <= 61, "pt.quota.sixty-fps-conserves");
    }
    // Rate 1/s over 10 s (600 frames): the floor boundary can wobble a
    // single spawn either way (carry never lost, just deferred a frame),
    // but the long-window total is conserved: rate * seconds +/- 1.
    {
        float carry = 0.0f;
        uint32_t spawned = 0;
        const float dt = 1.0f / 60.0f;
        for (int f = 0; f < 600; ++f)
            spawned += Particles::SpawnQuotaForFrame(1.0f, dt, carry, &carry);
        Check(spawned >= 9 && spawned <= 11, "pt.quota.one-per-second-conserves");
    }
    // Degenerate inputs: zero rate / zero dt / negative rate never spawn
    // and never poison the carry.
    {
        float carry = 0.5f;
        Check(Particles::SpawnQuotaForFrame(0.0f, 1.0f, carry, &carry) == 0, "pt.quota.zero-rate");
        Check(Particles::SpawnQuotaForFrame(10.0f, 0.0f, carry, &carry) == 0, "pt.quota.zero-dt");
        Check(Particles::SpawnQuotaForFrame(-10.0f, 1.0f, carry, &carry) == 0,
              "pt.quota.negative-rate");
        Check(carry == 0.5f, "pt.quota.carry-preserved");
    }
    // Hibernation spike: dt is clamped by the app (1/30 s), so a 5 s pause
    // spawns at most the clamped quota — this pins the clamp rationale.
    {
        float carry = 0.0f;
        const uint32_t q = Particles::SpawnQuotaForFrame(90.0f, 1.0f / 30.0f, carry, &carry);
        Check(q == 3, "pt.quota.dt-clamp-burst");
    }
}

// ---------------------------------------------------------------------------
// T5: free-list stack invariants under the GPU access pattern (emit pops
// with atomicAdd -1; sim pushes with atomicAdd +1). A slot can never be
// handed out while alive; the count stays inside [0, max].
// ---------------------------------------------------------------------------
void TestFreeListInvariants() {
    constexpr uint32_t kMax = 16;
    auto init = smvk::Particles::BuildFreeListInitData(kMax);
    std::vector<uint32_t> count{init[0]};
    std::vector<uint32_t> slots(init.begin() + 1, init.end());
    std::vector<bool> alive(kMax, false);

    bool invariantsOk = true;
    uint32_t rng = 12345;
    auto pop = [&](uint32_t& slot) {
        if (count[0] < 1 || count[0] > kMax) {
            count[0] = count[0] < 1 ? 0 : count[0]; // GPU restores the miss
            return false;
        }
        count[0] -= 1;
        slot = slots[count[0]];
        if (slot >= kMax || alive[slot]) {
            invariantsOk = false;
            return false;
        }
        alive[slot] = true;
        return true;
    };
    auto push = [&](uint32_t slot) {
        if (!alive[slot]) {
            invariantsOk = false;
            return;
        }
        if (count[0] >= kMax) {
            invariantsOk = false; // overflow guard (unreachable per design)
            return;
        }
        alive[slot] = false;
        slots[count[0]] = slot;
        count[0] += 1;
    };
    // Randomized pop/push churn: quota-batched spawns, staggered deaths.
    for (int frame = 0; frame < 256; ++frame) {
        rng = rng * 1664525u + 1013904223u;
        const uint32_t quota = (rng >> 24) % 5;
        for (uint32_t k = 0; k < quota; ++k) {
            uint32_t slot;
            (void)pop(slot);
        }
        // Kill a few random alive slots (simulate lifetime expiry).
        rng = rng * 1664525u + 1013904223u;
        for (uint32_t i = 0; i < kMax; ++i) {
            if (alive[i] && ((rng >> (i % 31)) & 7) == 0)
                push(i);
        }
    }
    Check(invariantsOk, "pt.freelist.invariants");
    Check(count[0] <= kMax, "pt.freelist.count-bounded");

    // Drain + full death sweep returns the stack to its initial state.
    for (uint32_t i = 0; i < kMax; ++i)
        if (alive[i])
            push(i);
    Check(count[0] == kMax, "pt.freelist.drains-full");
}

} // namespace

void TestParticleSourceGraph() {
    using namespace tour::particles;
    auto g = Graph::Default();
    auto provider =
        [](const std::string& type, int selector, std::vector<Point>& points, std::string& error) {
            if (type != kPainted) {
                error = "unexpected source";
                return false;
            }
            points.push_back({glm::vec3(1, 2, 3)});
            points.push_back({glm::vec3(2, 4, 6)});
            return true;
        };
    auto r = Evaluate(g, provider, 16);
    Check(r.ok && r.emitters.size() == 2, "pt.source.painted-to-emitter");
    Check(r.ok && r.emitters[0].tail.y == smvk::kParticleRender_Dust, "pt.source.dust-mode");
    Check(r.ok && std::abs(r.emitters[0].life.z - 9.0f) < 1e-5f, "pt.source.total-rate-split");
    uint32_t filter = g.AddFilter();
    g.topology.links.clear();
    Check(g.Connect(1, filter, "points") != 0 && g.Connect(filter, 2) != 0,
          "pt.source.typed-connections");
    g.attrs[filter].minY = 3.0f;
    r = Evaluate(g, provider, 16);
    Check(r.ok && r.emitters.size() == 1 && r.emitters[0].position.y == 4.0f,
          "pt.source.height-filter");
    const auto encoded = SerializeAttributes(kEmitter, g.attrs[2]);
    std::string parsedType;
    Attributes parsed;
    Check(ParseAttributes(encoded, parsedType, parsed) && parsedType == kEmitter &&
              parsed.rate == g.attrs[2].rate,
          "pt.source.attributes-roundtrip");
    auto bad = g;
    bad.topology.links.emplace(100, tour::graph::Link{100, 2, 1, "sources", "points"});
    Check(!Evaluate(bad, provider).ok, "pt.source.reject-invalid-link");
    r = Evaluate(Graph::Default(), provider, 1);
    Check(r.ok && r.emitters.size() == 1 && !r.warning.empty(), "pt.source.budget-downsample");
}

void TestMeshParticleSourceCompilation() {
    using namespace tour::particles;
    auto graph = Graph::Default();
    graph.attrs[2].renderMode = kMeshRender;
    graph.attrs[2].meshId = 3;
    const SourceProvider source =
        [](const std::string&, int, std::vector<Point>& points, std::string&) {
            points.push_back({glm::vec3(2.0f, 3.0f, 4.0f)});
            return true;
        };
    auto result = Evaluate(graph, source, 1);
    Check(result.ok && result.emitters.empty() && result.meshEmitters.size() == 1,
          "pt.mesh.compiles-mesh-only");
    if (!result.meshEmitters.empty()) {
        const auto& e = result.meshEmitters.front();
        Check(e.life.w == 3.0f && e.position.x == 2.0f, "pt.mesh.bin-and-position");
        Check(e.life.z == graph.attrs[2].rate && e.size.x == graph.attrs[2].sizeStart,
              "pt.mesh.rate-and-size");
    }
    const auto encoded = SerializeAttributes(kEmitter, graph.attrs[2]);
    std::string type;
    Attributes parsed;
    Check(ParseAttributes(encoded, type, parsed) && parsed.renderMode == kMeshRender &&
              parsed.meshId == 3,
          "pt.mesh.v2-roundtrip");
    graph.attrs[2].meshId = smvk::kMeshParticlesMaxBins;
    Check(!Evaluate(graph, source).ok, "pt.mesh.reject-unsupported-bin");
    Attributes invalid = graph.attrs[2];
    const std::string invalidValue = SerializeAttributes(kEmitter, invalid);
    Check(!ParseAttributes(invalidValue, type, parsed), "pt.mesh.reject-invalid-serialized-bin");
    graph.attrs[2].meshId = 0;
    graph.attrs[2].renderMode = kBillboardRender;
    result = Evaluate(graph, source);
    Check(result.ok && result.emitters.size() == 1 && result.meshEmitters.empty(),
          "pt.mesh.billboard-backward-compatibility");
    const std::string legacy =
        "v1 particle.emitter -1 -100 100 18 0.35 1.8 6 0.3 0.2 0.58 0.49 0.36";
    Check(ParseAttributes(legacy, type, parsed) && parsed.renderMode == kBillboardRender,
          "pt.mesh.legacy-v1-default");
    graph.attrs[2].renderMode = kMeshRender;
    graph.attrs[2].meshId = 1;
    const uint32_t secondSink = 4;
    graph.topology.nodes.emplace(secondSink, Graph::MakeNode(secondSink, kEmitter));
    graph.attrs[secondSink] = {};
    Check(graph.Connect(1, secondSink) != 0, "pt.mesh.second-sink-connect");
    auto mixed = Evaluate(graph, source, 1);
    Check(mixed.ok && mixed.meshEmitters.size() == 1 && mixed.emitters.empty() &&
              !mixed.warning.empty(),
          "pt.mesh.mixed-sinks-share-capacity");
    mixed = Evaluate(graph, source, 2);
    Check(mixed.ok && mixed.meshEmitters.size() == 1 && mixed.emitters.size() == 1,
          "pt.mesh.mixed-sinks-independent-outputs");
}

void TestParticleSourceTopologyRegressions() {
    using namespace tour::particles;
    // Multiple registered providers may fan into the same emitter sink.
    Graph graph = Graph::Default();
    const uint32_t custom = graph.AddSource("particle.source.mesh");
    Check(custom != 0 && graph.Connect(custom, 2) != 0, "pt.source.multiple-sources-connect");
    Check(graph.AddSource("particle.unrelated") == 0, "pt.source.reject-unregistered-namespace");
    const SourceProvider provider =
        [](const std::string& type, int selector, std::vector<Point>& points, std::string& error) {
            if (type == kPainted) {
                points.push_back({glm::vec3(1, 1, 0)});
                return true;
            }
            if (type == "particle.source.mesh") {
                if (selector != 7) {
                    error = "selector not forwarded";
                    return false;
                }
                points.push_back({glm::vec3(2, 2, 0)});
                points.push_back({glm::vec3(3, 3, 0)});
                return true;
            }
            error = "unknown provider";
            return false;
        };
    graph.attrs[custom].selector = 7;
    const auto first = Evaluate(graph, provider, 16);
    const auto second = Evaluate(graph, provider, 16);
    Check(first.ok && first.emitters.size() == 3 && first.emitters[0].position.x == 1.0f &&
              first.emitters[1].position.x == 2.0f && first.emitters[2].position.x == 3.0f,
          "pt.source.multiple-source-order");
    Check(first.ok && second.ok && first.emitters.size() == second.emitters.size() &&
              first.emitters[2].size.w == second.emitters[2].size.w,
          "pt.source.stable-emitter-ids");
    Check(first.ok && first.emitters.size() == 3 &&
              std::abs(first.emitters[0].life.z - 6.0f) < 1e-5f &&
              std::abs(first.emitters[1].life.z - 6.0f) < 1e-5f,
          "pt.source.rate-shared-across-providers");

    const auto capped = Evaluate(graph, provider, 2);
    Check(capped.ok && capped.emitters.size() == 2 && !capped.warning.empty() &&
              capped.emitters[0].position.x == 1.0f && capped.emitters[1].position.x == 2.0f,
          "pt.source.multi-provider-budget-deterministic");
    const auto emptyBudget = Evaluate(graph, provider, 0);
    Check(emptyBudget.ok && emptyBudget.emitters.empty() && !emptyBudget.warning.empty(),
          "pt.source.zero-budget-safe");

    // Source outputs cannot target an output plug or a nonexistent input.
    Check(graph.Connect(custom, 2, "points") == 0, "pt.source.reject-wrong-sink-port");
    Check(graph.Connect(9999, 2) == 0, "pt.source.reject-missing-source");
    Check(graph.Connect(2, custom) == 0, "pt.source.reject-sink-as-source");

    // A height filter has a single input; cycles must not be accepted.
    const uint32_t filterA = graph.AddFilter();
    const uint32_t filterB = graph.AddFilter();
    Check(graph.Connect(1, filterA, "points") != 0 && graph.Connect(custom, filterA, "points") == 0,
          "pt.source.filter-single-input");
    Check(graph.Connect(filterA, filterB, "points") != 0 &&
              graph.Connect(filterB, filterA, "points") == 0,
          "pt.source.reject-filter-cycle");
    Check(graph.Connect(filterA, filterA, "points") == 0, "pt.source.reject-filter-self-link");

    auto missingAttr = graph;
    missingAttr.attrs.erase(custom);
    Check(!Evaluate(missingAttr, provider).ok, "pt.source.reject-missing-attributes");
    auto dangling = graph;
    dangling.topology.links.emplace(999, tour::graph::Link{999, custom, 9999, "points", "sources"});
    Check(!Evaluate(dangling, provider).ok, "pt.source.reject-dangling-link");
    auto unknown = graph;
    unknown.topology.nodes.at(custom).type = "particle.invalid";
    Check(!Evaluate(unknown, provider).ok, "pt.source.reject-unknown-node-type");
    Check(!Evaluate(graph, SourceProvider{}).ok, "pt.source.reject-missing-provider");

    const SourceProvider failingProvider =
        [](const std::string&, int, std::vector<Point>&, std::string& error) {
            error = "test provider failure";
            return false;
        };
    const auto failed = Evaluate(graph, failingProvider);
    Check(!failed.ok && failed.error == "test provider failure",
          "pt.source.provider-error-propagates");

    // Removing a node removes its edges and attributes, leaving a valid graph.
    auto removed = graph;
    Check(removed.Remove(custom) && !removed.topology.nodes.contains(custom) &&
              !removed.attrs.contains(custom) && tour::graph::Validate(removed.topology).ok,
          "pt.source.remove-cleans-up-links");
    Check(!removed.Remove(custom), "pt.source.remove-missing-id");

    // Reject corrupt or unsupported editor-settings values.
    std::string type;
    Attributes parsed;
    const std::string valid = SerializeAttributes(kEmitter, graph.attrs.at(2));
    Check(!ParseAttributes("v3" + valid.substr(2), type, parsed),
          "pt.source.reject-unknown-schema-version");
    Check(!ParseAttributes(valid + " trailing", type, parsed), "pt.source.reject-extra-tokens");
    Check(!ParseAttributes("v1 particle.not-a-source -1 0 1 2 3 4 5 6 7 8 9 10", type, parsed),
          "pt.source.reject-invalid-serialized-type");
}

int main() {
    TestMeshParticleSourceCompilation();
    TestParticleSourceTopologyRegressions();
    TestParticleSourceGraph();
    TestStructAbi();
    TestFreeListInit();
    TestSimGroups();
    TestSpawnQuota();
    TestFreeListInvariants();
    std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
    return g_failures == 0 ? 0 : 1;
}
