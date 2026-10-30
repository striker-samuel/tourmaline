// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <unordered_set>

#include <Engine/WorldPartition/WorldPartition.h>

namespace engine::worldpartition {

void WorldPartition::AddProvider(IWorldStreamProvider* provider)
{
	assert(provider != nullptr);
	for (const IWorldStreamProvider* p : providers_)
		assert(p != provider && "provider registered twice");
	providers_.push_back(provider);
}

void WorldPartition::SetInterests(const StreamingInterest* interests, size_t count)
{
	assert(interests != nullptr || count == 0);
	interests_.assign(interests, interests + count);
	// Highest priority first — one sort here so every provider sees the
	// same order (the primary interest is always interests[0]).
	std::stable_sort(interests_.begin(), interests_.end(),
		[](const StreamingInterest& a, const StreamingInterest& b) {
			return a.priority > b.priority;
		});
}

StreamRequestSink WorldPartition::MakeSink()
{
	if (requestBuffer_.empty())
		requestBuffer_.resize(kStreamRequestInitialCapacity);
	return StreamRequestSink(requestBuffer_, kStreamRequestHardCap);
}

void WorldPartition::UpdateFrame(uint64_t frameIndex)
{
	// Optick: the whole partition tick under one Streaming scope (the
	// per-frame residency driver), tags for the demand it resolved.
	// Sub-phases below get their own scopes so a partition-heavy frame
	// shows WHICH half cost the time (request plumbing vs the providers'
	// commit work — the WorldFields system does its real work there).
	SMVK_PROFILE_STREAM("WorldPartition_UpdateFrame");
	SMVK_PROFILE_TAG("providers", providers_.size());
	SMVK_PROFILE_TAG("interests", interests_.size());
	// (1) interests are already set (SetInterests, pre-frame).
	// (2) providers respond to the resolved interest set.
	for (IWorldStreamProvider* p : providers_)
		p->OnStreamingInterests(interests_.data(), interests_.size());

	// (3) gather + resolve (v1: recorded sorted; the dedupe/priority/
	//     dependency engine grows on this exact seam with the terrain
	//     client — see docs/world_partition.md). The sink grows the
	//     gather scratch on demand: every provider request is recorded,
	//     never silently truncated.
	{
		SMVK_PROFILE_STREAM("WorldPartition_GatherRequests");
		requestCount_ = 0;
		requestDrops_ = 0;
		if (!providers_.empty()) {
			StreamRequestSink sink = MakeSink();
			for (IWorldStreamProvider* p : providers_)
				p->GatherRequests(&sink, frameIndex);
			requestCount_ = sink.Count();
			requestDrops_ = sink.Dropped();
			std::stable_sort(requestBuffer_.begin(),
				requestBuffer_.begin() + requestCount_,
				[](const StreamRequest& a, const StreamRequest& b) {
					return a.priority > b.priority;
				});
		}
		SMVK_PROFILE_TAG("requests", requestCount_);
		SMVK_PROFILE_TAG("requestDrops", requestDrops_);
	}

	// (4) dispatch within budgets — provider-internal in v1 (the
	//     WorldFields provider owns its worker dispatch/upload budget;
	//     the shared I/O + upload throttling lands with terrain).
	// (5) publish only completed, safe-to-use resources.
	{
		// CommitFrame is where a provider does its per-frame work (the
		// WorldFields residency + GPU publish ride this call), so the
		// commit loop is the interesting half of the partition tick.
		SMVK_PROFILE_STREAM("WorldPartition_CommitFrame");
		for (IWorldStreamProvider* p : providers_)
			p->CommitFrame(frameIndex);
	}

	// (6) retire, respecting in-flight GPU references.
	for (IWorldStreamProvider* p : providers_)
		p->RetireFrame(frameIndex);
}

size_t WorldPartition::ActiveCellCount() const
{
	std::unordered_set<WorldCellKey, WorldCellKeyHash> cells;
	for (const StreamingInterest& i : interests_) {
		// The interest's covering root-cell range, XZ.
		const glm::dvec3 min = i.position - glm::dvec3(i.radiusMeters, 0.0, i.radiusMeters);
		const glm::dvec3 max = i.position + glm::dvec3(i.radiusMeters, 0.0, i.radiusMeters);
		const WorldCellKey lo = GetWorldCell(min);
		const WorldCellKey hi = GetWorldCell(max);
		for (int64_t z = lo.z; z <= hi.z; ++z)
			for (int64_t x = lo.x; x <= hi.x; ++x)
				cells.insert(WorldCellKey{ x, z });
	}
	return cells.size();
}

} // namespace engine::worldpartition
