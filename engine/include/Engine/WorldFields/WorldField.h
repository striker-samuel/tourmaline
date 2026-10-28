// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// ============================================================================
// WorldFields — the atlas-backed virtual world volume system (VWVS).
//
// One infrastructure, many independent fields (fog density, wind,
// temperature, humidity, turbulence, future simulation data):
//
//   * VIRTUAL FIELD (this header + WorldFieldSystem): what the voxels
//     mean and where they exist — world-space lattice, LODs, decode,
//     fallback. Signed brick coordinates; the camera-centered clipmap
//     page tables live in world space.
//   * PHYSICAL ATLAS (WorldFieldAtlas.h): where RESIDENT voxel data is
//     stored — one persistent 3D image per pool, bricks assigned to
//     68^3 slots. The atlas is NOT per-brick textures and NOT a render
//     graph transient: a persistent bindless volume texture exactly
//     like the SG-GI probe grids.
//   * GPU TABLE (WorldFieldGpu.h + shaders/world_field.hlsli): the
//     compact page tables + field metadata the shaders read through the
//     FrameUniforms::worldFields BDA, and the ONE shared sampling
//     entry (SampleWorldField) every consumer calls.
//
// Brick geometry (a layout contract with the HLSL sampler):
//   interior 64^3 voxels of field data; the stored brick is 68^3 — a
//   2-voxel halo per side gives trilinear filtering its neighbors across
//   brick boundaries WITHOUT slot-to-slot atlas bleeding, and 68 stays
//   divisible by 4 so BCn blocks (4x4x1) tile a slot exactly. A brick's
//   world extent = interior * voxelSize (a 1 m fog brick covers 64 m; a
//   4 m wind brick covers 256 m — same paging machinery either way).
//
// PUBLICATION + SLOT-REUSE CONTRACT (v1, single render-thread publisher):
//   Everything GPU-bound flows through the upload streamer between
//   frames: brick region copies first, then the page-table buffer upload
//   (one transfer batch, command-buffer order). Every transfer batch
//   executes only after ALL previously submitted graphics frames have
//   fully retired (submitTransfer waits the latest graphics retirement —
//   which covers the frame's whole G1/async-compute/G2 chain — at
//   ALL_COMMANDS), and the current frame's main submit waits the
//   transfer chain. Net effect, structurally, with zero extra fences:
//     * same-frame render passes sample freshly-paged bricks;
//     * overwriting an evicted slot can never race a previous frame's
//       reads of its old contents;
//     * a page-table entry becomes visible only after its region copy
//       landed (batch-internal ordering: regions before table).
//   Relaxing this (worker-thread publishers, transfer-queue streaming
//   concurrent with recording) requires completion tickets + deferred
//   slot recycling FIRST — the shape the later streaming milestone adds.
//
// API convention (AGENTS.md): raw pointers + count/size, never
// containers in parameter lists. Memory the caller touches is caller
// owned; system storage is exposed as pointer+count accessor pairs.
//
// Field ids are REGISTRATION ORDER (the compact index into the GPU
// fields[] array). The set is append-only after the first GPU publish
// (asserted — the table buffer is sized from the registered set);
// register fields at init (WorldRenderer::onAttach does exactly that).
// ============================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/WorldFields/WorldFieldTypes.h>
#include <Engine/WorldFields/WorldFieldGpu.h>
#include <Engine/WorldFields/WorldFieldAtlas.h>
#include <Engine/WorldPartition/WorldPartition.h>

namespace engine::worldfields {

// ---------------------------------------------------------------------------
// Field descriptor — everything the addressing/paging/decode machinery
// needs. Fallback + decode math travel in the GPU table; the system
// never interprets units (m^-1, m/s — all just float4 to it).
// ---------------------------------------------------------------------------
struct WorldFieldDesc
{
	std::string name; // debug name (logs/dumps)

	smvk::Format format = smvk::Format::R8_UNorm;
	WorldFieldEncoding encoding = WorldFieldEncoding::Linear;
	WorldFieldUpdateMode updateMode = WorldFieldUpdateMode::StaticStreamed;

	// LOD 0 voxel edge in metres; LOD L uses base * 2^L.
	float baseVoxelSizeMeters = 1.0f;
	// MUST equal kBrickInteriorRes (the GPU sampler hardcodes 64/68; kept
	// a field so the contract is visible at registration, asserted).
	uint32_t brickResolution = kBrickInteriorRes;
	uint32_t lodCount = 3; // [1, kMaxFieldLods]

	// World-space min corner of brick (0,0,0) — keeps the brick lattice
	// aligned across scenes with negative world extents.
	glm::vec3 originMeters{ 0.0f, 0.0f, 0.0f };

	// Toroidal page-table dims per axis (power of two each;
	// product <= kMaxPageTableEntriesPerLevel). A 8x4x8 window covers
	// e.g. 512m x 256m x 512m of fog LOD 0 — the window is VIRTUAL: not
	// all cells need physical slots.
	uint32_t tableDimX = 8;
	uint32_t tableDimY = 4;
	uint32_t tableDimZ = 8;

	// Physical atlas pool shape (slot grid; one pool per field in v1).
	uint32_t poolSlotsX = 6;
	uint32_t poolSlotsY = 6;
	uint32_t poolSlotsZ = 5;

	// decode: attribute = decodeScale * decodeFn(enc) + decodeBias, per channel.
	glm::vec4 decodeScale{ 1.0f };
	glm::vec4 decodeBias{ 0.0f };
	// DECODED value used when no level resolves (already in world units —
	// e.g. prevailing wind m/s, ambient temperature, 0 fog).
	glm::vec4 fallbackValue{ 0.0f };
};

// ---------------------------------------------------------------------------
// Stable field handle (generation counter for destroy/reuse; the GPU id
// is the registration index — see the file header).
// ---------------------------------------------------------------------------
struct WorldFieldHandle
{
	uint32_t index = 0xFFFFFFFFu;
	uint32_t gen = 0;

	bool Valid() const { return index != 0xFFFFFFFFu; }
	bool operator==(const WorldFieldHandle& o) const { return index == o.index && gen == o.gen; }
};

// ---------------------------------------------------------------------------
// Brick source: world-space procedural or cooked-brick payload provider.
// The payload is the STORED brick (68^3 tight, atlas slot layout —
// interior + halo; a source samples its underlying world function at
// every stored texel's world position, so halos agree with neighbors by
// construction). Tightly packed: texel/block (x,y,z) at
//   uncompressed: ((z * stored) + y) * stored + x  texels
//   BCn         : ((z * blocksY) + by) * blocksX + bx blocks (4x4x1)
// (stored = 68 = kBrickStoredRes). Return false = brick known-empty (the
// pager negative-caches it; pages read it as not-resident).
//
// THREADING CONTRACT: with a work pool installed
// (WorldFieldSystem::SetWorkPool), GenerateBrick runs CONCURRENTLY on
// worker threads — implementations must be pure/thread-safe (no shared
// mutable state; immutable loaded assets are fine). Synchronous mode
// (no pool) runs it on the render thread. The payload buffer is
// caller-owned per call.
// ---------------------------------------------------------------------------
class IWorldFieldBrickSource
{
public:
	virtual ~IWorldFieldBrickSource() = default;
	virtual bool GenerateBrick(const WorldFieldDesc& field, uint32_t lod,
		int32_t x, int32_t y, int32_t z,
		std::byte* payload, size_t payloadBytes) = 0;
};

// ---------------------------------------------------------------------------
// Streaming policy (LRU + hysteresis knobs; see the residency rules in
// WorldField.cpp's ComputeResidency).
// ---------------------------------------------------------------------------
struct WorldFieldStreamingPolicy
{
	// Max bricks GENERATED per frame, across all fields — the dispatch
	// budget. In sync mode this bounds render-thread work; with a work
	// pool it bounds the per-frame dispatch (the in-flight cap below
	// bounds the outstanding set). A 68^3 R8 brick is ~307 KB, BC4 half.
	uint32_t maxBrickLoadsPerFrame = 8;
	// Outstanding async generations across all fields (work-pool mode).
	// The frame's requests beyond this stay un-dispatched and are simply
	// re-collected next frame — graceful, never corrupting.
	uint32_t maxInFlightBricks = 16;
	// A resident brick outside every window for this many frames becomes
	// evictable (hysteresis — camera jitter must not thrash pages).
	uint32_t evictHysteresisFrames = 30;
	// Residents aged past this are released even with no slot demand
	// (proactive free-list refill; skips protected bricks).
	uint32_t proactiveEvictFrames = 120;
	// Never demand-evict a field's coarsest-LOD bricks while any other
	// candidate exists (coarse pages are every fine miss's fallback).
	bool protectCoarsestLod = true;
	// Interest lookahead: clipmap centers lead the primary interest's
	// velocity by this many seconds (fast movement needs residency ahead
	// of the camera; 0 = centers exactly on the interest).
	float interestLeadSeconds = 0.5f;
};

// ---------------------------------------------------------------------------
// One queued region upload (test/tool visible via PendingUploads()).
// payload points into system-owned storage — stable until the next
// ComputeResidency/PublishGpu/DropPendingGpuWork.
// ---------------------------------------------------------------------------
struct WorldFieldRegionUpload
{
	smvk::ImageHandle image{}; // empty = CPU-only mode (no GPU atlas yet)
	smvk::ImageRegion3D region{};
	WorldBrickKey key{};
	uint32_t atlasSlot = 0;
	const std::byte* payload = nullptr;
	size_t payloadBytes = 0;
};

struct WorldFieldStats
{
	uint32_t fieldCount = 0;
	uint32_t residentBricks = 0;
	uint32_t absentBricks = 0; // negative-cached empties (no physical slot)
	uint32_t inFlightBricks = 0; // bricks MARKED in-flight (the map's
	// phase) — InFlightCount() is the live jobs; entries > jobs would be
	// the drained-but-unpublished state (DrainInFlight erases it)
	uint32_t pendingUploads = 0; // this frame's queued region copies
	uint32_t evictions = 0; // this frame
	uint32_t requestsSkipped = 0; // this frame (budget/pool exhausted)
	uint32_t freeSlots = 0; // pool free-list total
	uint64_t atlasBytes = 0; // committed payload bytes of created pools
};

// ---------------------------------------------------------------------------
// WorldFieldSystem — fields + residency + publication.
// Lifecycle: RegisterField (init time) -> SetBrickSource -> per-frame
// PrepareFrame(ctx, frame, eye) BEFORE UpdateFrameUniforms (it publishes
// the BDA/count FrameUniforms carries). Headless-safe: PrepareFrame with
// ctx == nullptr runs the whole residency/window/table build with no GPU
// work (the test path).
// ---------------------------------------------------------------------------
class WorldFieldSystem
{
public:
	WorldFieldSystem() = default;
	~WorldFieldSystem() = default; // Holders self-release (deferred-ordered)

	WorldFieldSystem(const WorldFieldSystem&) = delete;
	WorldFieldSystem& operator=(const WorldFieldSystem&) = delete;

	// ---- registration (append-only after the first GPU publish) ----
	// Batch form (the init flow — one call for the whole field set):
	// registers `count` descriptors in order; outHandles (may be null)
	// receives each handle. Returns how many were registered (stops at
	// capacity; the per-entry validation asserts like the single form).
	uint32_t RegisterFields(const WorldFieldDesc* descs, size_t count,
		WorldFieldHandle* outHandles);
	// The 1-entry convenience (for tests/tools/single late fields).
	WorldFieldHandle RegisterField(const WorldFieldDesc& desc);
	void UnregisterField(WorldFieldHandle handle); // asserts !published_
	// Batch source install (pairs with RegisterFields at init; count
	// entries, may contain nulls to leave a field source-less).
	void SetBrickSources(const WorldFieldHandle* handles,
		IWorldFieldBrickSource* const* sources, size_t count);
	void SetBrickSource(WorldFieldHandle handle, IWorldFieldBrickSource* source);
	uint32_t FieldCount() const { return static_cast<uint32_t>(fields_.size()); }
	bool FieldAlive(uint32_t fieldId) const;
	// The installed brick source (null when source-less/dead/unknown id).
	// Debug/tooling ONLY — the runtime renders whatever the source bakes;
	// a consumer that needs to reason about the CONTENT (the World Map's
	// fog-heat reference layer) uses this to know whether ITS evaluation
	// matches the bricks (the procedural reference vs cooked content).
	IWorldFieldBrickSource* BrickSource(uint32_t fieldId) const;

	// ---- per frame ----
	// ComputeResidency: move clipmap windows to the interest point,
	// request/evict/generate bricks, rebuild the GPU table (CPU-pure on
	// the render thread; async generations dispatch to the work pool
	// here and their results publish in a LATER frame's PublishGpu).
	// PublishGpu: ensure pools/buffer, then ONE BATCHED region upload per
	// target atlas (UploadDataToImage3DRegions — one ring alloc, one
	// memcpy, ONE layout-transition pair, one copy command per image) and
	// the table upload LAST (the publication contract). PrepareFrame =
	// both.
	void PrepareFrame(smvk::ResourceManager* ctx, uint32_t frameIndex,
		glm::vec3 interestPoint);
	// The WorldPartition provider path (Engine/WorldFields/
	// WorldFieldProvider.h): residency follows the RESOLVED interest set
	// — the highest-priority WorldField-layer interest drives the clipmap
	// centers, LED along its velocity by policy.interestLeadSeconds (the
	// prefetch rule: d >= v * t_worstReady + d_safety).
	void PrepareFrameInterests(smvk::ResourceManager* ctx, uint64_t frameIndex,
		const worldpartition::StreamingInterest* interests, size_t count);
	void ComputeResidency(glm::vec3 interestPoint, uint64_t frameIndex);
	void PublishGpu(smvk::ResourceManager& ctx);
	void DropPendingGpuWork(); // discard queued uploads (headless/tests)

	// ---- async generation ----
	// Install/uninstall the worker pool (NOT owned). null = synchronous
	// generation on the render thread (the default; tests/headless). The
	// pool must outlive the system's in-flight jobs (or call
	// WaitForInFlightBricks first).
	void SetWorkPool(smvk::ThreadPool* pool) { workPool_ = pool; }
	smvk::ThreadPool* WorkPool() const { return workPool_; }
	// Block until every in-flight generation lands in the ready list.
	// Owners with STACK-owned sources call this before destroying them
	// (in-flight workers still reference the source).
	void WaitForInFlightBricks();
	uint32_t InFlightCount() const { return uint32_t(inFlight_.size()); }

	// ---- GPU binding (FrameUniforms::worldFields/worldFieldCount) ----
	WorldFieldGpuBinding GpuBinding() const { return binding_; }
	// The persistent table storage buffer (readback/debug; empty before
	// the first publish) and a field's atlas image (the region-upload
	// target; empty while CPU-only). Render-thread use (debug readbacks
	// stall) — the profiler/editor's inspection path.
	smvk::BufferHandle TableBuffer() const { return tableBuffer_; }
	smvk::ImageHandle FieldAtlas(uint32_t fieldId) const;

	// ---- inspection: pointer + count pairs (AGENTS.md convention) ----
	const WorldFieldRegionUpload* PendingUploads() const
	{
		return pendingUploads_.data();
	}
	uint32_t PendingUploadCount() const
	{
		return static_cast<uint32_t>(pendingUploads_.size());
	}
	const std::byte* TableBytes() const { return table_.data(); }
	uint32_t TableUsedBytes() const { return tableUsedBytes_; }
	WorldFieldStats Stats() const;

	void SetStreamingPolicy(const WorldFieldStreamingPolicy& policy)
	{
		policy_ = policy;
	}

	// ---- debug/inspection (the World Map C window's Fields layer; the
	// editor's residency view grows on the same API) — pointer+count
	// collectors, count clamped to capacity, the TRUE total returned ----
	struct WorldFieldDebugBrick
	{
		uint32_t lod = 0;
		int32_t x = 0;
		int32_t y = 0;
		int32_t z = 0;
	};
	struct WorldFieldDebugWindow
	{
		uint32_t lod = 0;
		int32_t minX = 0;
		int32_t minY = 0;
		int32_t minZ = 0;
		uint32_t dimX = 0;
		uint32_t dimY = 0;
		uint32_t dimZ = 0;
		float voxelSizeMeters = 0.0f;
		float originMeters[3] = { 0.0f, 0.0f, 0.0f }; // world min corner of brick (0,0,0)
	};
	// Resident bricks of a field (their world footprints derive from
	// key * interiorRes * voxelSize + the field origin).
	uint32_t CollectDebugBricks(uint32_t fieldId,
		WorldFieldDebugBrick* out, uint32_t capacity) const;
	// The per-LOD clipmap window rects (absolute brick coords).
	uint32_t CollectDebugWindows(uint32_t fieldId,
		WorldFieldDebugWindow* out, uint32_t capacity) const;
	// Generations currently dispatched to the work pool.
	uint32_t CollectDebugInFlight(uint32_t fieldId,
		WorldFieldDebugBrick* out, uint32_t capacity) const;

	// ---- shared addressing math (the CPU half of world_field.hlsli;
	// formulas identical incl. floor discipline — the tests pin it) ----

	// Continuous voxel cell + SIGNED brick + in-brick local coord.
	// floor() everywhere — int truncation maps negative world positions
	// wrong (cell -0.5 must be brick -1, local 63.5 — not brick 0).
	struct WorldVoxelAddress
	{
		int32_t brick[3];
		float local[3]; // [0, kBrickInteriorRes)
	};
	static WorldVoxelAddress WorldToVoxelAddress(glm::vec3 worldPos,
		glm::vec3 origin, float voxelSizeMeters);
	static glm::ivec3 BrickOfWorld(glm::vec3 worldPos, glm::vec3 origin,
		float voxelSizeMeters);
	// The atlas UVW the GPU computes for (slot, local): stored-texel pos =
	// slotOrigin * storedRes + local + border, normalized by atlas dims.
	static glm::vec3 AtlasUvwFromAddress(const WorldVoxelAddress& a,
		const uint32_t slotXYZ[3], const uint32_t atlasDims[3]);
	// Linear slot id -> slot grid XYZ (the HLSL DecodeWorldAtlasSlot).
	static void DecodeAtlasSlot(uint32_t slot, uint32_t slotsX,
		uint32_t slotsY, uint32_t outXYZ[3]);
	// Encoded-texel -> attribute (the HLSL DecodeWorldField).
	static glm::vec4 DecodeWorldFieldEncoded(glm::vec4 encoded,
		WorldFieldEncoding encoding, glm::vec4 decodeScale,
		glm::vec4 decodeBias);

private:
	struct FieldSlot
	{
		WorldFieldDesc desc{};
		uint32_t gen = 0;
		bool alive = true;
		IWorldFieldBrickSource* source = nullptr; // not owned
		uint32_t sourceGen = 0; // bumped on source swap: in-flight results
		// from the previous source are dropped at drain
		std::unique_ptr<WorldFieldAtlasPool> pool; // one pool per field (v1)
	};

	// Brick lifecycle phase (the streamed state machine):
	//   Absent   : known-empty (negative cache; no slot, no upload)
	//   InFlight : generation dispatched to the work pool (sync mode never
	//              holds this state — it goes straight to Resident)
	//   Resident : owns an atlas slot, published in the table
	enum class BrickPhase : uint8_t
	{
		Absent,
		InFlight,
		Resident,
	};

	struct BrickEntry
	{
		WorldBrickKey key{};
		int32_t slot = -1; // physical slot; -1 unless Resident
		BrickPhase phase = BrickPhase::Absent;
		uint64_t lastUseFrame = 0; // last frame inside a window (LRU clock)
	};

	// One dispatched async generation (the future owns the payload; the
	// worker owns nothing shared — desc is copied, the source must be
	// thread-safe per the IWorldFieldBrickSource contract). sourceGen
	// stamps the source-swap generation: results from a swapped-out
	// source never publish.
	struct InFlightJob
	{
		WorldBrickKey key{};
		std::future<std::vector<std::byte>> payload{};
		uint64_t submitFrame = 0;
		uint32_t sourceGen = 0;
	};

	// Per field per LOD window bookkeeping (CPU side of WorldLevelGpu).
	struct LevelState
	{
		glm::ivec3 windowMinBrick{ 0 };
		bool established = false;
		uint32_t pageTableOffset = 0; // byte offset in the page pool
		uint32_t entryCount = 0; // dimX*dimY*dimZ
	};

	struct FieldRuntime
	{
		LevelState levels[kMaxFieldLods];
	};

	// Request collection scratch (per frame, sorted nearest-first).
	struct RequestQ
	{
		WorldBrickKey key{};
		float distSq = 0.0f;
	};

	// -- residency internals --
	bool PickVictimAndRelease(uint32_t fieldId, const FieldSlot& f,
		uint64_t frame, WorldBrickKey& outVictim);
	void ProactiveEvict(uint64_t frame);
	void RebuildTable();
	void EnsureTableStaging();
	size_t ComputeTableCapacityBytes() const;
	bool EnsureTableGpu(smvk::ResourceManager& ctx);
	bool EnsurePoolsGpu(smvk::ResourceManager& ctx, bool& outAnyCreated);
	// Async plumbing: dispatch a generation to the work pool (false =
	// cap reached — re-request next frame); drain completed jobs into the
	// resident/pending-upload path on the render thread (the publication
	// contract is untouched: uploads still flow PublishGpu on the render
	// thread, regions before the table, one batch).
	bool DispatchBrickJob(const WorldBrickKey& key, const FieldSlot& f);
	void DrainInFlight(uint64_t frame);
	void CancelInFlightForField(uint32_t fieldId);
	// The shared resident-publish path (sync + async results converge
	// here): slot acquire (+ demand eviction), payload -> arena, the
	// queued region upload, the table mark.
	bool PublishBrick(const WorldBrickKey& key, FieldSlot& f,
		const std::byte* payload, size_t payloadBytes, uint64_t frame);

	std::vector<FieldSlot> fields_; // grows; index = field id (GPU order)
	std::vector<FieldRuntime> fieldRuntime_; // parallel to fields_
	std::unordered_map<WorldBrickKey, BrickEntry, WorldBrickKeyHash> bricks_;
	std::vector<RequestQ> requestScratch_;
	std::vector<InFlightJob> inFlight_; // outstanding async generations

	std::vector<WorldFieldRegionUpload> pendingUploads_;
	std::vector<std::byte> pendingArena_; // payload storage for pendingUploads_
	std::vector<smvk::ImageRegionCopy3D> batchCopies_; // publish grouping scratch
	std::vector<std::byte> brickScratch_; // source write target, reused
	std::vector<std::byte> table_; // GPU table staging (capacity-sized)
	uint32_t tableUsedBytes_ = 0;
	bool tableDirty_ = false;

	smvk::Holder<smvk::BufferHandle> tableBuffer_;
	uint64_t tableBda_ = 0;
	WorldFieldGpuBinding binding_{};
	bool published_ = false; // first PublishGpu done (field set closes)
	uint32_t aliveCount_ = 0;
	uint32_t nextGen_ = 0;
	smvk::ThreadPool* workPool_ = nullptr; // optional async generation

	WorldFieldStreamingPolicy policy_{};
	uint64_t frame_ = 0;
	glm::vec3 lastInterestCenter_{ 0.0f }; // no-interest frames keep it

	// per-frame stat deltas (reset in ComputeResidency)
	uint32_t statEvictions_ = 0;
	uint32_t statRequestsSkipped_ = 0;
};

} // namespace engine::worldfields
