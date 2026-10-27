// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>

#include <Engine/WorldFields/WorldField.h>
#include <Engine/WorldFields/WorldFieldAtlas.h>

namespace engine::worldfields {

// Destination scope for every world-field GPU write: fragments sample
// (the fog march), vertex/compute too (wind particles, sim passes).
static const smvk::PipelineStage kFieldUploadStages =
	smvk::PipelineStage(smvk::PipelineStageBit_Vertex
		| smvk::PipelineStageBit_Fragment
		| smvk::PipelineStageBit_Compute);

// ============================================================================
// Addressing — the CPU half of world_field.hlsli. IDENTICAL formulas,
// including the floor discipline; the tests pin negative coordinates,
// LOD boundaries and exact brick edges against both.
// ============================================================================

WorldFieldSystem::WorldVoxelAddress WorldFieldSystem::WorldToVoxelAddress(
	glm::vec3 worldPos, glm::vec3 origin, float voxelSizeMeters)
{
	// Continuous voxel cell (negative anywhere the lattice extends below
	// the origin).
	const glm::vec3 cell = (worldPos - origin) / voxelSizeMeters;
	// floor() NOT int-truncation: cell -0.5 must land in brick -1 with
	// local 63.5 — truncation would give brick 0, local -0.5 (one brick
	// off for every negative cell).
	const glm::vec3 brickF = glm::floor(cell / float(kBrickInteriorRes));
	const glm::ivec3 brick(brickF);
	const glm::vec3 local = cell - glm::vec3(brick) * float(kBrickInteriorRes);
	WorldVoxelAddress a;
	a.brick[0] = brick.x;
	a.brick[1] = brick.y;
	a.brick[2] = brick.z;
	a.local[0] = local.x;
	a.local[1] = local.y;
	a.local[2] = local.z;
	return a;
}

glm::ivec3 WorldFieldSystem::BrickOfWorld(glm::vec3 worldPos,
	glm::vec3 origin, float voxelSizeMeters)
{
	const glm::vec3 cell = (worldPos - origin) / voxelSizeMeters;
	return glm::ivec3(glm::floor(cell / float(kBrickInteriorRes)));
}

glm::vec3 WorldFieldSystem::AtlasUvwFromAddress(const WorldVoxelAddress& a,
	const uint32_t slotXYZ[3], const uint32_t atlasDims[3])
{
	// Stored-texel position: slot origin (in stored texels) + interior
	// local coord + the halo skip (+2). The halo means a filtered sample
	// at a brick edge reads its NEIGHBOR's data as stored in THIS slot —
	// never the next slot over (no atlas bleed).
	const glm::vec3 atlasVoxel = glm::vec3(
		float(slotXYZ[0]) * float(kBrickStoredRes) + a.local[0] + float(kBrickBorder),
		float(slotXYZ[1]) * float(kBrickStoredRes) + a.local[1] + float(kBrickBorder),
		float(slotXYZ[2]) * float(kBrickStoredRes) + a.local[2] + float(kBrickBorder));
	return atlasVoxel / glm::vec3(float(atlasDims[0]), float(atlasDims[1]), float(atlasDims[2]));
}

void WorldFieldSystem::DecodeAtlasSlot(uint32_t slot, uint32_t slotsX,
	uint32_t slotsY, uint32_t outXYZ[3])
{
	outXYZ[0] = slot % slotsX;
	outXYZ[1] = (slot / slotsX) % slotsY;
	outXYZ[2] = slot / (slotsX * slotsY);
}

glm::vec4 WorldFieldSystem::DecodeWorldFieldEncoded(glm::vec4 encoded,
	WorldFieldEncoding encoding, glm::vec4 decodeScale, glm::vec4 decodeBias)
{
	// Interpolation happens ENCODED (hardware trilinear); decode after.
	// SqrtUNorm: the sqrt packing of low densities means filtering in the
	// encoded domain is not identical to filtering decoded values — a
	// deliberate precision trade, tested against a dense reference.
	if (encoding == WorldFieldEncoding::SqrtUNorm)
		return decodeScale * (encoded * encoded) + decodeBias;
	return decodeScale * encoded + decodeBias; // Linear + SignedNormalized
}

// ============================================================================
// Registration
// ============================================================================

uint32_t WorldFieldSystem::RegisterFields(const WorldFieldDesc* descs, size_t count,
	WorldFieldHandle* outHandles)
{
	assert(!published_
		&& "field set closes at the first GPU publish (the table buffer is "
		   "sized from the registered set — see the file header)");
	assert(descs != nullptr || count == 0);
	const auto isPow2 = [](uint32_t v) { return v && (v & (v - 1)) == 0; };

	uint32_t registered = 0;
	for (size_t i = 0; i < count; ++i) {
		const WorldFieldDesc& desc = descs[i];
		assert(fields_.size() < kMaxFields && "WORLD_FIELD_MAX_FIELDS reached");
		if (fields_.size() >= kMaxFields)
			break; // graceful remainder skip (assert fired above)
		assert(desc.baseVoxelSizeMeters > 0.0f);
		assert(desc.brickResolution == kBrickInteriorRes
			&& "the GPU sampler hardcodes 64/68 (world_field.hlsli)");
		assert(desc.lodCount >= 1 && desc.lodCount <= kMaxFieldLods);
		const smvk::FormatBlockInfo fmt = smvk::GetFormatBlockInfo(desc.format);
		assert(desc.format != smvk::Format::Undefined && fmt.bytesPerBlock > 0
			&& "field format must carry an atlas payload");
		// Power-of-two table axes (the shader wraps with a bitmask).
		assert(isPow2(desc.tableDimX) && isPow2(desc.tableDimY) && isPow2(desc.tableDimZ));
		assert(desc.tableDimX * desc.tableDimY * desc.tableDimZ <= kMaxPageTableEntriesPerLevel);
		assert(desc.poolSlotsX >= 1 && desc.poolSlotsY >= 1 && desc.poolSlotsZ >= 1);
		assert(desc.poolSlotsX * desc.poolSlotsY * desc.poolSlotsZ <= 32768
			&& "atlas pool slot cap (memory guard)");

		WorldFieldAtlasPool::Desc pd;
		pd.format = desc.format;
		pd.storedBrickSize = kBrickStoredRes;
		pd.slotsX = desc.poolSlotsX;
		pd.slotsY = desc.poolSlotsY;
		pd.slotsZ = desc.poolSlotsZ;
		pd.name = desc.name.empty() ? "world field atlas" : desc.name;

		FieldSlot f;
		f.desc = desc;
		f.gen = nextGen_++;
		f.pool = std::make_unique<WorldFieldAtlasPool>(pd);
		fields_.push_back(std::move(f));
		fieldRuntime_.emplace_back();
		const WorldFieldHandle handle{ uint32_t(fields_.size() - 1), fields_.back().gen };
		if (outHandles)
			outHandles[registered] = handle;
		// Field id == index in fields_ == registration order == the GPU
		// fields[] compact index (dead slots are compacted out at publish).
		++registered;
	}
	return registered;
}

WorldFieldHandle WorldFieldSystem::RegisterField(const WorldFieldDesc& desc)
{
	WorldFieldHandle out{};
	RegisterFields(&desc, 1, &out);
	return out;
}

void WorldFieldSystem::UnregisterField(WorldFieldHandle handle)
{
	assert(!published_ && "field set closes at the first GPU publish");
	if (!handle.Valid() || handle.index >= fields_.size())
		return;
	FieldSlot& f = fields_[handle.index];
	if (!f.alive || f.gen != handle.gen)
		return;
	f.alive = false;
	f.source = nullptr;
	f.pool.reset(); // pre-publish pools own no GPU resources
	// Drop its residency rows (residents + absence cache alike).
	for (auto it = bricks_.begin(); it != bricks_.end();) {
		if (it->second.key.fieldId == handle.index)
			it = bricks_.erase(it);
		else
			++it;
	}
}

void WorldFieldSystem::SetBrickSource(WorldFieldHandle handle,
	IWorldFieldBrickSource* source)
{
	if (!handle.Valid() || handle.index >= fields_.size())
		return;
	FieldSlot& f = fields_[handle.index];
	if (!f.alive || f.gen != handle.gen)
		return;
	f.source = source;
	++f.sourceGen; // in-flight results from the old source drop at drain
	// A new source can answer bricks the old one called empty: drop this
	// field's ABSENCE cache (residents keep their slots; in-flight jobs
	// drain-drop via the sourceGen stamp). A source swap mid-run is not
	// a v1 flow, set once at init.
	for (auto it = bricks_.begin(); it != bricks_.end();) {
		if (it->second.key.fieldId == handle.index
			&& it->second.phase == BrickPhase::Absent)
			it = bricks_.erase(it);
		else
			++it;
	}
}

void WorldFieldSystem::SetBrickSources(const WorldFieldHandle* handles,
	IWorldFieldBrickSource* const* sources, size_t count)
{
	assert(handles != nullptr && sources != nullptr);
	for (size_t i = 0; i < count; ++i)
		if (sources[i])
			SetBrickSource(handles[i], sources[i]);
}

bool WorldFieldSystem::FieldAlive(uint32_t fieldId) const
{
	return fieldId < fields_.size() && fields_[fieldId].alive;
}

smvk::ImageHandle WorldFieldSystem::FieldAtlas(uint32_t fieldId) const
{
	if (fieldId >= fields_.size() || !fields_[fieldId].alive || !fields_[fieldId].pool)
		return {};
	return fields_[fieldId].pool->Image();
}

// ============================================================================
// Residency — the per-frame state machine.
//
// Rules (v1):
//   * windows: per field per LOD, the interest point's brick centered in
//     the power-of-two table; windowMin moves only when the center brick
//     moves (the toroidal table stays put, its entries re-tag).
//   * in-window residents/absents refresh lastUseFrame (the LRU clock +
//     hysteresis base: age implies "outside every window").
//   * missing bricks are requested NEAREST-FIRST (metres, cross-field),
//     under maxBrickLoadsPerFrame; each request generates synchronously
//     through the field's source.
//   * slot shortage: demand-evict the LRU AGED-OUT resident (hysteresis
//     passed, i.e. outside every window); the field's coarsest LOD is
//     protected while any other candidate exists (coarse pages are the
//     fallback of every fine miss). No candidate -> the request is
//     skipped gracefully (forced-exhaustion never corrupts).
//   * aged-out residents past proactiveEvictFrames release proactively
//     (keeps the free list warm without waiting for demand).
// ============================================================================

void WorldFieldSystem::PrepareFrame(smvk::ResourceManager* ctx, uint32_t frameIndex,
	glm::vec3 interestPoint)
{
	lastInterestCenter_ = interestPoint;
	ComputeResidency(lastInterestCenter_, frameIndex);
	if (ctx)
		PublishGpu(*ctx);
	else
		DropPendingGpuWork();
}

void WorldFieldSystem::PrepareFrameInterests(smvk::ResourceManager* ctx, uint64_t frameIndex,
	const worldpartition::StreamingInterest* interests, size_t count)
{
	assert(interests != nullptr || count == 0);
	// The highest-priority WorldField-layer interest (the partition sorts
	// priority-first) drives the clipmap centers, LED along its velocity
	// by policy.interestLeadSeconds — the prefetch rule: fast movement
	// needs residency ahead of the camera, not just around it.
	for (size_t i = 0; i < count; ++i) {
		const worldpartition::StreamingInterest& in = interests[i];
		if ((in.layerMask & worldpartition::StreamLayerBit(
				worldpartition::StreamLayer::WorldField)) == 0)
			continue;
		lastInterestCenter_ = glm::vec3(in.position
			+ in.velocity * double(policy_.interestLeadSeconds));
		break;
	}
	// No field-layer interest this frame: KEEP the last centers (no window
	// churn; residents keep refreshing) and still publish whatever became
	// ready.
	ComputeResidency(lastInterestCenter_, frameIndex);
	if (ctx)
		PublishGpu(*ctx);
	else
		DropPendingGpuWork();
}

void WorldFieldSystem::ComputeResidency(glm::vec3 interestPoint, uint64_t frame)
{
	frame_ = frame;
	statEvictions_ = 0;
	statRequestsSkipped_ = 0;
	pendingUploads_.clear();
	pendingArena_.clear();

	aliveCount_ = 0;
	for (const FieldSlot& f : fields_)
		if (f.alive)
			++aliveCount_;

	// Staging keeps capacity for the registered set (registration is
	// closed once published, so this only grows pre-first-publish).
	EnsureTableStaging();

	if (aliveCount_ == 0) {
		tableUsedBytes_ = 0;
		return;
	}

	// ---- windows + in-window refresh + request collection ----
	requestScratch_.clear();
	for (uint32_t fi = 0; fi < fields_.size(); ++fi) {
		FieldSlot& f = fields_[fi];
		if (!f.alive)
			continue;
		const WorldFieldDesc& d = f.desc;
		FieldRuntime& rt = fieldRuntime_[fi];
		for (uint32_t lod = 0; lod < d.lodCount; ++lod) {
			const float voxel = d.baseVoxelSizeMeters * float(1u << lod);
			const glm::ivec3 center = BrickOfWorld(interestPoint, d.originMeters, voxel);
			const glm::ivec3 dim(int(d.tableDimX), int(d.tableDimY), int(d.tableDimZ));
			const glm::ivec3 winMin = center - dim / 2;
			LevelState& lv = rt.levels[lod];
			if (!lv.established || lv.windowMinBrick != winMin) {
				lv.windowMinBrick = winMin;
				lv.established = true;
				lv.entryCount = d.tableDimX * d.tableDimY * d.tableDimZ;
				tableDirty_ = true;
			}
			// Brick world half-extent (request ordering in metres).
			const float halfExtent = float(kBrickInteriorRes) * 0.5f * voxel;
			for (int cz = 0; cz < dim.z; ++cz)
				for (int cy = 0; cy < dim.y; ++cy)
					for (int cx = 0; cx < dim.x; ++cx) {
						const glm::ivec3 abs = lv.windowMinBrick + glm::ivec3(cx, cy, cz);
						WorldBrickKey key{ fi, lod, abs.x, abs.y, abs.z };
						auto it = bricks_.find(key);
						if (it != bricks_.end()) {
							it->second.lastUseFrame = frame; // in-window: LRU tick
							continue;
						}
						if (!f.source)
							continue; // no source: pages stay not-resident
						const glm::vec3 brickMin = d.originMeters
							+ glm::vec3(abs) * (float(kBrickInteriorRes) * voxel);
						const glm::vec3 c = brickMin + halfExtent;
						const glm::vec3 dlt = interestPoint - c;
						RequestQ rq{ key, glm::dot(dlt, dlt) };
						requestScratch_.push_back(rq);
					}
		}
	}

	// Nearest-first, cross-field comparable (metres).
	std::sort(requestScratch_.begin(), requestScratch_.end(),
		[](const RequestQ& a, const RequestQ& b) {
			if (a.distSq != b.distSq)
				return a.distSq < b.distSq;
			return a.key.fieldId < b.key.fieldId; // tie-break: deterministic
		});

	// ---- proactive eviction (aged residents; protected LODs skipped) ----
	ProactiveEvict(frame);

	// ---- drain completed async generations FIRST (render thread) ----
	// Publishing frees slots + turns requests non-pending before the
	// dispatch below re-collects; stale results (source swapped mid-flight,
	// or the brick left every window — a teleport) are DROPPED: never
	// published, never a stale page. The publication contract is untouched
	// (uploads still flow PublishGpu on this thread, regions before table).
	// Gate: non-empty jobs — NOT workPool_ (draining is pool-independent;
	// an owner uninstalling the pool with jobs outstanding still gets
	// them published; only DISPATCHING needs the pool).
	if (!inFlight_.empty())
		DrainInFlight(frame);

	// ---- request processing under budget ----
	{
		// Arena reserve: payloads must stay pointer-stable for this frame
		// (pendingUploads_ points into it). Worst case = the async drain
		// (up to maxInFlight) + this frame's dispatch (maxBrickLoads).
		size_t maxPayload = 1;
		for (const FieldSlot& f : fields_)
			if (f.alive && f.pool)
				maxPayload = std::max<size_t>(maxPayload, f.pool->StoredBrickBytes());
		pendingArena_.reserve(
			(size_t(policy_.maxInFlightBricks) + policy_.maxBrickLoadsPerFrame)
			* maxPayload);
	}

	uint32_t processed = 0;
	for (size_t ri = 0; ri < requestScratch_.size(); ++ri) {
		if (processed >= policy_.maxBrickLoadsPerFrame) {
			statRequestsSkipped_ += uint32_t(requestScratch_.size() - ri);
			break;
		}
		const WorldBrickKey& key = requestScratch_[ri].key;
		FieldSlot& f = fields_[key.fieldId];
		++processed; // generation counts against the budget, hit or miss

		// ASYNC: dispatch to the work pool; the result publishes in a
		// LATER frame's drain (the frame's publication point has not
		// passed yet for this brick — by design, see the provider doc).
		if (workPool_) {
			if (inFlight_.size() >= policy_.maxInFlightBricks) {
				++statRequestsSkipped_; // in-flight cap — re-request next frame
				continue;
			}
			if (!DispatchBrickJob(key, f)) {
				++statRequestsSkipped_; // no source / enqueue refused
				continue;
			}
			continue;
		}

		// SYNC: generate inline, publish immediately.
		const size_t payloadBytes = f.pool->StoredBrickBytes();
		if (brickScratch_.size() < payloadBytes)
			brickScratch_.resize(payloadBytes);
		const bool ok = f.source && f.source->GenerateBrick(f.desc, key.lod,
			key.x, key.y, key.z, brickScratch_.data(), payloadBytes);
		if (!ok) {
			// Known-empty: negative-cache the key (no slot, no upload,
			// no re-request while it stays in window).
			BrickEntry e{};
			e.key = key;
			e.phase = BrickPhase::Absent;
			e.lastUseFrame = frame;
			bricks_.insert_or_assign(key, e);
			continue;
		}
		(void)PublishBrick(key, f, brickScratch_.data(), payloadBytes, frame);
	}

	if (tableDirty_)
		RebuildTable();
}

// ---------------------------------------------------------------------------
// Async generation plumbing
// ---------------------------------------------------------------------------

bool WorldFieldSystem::DispatchBrickJob(const WorldBrickKey& key, const FieldSlot& f)
{
	assert(workPool_ != nullptr && f.source != nullptr && f.pool != nullptr);
	const size_t payloadBytes = f.pool->StoredBrickBytes();
	// Worker-owned copies (no shared mutable state; the source must be
	// thread-safe per the IWorldFieldBrickSource contract).
	const WorldFieldDesc desc = f.desc;
	IWorldFieldBrickSource* source = f.source;
	const uint32_t lod = key.lod;
	const int32_t bx = key.x, by = key.y, bz = key.z;

	std::future<std::vector<std::byte>> fut;
	try {
		// enqueue BLOCKS on a full ring (the pool's backpressure contract)
		// and THROWS if the pool is stopped — a stopped/unshared-safe pool
		// is an owner bug, but degrading to a skip beats crashing.
		fut = workPool_->enqueue([desc, source, lod, bx, by, bz, payloadBytes]
			() -> std::vector<std::byte> {
			// Value-init then fill: the memset is accepted (a memset of
			// 307 KB is ~10us against a generation pass in the hundreds).
			std::vector<std::byte> payload(payloadBytes, std::byte{});
			if (!source->GenerateBrick(desc, lod, bx, by, bz,
					payload.data(), payloadBytes))
				payload.clear(); // empty vector = "source reports absent"
			return payload;
		});
	}
	catch (const std::exception&) {
		return false;
	}

	InFlightJob job{};
	job.key = key;
	job.payload = std::move(fut);
	job.submitFrame = frame_;
	job.sourceGen = f.sourceGen;
	inFlight_.push_back(std::move(job));

	// Mark the entry InFlight so the request scan does not re-dispatch.
	BrickEntry e{};
	e.key = key;
	e.phase = BrickPhase::InFlight;
	e.lastUseFrame = frame_;
	bricks_.insert_or_assign(key, e);
	return true;
}

void WorldFieldSystem::DrainInFlight(uint64_t frame)
{
	for (size_t i = 0; i < inFlight_.size();) {
		InFlightJob& job = inFlight_[i];
		const std::future_status st = job.payload.wait_for(std::chrono::seconds(0));
		if (st != std::future_status::ready) {
			++i;
			continue;
		}
		const WorldBrickKey key = job.key;
		const uint32_t sourceGen = job.sourceGen;
		std::vector<std::byte> payload = job.payload.get();
		// Swap-remove; the moved-in back element re-processes this slot.
		inFlight_[i] = std::move(inFlight_.back());
		inFlight_.pop_back();

		auto it = bricks_.find(key);
		const bool tracked = it != bricks_.end()
			&& it->second.phase == BrickPhase::InFlight;
		// STALE results never publish: a source swap mid-flight
		// (sourceGen bumped) or an entry that vanished (field teardown).
		const bool staleSource = key.fieldId >= fields_.size()
			|| !fields_[key.fieldId].alive
			|| fields_[key.fieldId].sourceGen != sourceGen;
		if (!tracked || staleSource) {
			if (it != bricks_.end() && it->second.phase == BrickPhase::InFlight)
				bricks_.erase(it); // re-requestable under the new source
			continue;
		}
		// Absent result: negative-cache (same as the sync path).
		if (payload.empty()) {
			it->second.phase = BrickPhase::Absent;
			continue;
		}
		// Teleported away mid-flight? Drop the work — never a stale page.
		if ((frame - it->second.lastUseFrame) > policy_.evictHysteresisFrames) {
			bricks_.erase(it);
			continue;
		}
		FieldSlot& f = fields_[key.fieldId];
		(void)PublishBrick(key, f, payload.data(), payload.size(), frame);
	}
}

void WorldFieldSystem::WaitForInFlightBricks()
{
	// WAIT, never get(): std::future::get() is single-shot (it consumes
	// the shared state — a later drain's get() would throw future_error
	// out of ComputeResidency). wait() blocks without consuming; the
	// drain on the next ComputeResidency owns the payloads.
	for (InFlightJob& job : inFlight_)
		job.payload.wait();
}

bool WorldFieldSystem::PublishBrick(const WorldBrickKey& key, FieldSlot& f,
	const std::byte* payload, size_t payloadBytes, uint64_t frame)
{
	WorldFieldAtlasPool& pool = *f.pool;
	uint32_t slot = 0;
	if (!pool.TryAcquireSlot(key, slot)) {
		WorldBrickKey victim{};
		if (!PickVictimAndRelease(key.fieldId, f, frame, victim)) {
			++statRequestsSkipped_; // pool exhausted — graceful skip
			return false;
		}
		++statEvictions_;
		if (!pool.TryAcquireSlot(key, slot)) {
			// The victim release must have returned a slot; if not, skip
			// rather than upload into a slot we do not own.
			++statRequestsSkipped_;
			return false;
		}
	}

	// Payload -> arena (16-aligned offset; pointer-stable this frame via
	// the reserve), then the queued region upload.
	const size_t aligned = (pendingArena_.size() + 15u) & ~size_t(15u);
	pendingArena_.resize(aligned + payloadBytes);
	std::memcpy(pendingArena_.data() + aligned, payload, payloadBytes);
	pendingUploads_.push_back(WorldFieldRegionUpload{
		pool.Image(), pool.SlotRegion(slot), key, slot,
		pendingArena_.data() + aligned, payloadBytes });

	BrickEntry e{};
	e.key = key;
	e.slot = int32_t(slot);
	e.phase = BrickPhase::Resident;
	e.lastUseFrame = frame;
	bricks_.insert_or_assign(key, e);
	tableDirty_ = true;
	return true;
}

bool WorldFieldSystem::PickVictimAndRelease(uint32_t fieldId, const FieldSlot& f,
	uint64_t frame, WorldBrickKey& outVictim)
{
	const uint32_t coarsest = f.desc.lodCount - 1;
	// Pass 0: protected view — everything except the coarsest LOD (or,
	// with protection off, everything). Pass 1 (protection on): the
	// coarsest bricks, last resort.
	const int passes = policy_.protectCoarsestLod ? 2 : 1;
	for (int pass = 0; pass < passes; ++pass) {
		const bool coarsePass = (pass == 1);
		auto best = bricks_.end();
		for (auto it = bricks_.begin(); it != bricks_.end(); ++it) {
			const BrickEntry& e = it->second;
			if (e.phase != BrickPhase::Resident || e.key.fieldId != fieldId)
				continue;
			const bool isCoarsest = (e.key.lod == coarsest);
			if (isCoarsest != coarsePass)
				continue;
			// Aged-out only: lastUseFrame is the last in-window frame, so
			// hysteresis-protected bricks (recently sampled regions) can
			// never be stolen by a fresh request.
			if ((frame - e.lastUseFrame) <= policy_.evictHysteresisFrames)
				continue;
			if (best == bricks_.end() || e.lastUseFrame < best->second.lastUseFrame)
				best = it; // LRU: smallest last-use frame wins
		}
		if (best != bricks_.end()) {
			outVictim = best->second.key;
			// Release BEFORE the rebound below, in one pass: the map entry
			// dies, the pool free list gains the slot, the table page
			// reverts to not-resident at the rebuild.
			f.pool->ReleaseSlot(uint32_t(best->second.slot), best->second.key);
			bricks_.erase(best);
			tableDirty_ = true;
			return true;
		}
	}
	return false;
}

void WorldFieldSystem::ProactiveEvict(uint64_t frame)
{
	const uint64_t threshold = policy_.proactiveEvictFrames;
	if (threshold == 0)
		return;
	for (auto it = bricks_.begin(); it != bricks_.end();) {
		BrickEntry& e = it->second;
		if (e.phase != BrickPhase::Resident
			|| (frame - e.lastUseFrame) <= threshold) {
			++it;
			continue;
		}
		if (e.key.fieldId >= fields_.size() || !fields_[e.key.fieldId].alive) {
			++it;
			continue;
		}
		FieldSlot& f = fields_[e.key.fieldId];
		const bool isCoarsest = (e.key.lod + 1 == f.desc.lodCount);
		if (policy_.protectCoarsestLod && isCoarsest) {
			++it; // fallback pages stay until demand needs them
			continue;
		}
		f.pool->ReleaseSlot(uint32_t(e.slot), e.key);
		it = bricks_.erase(it);
		tableDirty_ = true;
		++statEvictions_;
	}
}

// ============================================================================
// GPU table build
// ============================================================================

void WorldFieldSystem::EnsureTableStaging()
{
	const size_t capacity = ComputeTableCapacityBytes();
	if (table_.size() < capacity)
		table_.assign(capacity, std::byte{});
}

size_t WorldFieldSystem::ComputeTableCapacityBytes() const
{
	size_t pages = 0;
	for (const FieldSlot& f : fields_) {
		if (!f.alive)
			continue;
		pages += size_t(f.desc.tableDimX) * f.desc.tableDimY * f.desc.tableDimZ
			* f.desc.lodCount;
	}
	return sizeof(WorldFieldTableHeader) + fields_.size() * sizeof(WorldFieldGpu)
		+ pages * sizeof(WorldPageGpu);
}

void WorldFieldSystem::RebuildTable()
{
	// Layout: header | fields[] (compact, registration order) | pages[]
	// (levels in field-then-LOD order; each level's page array is
	// dimX*dimY*dimZ entries, toroidal cell -> linear index z-major).
	const uint32_t fieldStride = sizeof(WorldFieldGpu);
	const uint32_t pagesOffset = uint32_t(sizeof(WorldFieldTableHeader)
		+ aliveCount_ * fieldStride);
	uint32_t pagesCursor = pagesOffset;

	// Pass 1: assign each level its page-array offset (deterministic).
	for (uint32_t fi = 0; fi < fields_.size(); ++fi) {
		if (!fields_[fi].alive)
			continue;
		FieldRuntime& rt = fieldRuntime_[fi];
		for (uint32_t lod = 0; lod < fields_[fi].desc.lodCount; ++lod) {
			rt.levels[lod].pageTableOffset = pagesCursor;
			pagesCursor += rt.levels[lod].entryCount * sizeof(WorldPageGpu);
		}
	}
	tableUsedBytes_ = pagesCursor;
	assert(table_.size() >= tableUsedBytes_ && "table staging capacity");

	// Header.
	{
		WorldFieldTableHeader h{};
		h.fieldCount = aliveCount_;
		h.fieldStride = fieldStride;
		h.pagesOffset = pagesOffset;
		std::memcpy(table_.data(), &h, sizeof(h));
	}

	// Fields + their levels' pages.
	uint32_t out = 0;
	for (uint32_t fi = 0; fi < fields_.size(); ++fi) {
		const FieldSlot& f = fields_[fi];
		if (!f.alive)
			continue;
		const WorldFieldDesc& d = f.desc;
		const FieldRuntime& rt = fieldRuntime_[fi];

		WorldFieldGpu gpu{};
		gpu.originMeters = glm::vec4(d.originMeters, 0.0f);
		gpu.decodeScale = d.decodeScale;
		gpu.decodeBias = d.decodeBias;
		gpu.fallbackValue = d.fallbackValue;
		gpu.atlasSlots = glm::uvec4(d.poolSlotsX, d.poolSlotsY, d.poolSlotsZ, 0);
		gpu.meta = glm::uvec4(
			f.pool ? f.pool->BindlessIndex() : 0xFFFFFFFFu, // UINT32_MAX = no atlas (degrades to fallback)
			uint32_t(d.encoding),
			d.lodCount,
			0);
		for (uint32_t lod = 0; lod < d.lodCount; ++lod) {
			const LevelState& lv = rt.levels[lod];
			WorldLevelGpu& lg = gpu.levels[lod];
			lg.windowMinBrick = glm::ivec4(lv.windowMinBrick, 0);
			lg.windowDim = glm::uvec4(d.tableDimX, d.tableDimY, d.tableDimZ, 0);
			lg.pageTableOffset = lv.pageTableOffset;
			lg.voxelSizeMeters = d.baseVoxelSizeMeters * float(1u << lod);
			lg.reserved[0] = lg.reserved[1] = 0;
		}
		std::memcpy(table_.data() + sizeof(WorldFieldTableHeader)
			+ out * fieldStride, &gpu, sizeof(gpu));

		// The level's page array — TOROIDAL: each entry's LINEAR index is
		// the ABSOLUTE brick's ring slot (brick & (dim-1)), matching the
		// shader lookup exactly. Writing at CELL index instead would only
		// line up when windowMin happens to be a multiple of dim — every
		// window move would silently misplace every page. The ring map is
		// a bijection over the dim^3 window bricks, so each ring slot is
		// written exactly once per rebuild; the absolute-tag guard in the
		// sampler still protects readers against mid-move tables.
		for (uint32_t lod = 0; lod < d.lodCount; ++lod) {
			const LevelState& lv = rt.levels[lod];
			const glm::ivec3 dim(int(d.tableDimX), int(d.tableDimY), int(d.tableDimZ));
			const glm::ivec3 mask(dim - glm::ivec3(1));
			WorldPageGpu page{};
			uint32_t written = 0;
			for (int cz = 0; cz < dim.z; ++cz)
				for (int cy = 0; cy < dim.y; ++cy)
					for (int cx = 0; cx < dim.x; ++cx) {
						const glm::ivec3 abs = lv.windowMinBrick + glm::ivec3(cx, cy, cz);
						page.brickX = abs.x;
						page.brickY = abs.y;
						page.brickZ = abs.z;
						page.atlasSlot = -1; // not resident (empty tag = the abs brick)
						const WorldBrickKey key{ fi, lod, abs.x, abs.y, abs.z };
						const auto it = bricks_.find(key);
						if (it != bricks_.end()
							&& it->second.phase == BrickPhase::Resident)
							page.atlasSlot = it->second.slot;
						// int & on two's complement == the shader's uint3
						// wrap for negative bricks (-1 & 3 == 3).
						const glm::ivec3 ring = abs & mask;
						const uint32_t idx = uint32_t(
							(ring.z * dim.y + ring.y) * dim.x + ring.x);
						std::memcpy(table_.data() + lv.pageTableOffset
							+ idx * sizeof(WorldPageGpu), &page, sizeof(page));
						++written;
					}
			assert(written == lv.entryCount);
		}
		++out;
	}
	// NOTE: does NOT clear tableDirty_ — the flag means "the GPU copy is
	// stale" (PublishGpu clears it after the buffer upload). RebuildTable
	// only refreshes the CPU staging.
}

// ============================================================================
// GPU publication
// ============================================================================

bool WorldFieldSystem::EnsurePoolsGpu(smvk::ResourceManager& ctx, bool& outAnyCreated)
{
	outAnyCreated = false;
	for (FieldSlot& f : fields_) {
		if (!f.alive || !f.pool)
			continue;
		if (!f.pool->GpuReady()) {
			if (!f.pool->EnsureGpu(ctx))
				return false; // exhausted: whole system degrades to CPU-only
			outAnyCreated = true;
		}
	}
	return true;
}

bool WorldFieldSystem::EnsureTableGpu(smvk::ResourceManager& ctx)
{
	if (tableBuffer_.valid() && tableBda_ != 0)
		return true;
	const size_t capacity = ComputeTableCapacityBytes();
	tableBuffer_ = ctx.CreateBuffer(capacity, smvk::BufferType_Storage,
		"world field table");
	if (tableBuffer_.empty()) {
		smvk::OnResourceExhaustion("WorldFieldSystem::EnsureTableGpu");
		return false;
	}
	// Creation ZERO-FILL: DEVICE_LOCAL fresh allocations recycle freed
	// pages and can start garbage (the GPU-counter convention, AGENTS.md).
	// A zeroed header reads fieldCount 0 — benign even if a frame ever
	// sampled ahead of the first real upload.
	std::vector<std::byte> zeros(capacity, std::byte{});
	ctx.UploadDataToBuffer(tableBuffer_, zeros.data(), capacity,
		kFieldUploadStages, smvk::AccessFlags::ShaderRead);
	tableBda_ = ctx.GetBufferDeviceAddress(tableBuffer_);
	return true;
}

void WorldFieldSystem::PublishGpu(smvk::ResourceManager& ctx)
{
	if (aliveCount_ == 0) {
		DropPendingGpuWork();
		return;
	}

	bool anyPoolCreated = false;
	if (!EnsurePoolsGpu(ctx, anyPoolCreated)) {
		DropPendingGpuWork();
		return; // tableDirty_ stays: retried next frame
	}
	if (anyPoolCreated) {
		RebuildTable(); // atlas bindless indices just appeared
		tableDirty_ = true; // the staging refresh must reach the GPU
	}
	if (!EnsureTableGpu(ctx)) {
		DropPendingGpuWork();
		return;
	}
	published_ = true; // the field set is closed from here on

	// Refresh the upload targets: ComputeResidency records pool images
	// best-effort — the pools create their atlases lazily HERE, so a
	// first-publish frame recorded empty handles.
	for (WorldFieldRegionUpload& u : pendingUploads_)
		u.image = FieldAtlas(u.key.fieldId);

	// ---- BATCHED REGION UPLOADS, ONE PER TARGET ATLAS, THEN THE TABLE
	// UPLOAD LAST — the publication contract (see the file header): no
	// page can point at slot data that has not landed, and the batch
	// itself executes only after every previously submitted frame has
	// fully retired (so an overwritten slot can never race an older
	// frame's reads of its previous contents). The blob is this frame's
	// payload arena; per-image groups rebase their entries' srcOffsets
	// onto the group's own [min, maxEnd) span (a span may interleave
	// other pools' payloads — those stage too, no copy reads them).
	if (!pendingUploads_.empty()) {
		struct Group
		{
			smvk::ImageHandle image{};
			const std::byte* blobStart = nullptr;
			const std::byte* blobEnd = nullptr;
			uint32_t count = 0;
		};
		Group groups[8]; // one per field (one pool per field, v1)
		uint32_t groupCount = 0;
		for (const WorldFieldRegionUpload& u : pendingUploads_) {
			Group* g = nullptr;
			for (uint32_t gi = 0; gi < groupCount; ++gi)
				if (groups[gi].image == u.image) {
					g = &groups[gi];
					break;
				}
			if (!g) {
				assert(groupCount < 8 && "one atlas image per field (v1 pools)");
				g = &groups[groupCount++];
				g->image = u.image;
				g->blobStart = u.payload;
				g->blobEnd = u.payload + u.payloadBytes;
			}
			g->blobStart = std::min(g->blobStart, u.payload);
			g->blobEnd = std::max(g->blobEnd, u.payload + u.payloadBytes);
			++g->count;
		}
		for (uint32_t gi = 0; gi < groupCount; ++gi) {
			const Group& g = groups[gi];
			batchCopies_.clear();
			batchCopies_.reserve(g.count);
			for (const WorldFieldRegionUpload& u : pendingUploads_) {
				if (!(u.image == g.image))
					continue;
				smvk::ImageRegionCopy3D copy{};
				copy.region = u.region;
				copy.srcOffset = size_t(u.payload - g.blobStart);
				batchCopies_.push_back(copy);
			}
			// ONE call per image: one ring allocation, one memcpy pass,
			// ONE layout-transition pair, one copy command with every
			// region (the BulkCopyBuffer pattern, image edition — N
			// single uploads would cost N barrier pairs).
			ctx.UploadDataToImage3DRegions(g.image,
				batchCopies_.data(), batchCopies_.size(),
				g.blobStart, size_t(g.blobEnd - g.blobStart),
				kFieldUploadStages, smvk::AccessFlags::ShaderRead);
		}
	}
	if (tableDirty_) {
		ctx.UploadDataToBuffer(tableBuffer_, table_.data(), tableUsedBytes_,
			kFieldUploadStages, smvk::AccessFlags::ShaderRead);
		tableDirty_ = false; // the GPU copy now matches the staging
	}

	binding_ = WorldFieldGpuBinding{ tableBda_, aliveCount_ };
	pendingUploads_.clear();
	pendingArena_.clear(); // capacity kept (the reserve high-water)
}

void WorldFieldSystem::DropPendingGpuWork()
{
	// Headless/tests: the payloads were NOT executed against any atlas —
	// callers that want to simulate the atlas consume PendingUploads()
	// BEFORE dropping. Residency state stands (the table says resident);
	// with no GPU consumers that is consistent by definition.
	pendingUploads_.clear();
	pendingArena_.clear();
}

WorldFieldStats WorldFieldSystem::Stats() const
{
	WorldFieldStats s;
	s.fieldCount = aliveCount_;
	s.pendingUploads = uint32_t(pendingUploads_.size());
	s.evictions = statEvictions_;
	s.requestsSkipped = statRequestsSkipped_;
	s.inFlightBricks = uint32_t(inFlight_.size());
	for (const auto& kv : bricks_) {
		if (kv.second.phase == BrickPhase::Resident)
			++s.residentBricks;
		else if (kv.second.phase == BrickPhase::Absent)
			++s.absentBricks;
		// (InFlight counts via inFlightBricks above)
	}
	for (const FieldSlot& f : fields_) {
		if (!f.alive || !f.pool)
			continue;
		s.freeSlots += f.pool->FreeSlotCount();
		s.atlasBytes += f.pool->AtlasBytes();
	}
	return s;
}

} // namespace engine::worldfields
