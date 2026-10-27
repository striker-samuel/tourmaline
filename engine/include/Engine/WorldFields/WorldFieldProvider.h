// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldFieldProvider — WorldFields as a WorldPartition streaming client
// (the partition's first live provider; terrain/ecology/scene follow on
// the same seam — see Engine/WorldPartition/WorldPartition.h).
//
// Mapping the partition's frame sequence onto the system:
//   OnStreamingInterests -> snapshot the resolved interest set
//   CommitFrame          -> PrepareFrameInterests (compute residency +
//                           publish GPU, inside the frame's publication
//                           point — async generations finishing later
//                           defer to the NEXT frame's commit, by design)
//   RetireFrame          -> no-op (the system's proactive eviction runs
//                           inside ComputeResidency on its own clocks)
//
// The system pointer is NOT owned (the WorldRenderer owns both, side by
// side). The context pointer may be null (CPU-only/headless).

#include <Engine/WorldPartition/WorldPartition.h>
#include <Engine/WorldFields/WorldField.h>

namespace engine::worldfields {

class WorldFieldProvider final : public worldpartition::IWorldStreamProvider
{
public:
	explicit WorldFieldProvider(WorldFieldSystem& system)
		: system_(&system)
	{
	}

	void SetContext(smvk::ResourceManager* ctx) { ctx_ = ctx; }

	const char* ProviderName() const override { return "WorldFields"; }

	void OnStreamingInterests(const worldpartition::StreamingInterest* interests,
		size_t count) override
	{
		// Snapshot (the partition passes priority-first; the system's
		// PrepareFrameInterests picks the highest-priority WorldField
		// layer entry). Render thread — plain copy.
		lastInterests_.assign(interests, interests + count);
	}

	size_t GatherRequests(worldpartition::StreamRequestSink* sink,
		uint64_t frameIndex) override;

	void CommitFrame(uint64_t frameIndex) override
	{
		// The publication point: compute residency (windows + async
		// dispatch/drain) and publish (region uploads + the table) in ONE
		// call, on the render thread. Async generations finishing after
		// this point defer to the next frame's commit — by design.
		system_->PrepareFrameInterests(ctx_, frameIndex,
			lastInterests_.empty() ? nullptr : lastInterests_.data(),
			lastInterests_.size());
	}

	void RetireFrame(uint64_t frameIndex) override
	{
		(void)frameIndex; // the system's own eviction owns field retirement
	}

private:
	WorldFieldSystem* system_ = nullptr;
	smvk::ResourceManager* ctx_ = nullptr;
	std::vector<worldpartition::StreamingInterest> lastInterests_; // snapshot
};

} // namespace engine::worldfields
