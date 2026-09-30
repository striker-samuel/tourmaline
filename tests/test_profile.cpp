// profile_tests: CPU-side tests for the frame-time histogram state
// machine (Engine/Profile/profile.h — RecordFrameTime /
// GetFrameHistogramData / SetFrameHistogramEnabled). No GPU, no ImGui
// (the drawing side is exercised by the App/editor overlays).
//
// What it pins down:
//   1. disabled: RecordFrameTime records nothing (the zero-cost path —
//      one compare-and-return).
//   2. enable: fresh history; recorded frames land in the right 0.5 ms
//      buckets; avg/max/percentiles follow the bucket-midpoint estimate.
//   3. gaps: >= 1 s deltas (startup/loading) are dropped, not spiked.
//   4. ring: past 1024 frames the oldest contribution retires (count
//      caps, avg tracks the last 1024 only).
//   5. re-enable: the history resets (stale buckets never leak in).

#include <cmath>

#include <Engine/Profile/profile.h>

#include <iostream>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

constexpr size_t kRing = 1024; // mirror of the engine's ring (profile.cpp)

void TestDisabledRecordsNothing() {
	smvk::profiler::SetFrameHistogramEnabled(false);
	smvk::profiler::RecordFrameTime(8.0);
	smvk::profiler::RecordFrameTime(9.0);
	const smvk::profiler::FrameHistogramData d =
		smvk::profiler::GetFrameHistogramData();
	Check(d.count == 0 && d.avgMs == 0.0,
		"profile.hist.disabled-records-nothing");
}

void TestEnableResetsAndRecords() {
	smvk::profiler::SetFrameHistogramEnabled(true);
	smvk::profiler::FrameHistogramData d =
		smvk::profiler::GetFrameHistogramData();
	Check(d.count == 0, "profile.hist.enable-fresh");
	for (int i = 0; i < 100; ++i)
		smvk::profiler::RecordFrameTime(1.0); // bucket 2 [1.0,1.5), mid 1.25
	for (int i = 0; i < 5; ++i)
		smvk::profiler::RecordFrameTime(10.0); // bucket 20, mid 10.25
	smvk::profiler::RecordFrameTime(2.0); // bucket 4, mid 2.25 — the max
	d = smvk::profiler::GetFrameHistogramData();
	Check(d.count == 106, "profile.hist.count");
	Check(d.buckets[2] == 100 && d.buckets[20] == 5 && d.buckets[4] == 1,
		"profile.hist.buckets");
	const double avg = (100 * 1.0 + 5 * 10.0 + 2.0) / 106.0;
	Check(std::abs(d.avgMs - avg) < 1e-9, "profile.hist.avg");
	Check(d.maxMs == 10.0, "profile.hist.max");
	// p50 sits in the 100-frame majority (bucket 2, midpoint 1.25); the
	// 95th of 106 frames is the single 2.0 ms one (bucket 4, mid 2.25);
	// p99 sits in the 10 ms tail (bucket 20, midpoint 10.25).
	Check(d.p50Ms == 1.25, "profile.hist.p50");
	Check(d.p95Ms == 2.25 && d.p99Ms == 10.25, "profile.hist.p95-p99");
	// Bucket sum equals the frame count (nothing double-counted).
	size_t sum = 0;
	for (size_t b = 0; b < smvk::profiler::FrameHistogramData::kBucketCount; ++b)
		sum += d.buckets[b];
	Check(sum == d.count, "profile.hist.bucket-sum");
}

void TestOverflowAndGaps() {
	// Fresh history: SetFrameHistogramEnabled(true) only resets on the
	// false -> true transition (idempotent otherwise) — cycle it.
	smvk::profiler::SetFrameHistogramEnabled(false);
	smvk::profiler::SetFrameHistogramEnabled(true);
	smvk::profiler::RecordFrameTime(1000.0); // >= 1 s: dropped (a gap)
	smvk::profiler::RecordFrameTime(2000.0);
	smvk::profiler::RecordFrameTime(-1.0);   // non-positive: dropped
	smvk::profiler::RecordFrameTime(64.0);   // >= 16 ms: overflow bucket
	smvk::profiler::RecordFrameTime(4.0);
	const smvk::profiler::FrameHistogramData d =
		smvk::profiler::GetFrameHistogramData();
	Check(d.count == 2, "profile.hist.gaps-dropped");
	Check(d.buckets[smvk::profiler::FrameHistogramData::kBucketCount - 1] == 1,
		"profile.hist.overflow-bucket");
	Check(d.maxMs == 64.0, "profile.hist.overflow-max");
	Check(d.p50Ms == 4.25 && d.p99Ms == 16.0, "profile.hist.overflow-percentiles");
}

void TestRingEviction() {
	// Fresh history (idempotent Set — cycle false -> true to reset).
	smvk::profiler::SetFrameHistogramEnabled(false);
	smvk::profiler::SetFrameHistogramEnabled(true);
	for (int i = 0; i < kRing; ++i)
		smvk::profiler::RecordFrameTime(1.0); // bucket 2, mid 1.25
	// Push 512 frames of 3.0 ms (bucket 6): the oldest 512 x 1.0 ms retire.
	for (int i = 0; i < 512; ++i)
		smvk::profiler::RecordFrameTime(3.0);
	const smvk::profiler::FrameHistogramData d =
		smvk::profiler::GetFrameHistogramData();
	Check(d.count == kRing, "profile.hist.ring-capped");
	Check(d.buckets[2] == 512 && d.buckets[6] == 512,
		"profile.hist.ring-eviction-buckets");
	const double avg = (512 * 1.0 + 512 * 3.0) / double(kRing);
	Check(std::abs(d.avgMs - avg) < 1e-9, "profile.hist.ring-avg");
	Check(d.p50Ms == 1.25 || d.p50Ms == 3.25, "profile.hist.ring-p50");
}

void TestReEnableResets() {
	smvk::profiler::SetFrameHistogramEnabled(true);
	for (int i = 0; i < 50; ++i)
		smvk::profiler::RecordFrameTime(2.0);
	smvk::profiler::SetFrameHistogramEnabled(false);
	smvk::profiler::SetFrameHistogramEnabled(true);
	const smvk::profiler::FrameHistogramData d =
		smvk::profiler::GetFrameHistogramData();
	Check(d.count == 0 && d.maxMs == 0.0,
		"profile.hist.reenable-fresh");
	// Leave disabled for the next suite user (the zero-cost default).
	smvk::profiler::SetFrameHistogramEnabled(false);
}

} // namespace

int main() {
	TestDisabledRecordsNothing();
	TestEnableResetsAndRecords();
	TestOverflowAndGaps();
	TestRingEviction();
	TestReEnableResets();
	std::cout << "checks=" << g_checks
		<< " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
