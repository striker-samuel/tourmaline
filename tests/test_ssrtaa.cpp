// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ssrtaa_tests: CPU-side contract test for the SSR temporal (TAA) state
// machine (SsrTaaBeginFrame / SsrTaaEndFrame / SsrTaaInvalidateHistory in
// engine/include/Engine/Rendergraph/ssr.h).
//
// What it pins down (mirrors the SmaaT2xState toggle test in
// test_smaajitter.cpp — same record-cadence discipline, scoped to the
// half-res SSR reflection layer):
//   1. Consecutive temporal records alternate the history slot
//      (P0,P1,P0,P1...) and every record after the first blends
//      (historyValid warmup on the first record only).
//   2. A spatial-only record (SSR ran without motion vectors, or SSR off)
//      freezes the phase and invalidates, so the next temporal record
//      warms up instead of blending against stale history.
//   3. The inactive snapshot is zeros (no temporal pass emitted).
//   4. Explicit invalidation (target rebuild / extent drift / AA-mode
//      change) resets to phase 0 with historyValid == false.
//   5. Push-block layout: SsrTemporalPush is 64 bytes of scalars (the
//      header static_asserts this; the HLSL mirror must match it or the
//      driver loads every member after the split at shifted offsets).
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure. No GPU required.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>

#include <Engine/Camera/camera.h>
#include <Engine/Rendergraph/ssr.h>

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

void TestSsrTaaToggle() {
	using namespace smvk;

	// Consecutive temporal records alternate slots; only the first is a
	// warmup (mirrors smaaglobal.fifo-N-consecutive-alternates-and-warms-up).
	{
		SsrTaaState st;
		bool ok = true;
		const int n = 6;
		for (int i = 0; i < n; ++i) {
			const SsrTaaFrameParams p = SsrTaaBeginFrame(st, true);
			if (p.parity != (uint32_t)(i % 2))
				ok = false;
			if (p.historyValid != (i != 0))
				ok = false;
			SsrTaaEndFrame(st, true);
		}
		Check(ok, "ssrtaa.consecutive-alternates-and-warms-up");
	}

	// A spatial-only record freezes the phase and invalidates (never
	// blend stale); re-entry warms up on the frozen phase, then
	// alternation resumes.
	{
		SsrTaaState st;
		SsrTaaEndFrame(st, true);  // record 0 ran temporal: parity 0->1, valid
		SsrTaaEndFrame(st, false); // spatial-only record: freeze + invalidate
		const SsrTaaFrameParams p = SsrTaaBeginFrame(st, true);
		Check(!p.historyValid, "ssrtaa.spatial-record-invalidates");
		Check(p.parity == 1, "ssrtaa.spatial-record-freezes-phase");
		SsrTaaEndFrame(st, true); // re-entry warms up: valid, parity 1->0
		const SsrTaaFrameParams q = SsrTaaBeginFrame(st, true);
		Check(q.historyValid && q.parity == 0, "ssrtaa.reentry-resumes-alternation");
	}

	// Two spatial records in a row stay invalidated on the frozen phase.
	{
		SsrTaaState st;
		SsrTaaEndFrame(st, true);
		SsrTaaEndFrame(st, false);
		SsrTaaEndFrame(st, false);
		const SsrTaaFrameParams p = SsrTaaBeginFrame(st, true);
		Check(!p.historyValid && p.parity == 1, "ssrtaa.double-spatial-stays-frozen");
	}

	// Inactive snapshot is zeros (no temporal pass emitted).
	{
		SsrTaaState st;
		SsrTaaEndFrame(st, true);
		const SsrTaaFrameParams p = SsrTaaBeginFrame(st, false);
		Check(p.parity == 0 && !p.historyValid, "ssrtaa.inactive-snapshot-is-zero");
	}

	// Explicit invalidation (rebuild / extent drift / AA-mode change)
	// resets to phase 0.
	{
		SsrTaaState st;
		SsrTaaEndFrame(st, true);
		SsrTaaEndFrame(st, true);
		SsrTaaInvalidateHistory(st);
		const SsrTaaFrameParams p = SsrTaaBeginFrame(st, true);
		Check(!p.historyValid && p.parity == 0, "ssrtaa.invalidate-resets");
	}

	// Parity is a slot index: out-of-range state still snapshots masked.
	{
		SsrTaaState st;
		st.parity = 7;
		st.historyValid = true;
		const SsrTaaFrameParams p = SsrTaaBeginFrame(st, true);
		Check(p.parity == 1, "ssrtaa.parity-masked-to-slot");
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Dither-epoch contract (SsrIGN in engine/shaders/ssr.frag.hlsl).
//
// Replica of the shader formula (kept line-for-line; any change there
// must update this replica, same discipline as test_smaajitter.cpp):
//   epoch 0 reproduces the legacy static pattern exactly (the
//   spatial-only path passes frameIndex 0, so its look is unchanged),
//   later epochs decorrelate across records (the temporal path passes
//   the record counter, so TAA converges speckle to its mean instead
//   of parking a static pattern), and every value stays in [0,1).
// ---------------------------------------------------------------------------
namespace {

// Pre-change formula (static IGN, no epoch).
float SsrIgnLegacy(float px, float py) {
	const float d = px * 0.06711056f + py * 0.00583715f;
	const float f1 = d - std::floor(d);
	const float f2 = 52.9829189f * f1;
	return f2 - std::floor(f2);
}

// Current shader formula with the dither epoch.
float SsrIgnForTest(float px, float py, uint32_t frameIndex) {
	const uint32_t f = frameIndex & 255u;
	const float qx = px + float(f) * 17.23f;
	const float qy = py + float(f) * 9.71f;
	const float d = qx * 0.06711056f + qy * 0.00583715f;
	const float f1 = d - std::floor(d);
	const float f2 = 52.9829189f * f1;
	return f2 - std::floor(f2);
}

void TestSsrDitherEpoch() {
	// Epoch 0 is bit-exact with the legacy static pattern (spatial-path
	// back-compat) across a sweep of pixels.
	{
		bool ok = true;
		for (int y = 0; y < 16 && ok; ++y)
			for (int x = 0; x < 16 && ok; ++x) {
				const float px = float(x * 131 + 7), py = float(y * 197 + 13);
				if (SsrIgnForTest(px, py, 0u) != SsrIgnLegacy(px, py))
					ok = false;
			}
		Check(ok, "ssrdither.epoch-0-matches-legacy");
	}

	// Every epoch value stays in [0,1) (frac contract).
	{
		bool ok = true;
		for (uint32_t f = 0; f < 300 && ok; ++f) {
			const float v = SsrIgnForTest(123.0f, 456.0f, f);
			if (!(v >= 0.0f && v < 1.0f))
				ok = false;
		}
		Check(ok, "ssrdither.values-in-unit-range");
	}

	// Consecutive epochs decorrelate: a fixed pixel takes many distinct
	// values over 32 records (TAA has something to converge).
	{
		float vals[32];
		for (uint32_t f = 0; f < 32; ++f)
			vals[f] = SsrIgnForTest(123.0f, 456.0f, f);
		int distinct = 1;
		for (int i = 1; i < 32; ++i) {
			bool seen = false;
			for (int j = 0; j < i; ++j)
				if (std::fabs(vals[i] - vals[j]) < 1e-3f) { seen = true; break; }
			if (!seen)
				++distinct;
		}
		char name[64];
		std::snprintf(name, sizeof(name), "ssrdither.decorrelates-32-frames (distinct=%d)", distinct);
		Check(distinct >= 16, name);
	}

	// The epoch wraps every 256 records (bounded float precision).
	{
		const float a = SsrIgnForTest(123.0f, 456.0f, 0u);
		const float b = SsrIgnForTest(123.0f, 456.0f, 256u);
		const float c = SsrIgnForTest(123.0f, 456.0f, 1u);
		const float d = SsrIgnForTest(123.0f, 456.0f, 257u);
		Check(a == b && c == d, "ssrdither.epoch-wraps-at-256");
	}
}

} // namespace

int main() {
	TestSsrTaaToggle();
	TestSsrDitherEpoch();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
