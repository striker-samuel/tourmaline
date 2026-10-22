// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Profile/profile.h>

// USE_OPTICK gates ALL Optick integration; this TU stays
// backend-neutral (the GPU timeline machinery lives behind
// InitBackendGpuTimelines in the linked backend's facade).
#if USE_OPTICK
#include <optick.h>
#endif

namespace smvk::profiler {

namespace {

bool g_enabled = false;     // --use_optick (CPU capture session)
bool g_gpuEnabled = false;  // --use_optick_gpu (GPU timelines; implies enabled)

// Frame-time histogram state (fixed storage: no allocation, no locks — the
// main thread is the only writer/reader; RecordFrameTime hook + UI reads).
constexpr size_t kFrameHistRing = 1024;
struct FrameHistogramState {
	double ring[kFrameHistRing] = {};
	size_t next = 0;
	size_t count = 0;
	uint32_t buckets[FrameHistogramData::kBucketCount] = {};
	double totalMs = 0.0;
	double maxMs = 0.0;
	bool enabled = false;
};
FrameHistogramState g_frameHist;

inline size_t FrameHistBucketOf(double ms) {
	const size_t b = size_t(ms / 0.5); // 0.5 ms buckets
	return b >= FrameHistogramData::kBucketCount
		? FrameHistogramData::kBucketCount - 1 : b;
}

} // namespace

void SetEnabled(bool enabled) { g_enabled = enabled; }
bool Enabled() { return g_enabled; }
void SetGpuEnabled(bool enabled) {
    g_gpuEnabled = enabled;
    if (enabled)
        g_enabled = true; // --use_optick_gpu implies --use_optick
}
bool GpuEnabled() { return g_gpuEnabled; }

void SetFrameHistogramEnabled(bool enabled) {
	FrameHistogramState& h = g_frameHist;
	if (h.enabled == enabled)
		return;
	// (Re)enabling starts a fresh history — stale buckets from an earlier
	// session would misrepresent the run being measured.
	h = FrameHistogramState{};
	h.enabled = enabled;
}
bool FrameHistogramEnabled() { return g_frameHist.enabled; }

void RecordFrameTime(double milliseconds) {
	// Zero cost while disabled: one load + branch, nothing else (see
	// profile.h — the whole feature is opt-in for exactly this reason).
	FrameHistogramState& h = g_frameHist;
	if (!h.enabled)
		return;
	// Startup/loading gaps (first call after boot, world loads, device
	// stalls) are not frame times: drop >= 1 s deltas instead of spiking
	// the history.
	if (!(milliseconds > 0.0) || milliseconds >= 1000.0)
		return;
	if (h.count < kFrameHistRing) {
		++h.count;
	} else {
		// Ring full: retire the evicted frame's bucket/total contribution.
		const double evicted = h.ring[h.next];
		--h.buckets[FrameHistBucketOf(evicted)];
		h.totalMs -= evicted;
	}
	h.ring[h.next] = milliseconds;
	h.next = h.next + 1 == kFrameHistRing ? 0 : h.next + 1;
	++h.buckets[FrameHistBucketOf(milliseconds)];
	h.totalMs += milliseconds;
	if (milliseconds > h.maxMs)
		h.maxMs = milliseconds;
	// Ride along with an Optick capture: the tag lands on the current
	// scope (the DrawFrame event at the UpdateDeltaTime hook sites).
	SMVK_PROFILE_TAG("FrameMs", static_cast<float>(milliseconds));
}

FrameHistogramData GetFrameHistogramData() {
	FrameHistogramData out{};
	const FrameHistogramState& h = g_frameHist;
	out.count = h.count;
	if (h.count == 0)
		return out;
	out.avgMs = h.totalMs / double(h.count);
	out.maxMs = h.maxMs;
	// Percentiles from the bucket counts (bucket midpoint as the estimate;
	// the overflow bucket reports its floor).
	const size_t n50 = (h.count + 1) / 2;
	const size_t n95 = (h.count * 95 + 99) / 100;
	const size_t n99 = (h.count * 99 + 99) / 100;
	bool got50 = false, got95 = false, got99 = false;
	size_t seen = 0;
	for (size_t b = 0; b < FrameHistogramData::kBucketCount; ++b) {
		seen += h.buckets[b];
		const double mid = (b + 1 == FrameHistogramData::kBucketCount)
			? 16.0 : (double(b) + 0.5) * 0.5;
		if (!got50 && seen >= n50) { out.p50Ms = mid; got50 = true; }
		if (!got95 && seen >= n95) { out.p95Ms = mid; got95 = true; }
		if (!got99 && seen >= n99) { out.p99Ms = mid; got99 = true; }
	}
	for (size_t b = 0; b < FrameHistogramData::kBucketCount; ++b)
		out.buckets[b] = h.buckets[b];
	return out;
}

void InitBackendGpuTimelines() {
    // The backend seam: the linked engine facade owns the device/queue
    // natives + Optick's per-API GPU event init (impl.cpp on Vulkan;
    // dx12_rhi_globals.cpp on D3D12). This TU only carries the gates.
    InitBackendGpuTimelinesImpl();
}

void Shutdown() {
#if USE_OPTICK
    OPTICK_SHUTDOWN();
#endif
}

} // namespace smvk::profiler
