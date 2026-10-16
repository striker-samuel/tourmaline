// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Texture streaming (Granite-lite): mip-tail residency with Skull & Bones
// style feedback-driven upgrades.
//
// The problem: every KTX2 texture uploads ALL mips into VRAM at load. A 4K
// BC7 texture is ~16 MiB resident even when it covers 20 pixels.
//
// The model: each streamed texture keeps its full mip chain conceptually,
// but only the contiguous tail [residentBase, fullMips) is backed by VRAM.
// The VkImage is allocated at the tail resolution (base extent >>
// residentBase) with (fullMips - residentBase) levels, so normalized-UV
// sampling just works at lower detail. Changing residency reallocates the
// image IN PLACE (same TextureHandle / ImageViewHandle / bindless slot),
// so baked MaterialValues indices never go stale.
//
// Feedback (what SHOULD be resident) is the minimum of two sources:
//   * GPU readback: the gfx filter reports the wanted mip per visible
//     meshlet's material texture slots (see Renderer/gfx_filter; wired
//     through RequestMipFromLod).
//   * CPU heuristic: screen-coverage / distance demand (WantedMipFor*,
//     driven by the scene pass) covers pre-warming and edge cases.
// Both feed the same min-demand; Update() enforces the budget either way.
//
// Granite path: when sparse residency lands, only the RHI retarget changes
// (bind physical tiles instead of reallocating); this manager, the budget
// and the feedback inputs stay identical.
//
// Threading: render thread only. Update() must run between frames, followed
// by ResourceManager::FlushUploads() to kick the re-uploads.

#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace smvk {

class ResourceManager;

struct TextureStreamerConfig {
	uint64_t budgetBytes = 256ull * 1024 * 1024; // VRAM budget for streamed textures
	uint32_t minResidentMips = 2;   // floor: always keep >= N tail mips (tiny textures stay full)
	uint32_t initialResidentDimCap = 512; // initial upload: coarsest base with long side <= cap
	uint32_t maxUpgradesPerFrame = 2;     // realloc pacing (each costs a decode+transcode+upload)
	uint32_t maxDowngradesPerFrame = 8;
	uint32_t evictCooldownFrames = 30;    // idle hysteresis before decaying to the floor
	float mipBias = 0.0f;                 // added to wanted mip (negative = sharper)
	// Retarget transcode POLICY (a Basis-family payload re-decoded at a
	// demand upgrade/downgrade):
	//   false (default; the shipped App + cooked paks): REFUSE + swap in
	//     the solid-pink error texture (entry pinned). A Basis payload in
	//     a streaming pak violates the v4 cook contract (the cook
	//     pink-embeds failed transcodes, so GPU-ready paks never hit
	//     this) — the error is REPORTED and the pink surface is the
	//     visible "recook the pak" flag. Never a hitch, never a crash.
	//   true (the editor/authoring hosts): TRANSCODE in place (the same
	//     Basis->BC7 the first load runs — see PrepareKtxStreamPlan) and
	//     proceed with the REAL content. Authoring clusters stash SOURCE
	//     bytes by design (a foreign Basis .ktx2 rides the pool verbatim
	//     until the cook converts it), so refusing would pink every
	//     legitimately-sourced texture on its first demand upgrade —
	//     correctness over frame cost in the editor.
	bool allowRetargetTranscode = false;
};

struct TextureStreamingStats {
	uint64_t fullBytes = 0;      // what full-residency would cost
	uint64_t residentBytes = 0;  // what is actually backed by VRAM
	uint32_t textureCount = 0;
	uint32_t fullResidentCount = 0; // textures currently at base 0
	uint32_t upgradesLastUpdate = 0;
	uint32_t downgradesLastUpdate = 0;
};

struct TextureStreamEntryDebug {
	uint32_t handleIndex = 0;
	uint32_t handleGen = 0;
	std::string name;
	uint32_t fullMips = 0;
	uint32_t residentBase = 0; // full-chain mip index backing image level 0
	uint32_t desiredBase = 0;  // feedback demand (min == sharpest)
	uint32_t residentWidth = 0;
	uint32_t residentHeight = 0;
	uint64_t residentBytes = 0;
	uint64_t fullBytes = 0;
	uint32_t lastRequestFrame = 0;
};

class TextureStreamer {
public:
	explicit TextureStreamer(ResourceManager& ctx, TextureStreamerConfig config = {});
	~TextureStreamer();

	TextureStreamer(const TextureStreamer&) = delete;
	TextureStreamer& operator=(const TextureStreamer&) = delete;

	// Pure tail-split (shared by StreamTexture and the pak cooker digest):
	// keep the coarsest minResidentMips mips resident at minimum, then walk
	// the base up while the resident-base extent still exceeds dimCap. The
	// cooker stamps tailFirstMip with these same numbers, so pass the
	// runtime config's values (defaults are the contract).
	static uint32_t InitialResidentBase(uint32_t fullMips, uint32_t baseLongSide,
		uint32_t minResidentMips, uint32_t dimCap);
	static uint32_t MaxResidentBase(uint32_t fullMips, uint32_t minResidentMips);

	// Decode + transcode + upload ONLY the initial tail of a KTX2 payload
	// and register it for streaming. The payload bytes must outlive the
	// registration (the glTF path keeps them in Cluster::textureSources;
	// call ReleaseTextureSources() only after the streamer is done).
	// Returns an empty holder on failure (error already logged).
	Holder<TextureHandle> StreamTexture(const void* payload, size_t payloadSize,
		const std::string& debugName, const ImageCreateInfo& imageCI = {});

	// Register an ALREADY-UPLOADED tail-resident texture for streaming
	// upgrades (the pak v4 fast-load path: objects + tail upload happen
	// through CreateStreamingTextureFromMeta, so no ktx object is parsed
	// here). payloadData/payloadSize are the FULL embedded ktx bytes the
	// demand upgrades transcode from — same lifetime contract as
	// StreamTexture (pak path borrows the loaded pak image, pinned by the
	// cluster). residentBase is the full-chain mip backing image level 0.
	// Returns false when the handle is already registered.
	bool RegisterExisting(TextureHandle handle,
		const void* payloadData, size_t payloadSize,
		const std::string& debugName, uint32_t fullMips, uint32_t baseWidth,
		uint32_t baseHeight, uint32_t arrayLayers, uint32_t residentBase);

	// Drop a texture from the budget (call before destroying its holder).
	void Unregister(TextureHandle handle);

	// Feedback input: full-chain mip index the texture needs this frame
	// (0 = full res). Takes the minimum across callers; consumed by
	// Update(), which then decays it back toward the floor.
	void RequestMip(TextureHandle handle, uint32_t wantedFullMip, uint32_t frameIndex);

	// GPU-feedback input: RESIDENT-relative LOD as reported by the shader
	// (0 = sharpest resident mip). Converted to a full-chain mip with the
	// entry's residentBase (resident level r == full mip residentBase + r)
	// and fed to RequestMip. Out-of-range reports clamp to the tail.
	void RequestMipFromLod(TextureHandle handle, uint32_t residentLod, uint32_t frameIndex);

	// Pure conversion behind RequestMipFromLod (device-free, unit-tested).
	static uint32_t ResolveFullMip(uint32_t residentBase, uint32_t fullMips, uint32_t reportedLod);

	// Batch demand from the scene pass: bindless slot -> closest camera
	// distance (meters) among this frame's instances sampling it. Entries
	// are matched by their CURRENT bindless slot (stable across retargets);
	// slots absent from the map get no request and decay via Update().
	// Textures with no reporting instance keep their floor residency.
	void RequestMipsByDistance(const std::unordered_map<uint32_t, float>& minDistBySlot,
		float viewportHeightPx, float fovYRad, uint32_t frameIndex,
		float approxWorldSize = 2.0f);

	// Bindless slot -> streamed TextureHandle snapshot for demand
	// consumers (slot-keyed GPU feedback). Slots without
	// a streamed texture hold an empty handle. Sized to the highest live
	// slot + 1 (may be shorter than the consume range; the consumer takes
	// the minimum).
	std::vector<TextureHandle> BuildBindlessSlotMap() const;

	// Per-slot meta for the gfx filter's draw-level demand publisher
	// (gfxfilter.cs.hlsl PublishDrawDemand): baseLongSide + fullMips per
	// bindless slot, same sizing rule as BuildBindlessSlotMap. Slots
	// without a streamed texture report fullMips == 0 (the shader skips
	// them). Slots are stable across retargets, so this only changes on
	// register/unregister — gate uploads on SlotMetaEpoch.
	struct TextureSlotMeta {
		uint32_t longSide = 0; // max(baseWidth, baseHeight) texels
		uint32_t fullMips = 0; // 0 = slot holds no streamed texture
	};
	std::vector<TextureSlotMeta> BuildSlotMeta() const;
	// Bumped on every register/unregister (NOT on retargets — the
	// slot->texture mapping is retarget-stable, only the slot set
	// changes). Consumers re-upload their meta table when this moves.
	uint64_t SlotMetaEpoch() const;

	// CPU heuristic: which full-chain mip satisfies `screenCoveragePx`
	// pixels of coverage for a texture with baseLongSide texels.
	// m = ceil(log2(base / coverage)) + bias, clamped to [0, fullMips).
	uint32_t WantedMipForScreenCoverage(uint32_t fullMips, uint32_t baseLongSide,
		float screenCoveragePx) const;
	static uint32_t WantedMipForScreenCoverage(uint32_t fullMips, uint32_t baseLongSide,
		float screenCoveragePx, float mipBias);

	// Distance-flavoured wrapper: coverage ~= viewportH * worldSize /
	// (2 * dist * tan(fovY/2)). worldSize is the approximate world-space
	// diameter the texture spans (default 2 m); per-texture bounds are a
	// future refinement, not a correctness issue (bias covers the error).
	uint32_t WantedMipForDistance(uint32_t fullMips, uint32_t baseLongSide,
		float distance, float viewportHeightPx, float fovYRad,
		float approxWorldSize = 2.0f) const;
	static uint32_t WantedMipForDistance(uint32_t fullMips, uint32_t baseLongSide,
		float distance, float viewportHeightPx, float fovYRad,
		float approxWorldSize, float mipBias);

	// Enforce the budget: upgrades toward demand, downgrades on pressure /
	// idle decay. Retarget failures are logged and skipped (never fatal).
	// Caller must FlushUploads() afterwards.
	void Update(uint32_t frameIndex);

	void SetConfig(const TextureStreamerConfig& config) { config_ = config; }
	const TextureStreamerConfig& GetConfig() const { return config_; }

	TextureStreamingStats GetStats() const;
	std::vector<TextureStreamEntryDebug> GetEntriesDebug() const;

	// BC7 block estimate (all engine KTX2 transcodes to BC7): 16 bytes per
	// 4x4 block per mip per layer, summed over [firstMip, firstMip+mipCount).
	static uint64_t EstimateResidentBytes(uint32_t baseWidth, uint32_t baseHeight,
		uint32_t firstMip, uint32_t mipCount, uint32_t arrayLayers = 1);

private:
	struct Entry {
		TextureHandle handle{};
		const void* payloadData = nullptr; // non-owning (see StreamTexture)
		size_t payloadSize = 0;
		std::string name;
		uint32_t fullMips = 1;
		uint32_t baseWidth = 1;
		uint32_t baseHeight = 1;
		uint32_t arrayLayers = 1;
		uint32_t residentBase = 0;
		uint32_t desiredBase = 0; // min demand since last Update
		uint32_t lastRequestFrame = 0;
		uint32_t lastChangeFrame = 0;
		bool everRequested = false;
		// Sticky error flag: this entry's payload was refused at a
		// retarget (a Basis-family payload in a streaming pak — the v4
		// cook contract violation) and its backing image was swapped to
		// the solid-pink error texture. Pinned entries never retarget
		// again (one pink swap per entry per run — the fix is recooking
		// the pak, not re-decoding the same broken payload).
		bool errorPinned = false;
	};

	static uint64_t EntryKey(TextureHandle h) {
		return (static_cast<uint64_t>(h.gen()) << 32) | h.index();
	}

	uint32_t initialBaseFor(uint32_t fullMips, uint32_t baseLongSide) const;
	uint32_t maxBaseFor(uint32_t fullMips) const; // coarsest base allowed by minResidentMips
	bool retarget(Entry& e, uint32_t newBase, uint32_t frameIndex);

	ResourceManager& ctx_;
	TextureStreamerConfig config_;
	std::unordered_map<uint64_t, Entry> entries_;
	// Guards entries_ across the parallel texture-load path (one shard per
	// worker, same contract as LoadKtxTexturesParallelMemory). Update() and
	// the frame loop never contend: loads join before frames start.
	mutable std::mutex mutex_;
	uint64_t slotMetaEpoch_ = 1; // bumps on register/unregister (see SlotMetaEpoch)
	uint32_t statsUpgrades_ = 0;
	uint32_t statsDowngrades_ = 0;
};

} // namespace smvk
