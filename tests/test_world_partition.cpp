// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// WorldPartitionTests: the shared spatial-streaming core's CPU contracts
// (docs/world_partition.md milestone 1 — synthetic cells, no terrain):
//   * world-cell addressing (floor discipline, negative coordinates,
//     exact boundaries, 256 m root / 64 m subcell hierarchy);
//   * field-brick alignment relationships (fog 1 m/voxel -> 4 bricks per
//     root cell axis = one per 64 m subcell; wind 4 m/voxel -> 1);
//   * the interest set (priority-first ordering, layer masks);
//   * the provider frame sequence (interests -> gather -> commit ->
//     retire, with the recorded request currency sorted);
//   * the active-cell coverage accounting.
//
// Hand-rolled runner (no test framework dependency), like the other tests.

#include <cstdint>
#include <iostream>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/WorldPartition/WorldPartition.h>

namespace wp = engine::worldpartition;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name)
{
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

// A recording provider (the synthetic-cell milestone-1 client).
struct RecordingProvider final : wp::IWorldStreamProvider
{
	int interestsCalls = 0;
	int commitCalls = 0;
	int retireCalls = 0;
	size_t lastInterestCount = 0;
	bool sawCommitAfterInterests = false;

	const char* ProviderName() const override { return "Recording"; }
	void OnStreamingInterests(const wp::StreamingInterest* interests,
		size_t count) override
	{
		++interestsCalls;
		lastInterestCount = count;
	}
	size_t GatherRequests(wp::StreamRequestSink* sink,
		uint64_t frameIndex) override
	{
		// One request per interest cell (the diagnostic currency), each
		// stamped with the frame like a real provider would.
		for (size_t i = 0; i < lastInterestCount; ++i) {
			wp::StreamRequest r{};
			r.frameRequested = frameIndex;
			sink->Push(r);
		}
		return lastInterestCount;
	}
	void CommitFrame(uint64_t frameIndex) override
	{
		(void)frameIndex;
		++commitCalls;
	}
	void RetireFrame(uint64_t frameIndex) override
	{
		(void)frameIndex;
		++retireCalls;
	}
};

// A fixed-burst provider (the request-capacity regression driver): pushes
// `count` distinct-priority requests per frame.
struct BurstProvider final : wp::IWorldStreamProvider
{
	size_t count = 0;
	size_t gatherCalls = 0;

	const char* ProviderName() const override { return "Burst"; }
	void OnStreamingInterests(const wp::StreamingInterest*,
		size_t) override {}
	size_t GatherRequests(wp::StreamRequestSink* sink,
		uint64_t frameIndex) override
	{
		++gatherCalls;
		for (size_t i = 0; i < count; ++i) {
			wp::StreamRequest r{};
			r.frameRequested = frameIndex;
			r.priority = float(i); // distinct: the sort is verifiable
			sink->Push(r);
		}
		return count;
	}
	void CommitFrame(uint64_t) override {}
	void RetireFrame(uint64_t) override {}
};

} // namespace

int main()
{
	// ---- cell addressing ----
	{
		const wp::WorldCellKey c = wp::GetWorldCell(glm::dvec3(100.0, 0.0, 100.0));
		Check(c.x == 0 && c.z == 0, "cell.positive-inside-zero");
	}
	{
		const wp::WorldCellKey c = wp::GetWorldCell(glm::dvec3(-1.0, 0.0, 0.0));
		Check(c.x == -1 && c.z == 0, "cell.negative-floor");
	}
	{
		const wp::WorldCellKey c = wp::GetWorldCell(glm::dvec3(-256.0, 0.0, 255.999));
		Check(c.x == -1 && c.z == 0, "cell.exact-negative-boundary");
	}
	{
		const wp::WorldCellKey c = wp::GetWorldCell(glm::dvec3(-256.001, 0.0, 256.0));
		Check(c.x == -2 && c.z == 1, "cell.just-past-boundaries");
	}
	{
		const wp::WorldCellKey c = wp::GetWorldCell(glm::dvec3(256.0, 8.0, 512.0));
		Check(c.x == 1 && c.z == 2, "cell.exact-positive-boundary");
	}
	{
		// Bounds math: cell (-1,-1) spans [-256,0) x [-256,0).
		glm::dvec3 mn(0.0), mx(0.0);
		wp::WorldCellBounds(wp::WorldCellKey{ -1, -1 }, 256.0, mn, mx);
		Check(mn.x == -256.0 && mn.z == -256.0 && mx.x == 0.0 && mx.z == 0.0,
			"cell.bounds-negative");
	}
	{
		const wp::WorldSubcellKey s = wp::GetWorldSubcell(glm::dvec3(-1.0, 0.0, 63.9));
		Check(s.x == -1 && s.z == 0, "subcell.negative-floor");
		const wp::WorldCellKey c = wp::SubcellToCell(s);
		Check(c.x == -1 && c.z == 0, "subcell.to-cell");
	}

	// ---- field-brick alignment relationships ----
	{
		Check(wp::BricksPerRootCell(1.0) == 4,
			"align.fog-1m-brick-per-64m-subcell");
		Check(wp::BricksPerRootCell(4.0) == 1,
			"align.wind-4m-brick-per-256m-cell");
		Check(wp::BricksPerRootCell(0.5) == 8, "align.half-metre-brick");
	}

	// ---- interest ordering + layer masks ----
	{
		wp::WorldPartition p;
		wp::StreamingInterest in[3];
		in[0].priority = 0.5f;
		in[0].layerMask = wp::StreamLayerBit(wp::StreamLayer::Terrain);
		in[1].priority = 2.0f;
		in[1].layerMask = wp::StreamLayerBit(wp::StreamLayer::WorldField);
		in[2].priority = 1.0f;
		p.SetInterests(in, 3);
		Check(p.InterestCount() == 3 && p.Interests()[0].priority == 2.0f
			&& p.Interests()[1].priority == 1.0f,
			"interest.priority-first-sorted");
	}

	// ---- the provider frame sequence ----
	{
		wp::WorldPartition p;
		RecordingProvider rec;
		p.AddProvider(&rec);
		wp::StreamingInterest in;
		in.priority = 1.0f;
		p.SetInterests(&in, 1);
		p.UpdateFrame(7);
		p.UpdateFrame(8);
		Check(rec.interestsCalls == 2 && rec.lastInterestCount == 1,
			"frame.interests-delivered-per-frame");
		Check(rec.commitCalls == 2 && rec.retireCalls == 2,
			"frame.commit-and-retire-order");
		Check(p.RequestCount() == 1, "frame.requests-recorded");
		Check(p.Requests()[0].frameRequested == 8,
			"frame.request-carries-frame");
	}

	// ---- request recording GROWS past the initial 256-slot scratch ----
	// (the silent-truncation regression: providers collectively pushing
	// more than the initial capacity must have EVERY request recorded —
	// a dropped terrain-streaming request is undiagnosable at runtime).
	{
		wp::WorldPartition p;
		BurstProvider burst;
		burst.count = 289; // 17x17 root cells: just past the 256 scratch
		p.AddProvider(&burst);
		wp::StreamingInterest in;
		in.priority = 1.0f;
		p.SetInterests(&in, 1);
		p.UpdateFrame(1);
		Check(p.RequestCount() == 289, "frame.requests-grow-past-256");
		Check(p.DroppedRequestCount() == 0, "frame.requests-none-dropped");
		// The sorted recording contract survives the growth (priority
		// descending) and the LAST request is present, not just the
		// first 256.
		Check(p.Requests()[0].priority == 288.0f
			&& p.Requests()[288].priority == 0.0f
			&& p.Requests()[288].frameRequested == 1,
			"frame.requests-recorded-to-the-end");
		// A second frame reuses the grown scratch (still complete).
		p.UpdateFrame(2);
		Check(p.RequestCount() == 289 && burst.gatherCalls == 2,
			"frame.requests-reuse-scratch");
	}

	// ---- active-cell coverage ----
	{
		wp::WorldPartition p;
		wp::StreamingInterest in;
		in.position = glm::dvec3(0.0, 0.0, 0.0);
		in.radiusMeters = 0.0; // exactly one cell
		p.SetInterests(&in, 1);
		Check(p.ActiveCellCount() == 1, "coverage.point-interest");
		// A radius spanning the 4-corner boundary of (0,0),(1,0),(0,1),(1,1)
		in.radiusMeters = 256.0;
		p.SetInterests(&in, 1);
		Check(p.ActiveCellCount() == 9, "coverage.radius-interest");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
