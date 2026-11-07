// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only regression tests for the VMA-budget warning state machine.
#include <Engine/RHI/gpu_memory_spill.h>

#include <cstdio>

namespace {
int failures = 0;

void Expect(bool ok, const char* test) {
    if (!ok) {
        std::fprintf(stderr, "FAIL gpu-memory.%s\\n", test);
        ++failures;
    }
}

smvk::GpuMemoryPressureSample Sample(unsigned percent,
                                     uint64_t nonlocalMiB = 0,
                                     bool nonlocalAvailable = true) {
    return {uint64_t(percent) * 10,
            1000,
            nonlocalMiB * 1024ull * 1024ull,
            nonlocalAvailable};
}

void TestPressureAndRecovery() {
    smvk::GpuMemorySpillDetector detector;
    auto r = detector.Update(Sample(50), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Healthy && r.changed, "initial-healthy");

    r = detector.Update(Sample(85), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Healthy, "pressure-not-immediate");
    r = detector.Update(Sample(85), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Pressure && r.changed, "pressure-after-2s");
    r = detector.Update(Sample(95), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Critical && r.changed, "critical-immediate");
    for (int i = 0; i < 4; ++i)
        r = detector.Update(Sample(79), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Critical, "recovery-hysteresis");
    r = detector.Update(Sample(79), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Healthy && r.changed, "recovery-after-5s");
}

void TestSpillHeuristic() {
    smvk::GpuMemorySpillDetector detector;
    (void)detector.Update(Sample(50, 10), 1.0);
    auto r = detector.Update(Sample(86, 80), 1.0);
    Expect(r.state != smvk::GpuMemoryWarning::SpillSuspected, "spill-requires-duration");
    r = detector.Update(Sample(86, 80), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::SpillSuspected &&
               r.nonlocalGrowthBytes >= 64ull * 1024ull * 1024ull,
           "spill-suspected-after-pressure-and-growth");
    r = detector.Update(Sample(86, 10), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Pressure, "spill-deescalates");
}

void TestUnavailableAndReset() {
    smvk::GpuMemorySpillDetector detector;
    auto r = detector.Update({}, 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Unavailable && !r.changed,
           "unavailable-no-budget");
    (void)detector.Update(Sample(95), 1.0);
    r = detector.Update({}, 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Unavailable && r.changed,
           "unavailable-transition");
    r = detector.Update(Sample(85), 1.0);
    Expect(r.state == smvk::GpuMemoryWarning::Healthy, "unavailable-resets-counters");
}
} // namespace

int main() {
    TestPressureAndRecovery();
    TestSpillHeuristic();
    TestUnavailableAndReset();
    return failures ? 1 : 0;
}
