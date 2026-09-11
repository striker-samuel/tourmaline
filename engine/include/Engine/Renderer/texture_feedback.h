#pragma once

// GPU readback texture-streaming feedback (Skull & Bones, GPU Zen 3).
//
// Pixel shaders report the mip they actually sampled per texture into a
// device buffer (see engine/shaders/texture_streaming.hlsli); the CPU reads
// it back every N frames and the streamer upgrades/downgrades residency.
//
// ABI (shader <-> CPU, must match texture_streaming.hlsli):
//   * One uint per bindless texture slot (indexed by bindless index).
//   * 0xFFFFFFFF = "no pixel reported this texture this frame".
//   * Otherwise the MINIMUM resident-relative LOD (0 = the sharpest
//     resident mip wins). Shaders publish with InterlockedMin; the CPU adds
//     the texture's residentBase (TextureStreamer::RequestMipFromLod) to
//     recover the full-chain mip. Reporting resident-relative keeps the
//     shader free of per-texture metadata (no extra buffer).
//
// Cadence (mirrors the exposure meter, same stall-free reasoning):
//   * Buffers are ringed per flight (owned by the RHI, always bound).
//   * Every record: consume the RETIRED slice first (it holds data from
//     two frames ago — Acquire retired the slot, no wait), then clear the
//     current slice to 0xFF for this frame's writes. Download-before-clear
//     order is load-bearing (see exposure.h).
//   * Consume runs every 4th frame; the clear runs every frame (a stale
//     min would pin textures sharp forever).
//
// Readback sharing: on consume frames the feedback download is batched
// with the exposure histogram download into ONE submit + wait (see
// ResourceManager::DownloadDataFromBuffers) instead of two.
//
// Threading: render thread only.

#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smvk {

class ResourceManager;
class TextureStreamer;

class TextureFeedback {
public:
	static constexpr uint32_t kNoReport = 0xFFFFFFFFu;
	// Consume cadence: every Nth frame (de-phased from the CPU demand
	// pass so the two feedback sources interleave smoothly).
	static constexpr uint32_t kConsumeInterval = 4;

	explicit TextureFeedback(ResourceManager& ctx);
	~TextureFeedback();

	TextureFeedback(const TextureFeedback&) = delete;
	TextureFeedback& operator=(const TextureFeedback&) = delete;

	// Caps the consumed slot range (RHI buffers always hold MAX_TEXTURES;
	// smaller scenes download less). Must run on the render thread.
	void SetSlotCount(uint32_t maxTextures);

	// Clears the current flight slice to kNoReport for this frame's
	// writes. Call every record AFTER ConsumeAndForward.
	void ClearForFrame(uint32_t flightSlot);

	// Downloads the retired flight slice, merges with CPU reports
	// (min-wins in the resident-lod domain) and forwards demands to the
	// streamer. Returns the number of textures that reported.
	uint32_t ConsumeAndForward(TextureStreamer& streamer,
		uint32_t flightSlot, uint32_t streamFrame);

	// Split form of ConsumeAndForward for batched readbacks: the caller
	// downloads the slice itself (e.g. in one submit with the exposure
	// histogram via DownloadDataFromBuffers) and forwards the bytes here.
	// Returns false (forwarding skipped) when the slice is unusable.
	bool DownloadSlice(std::vector<uint32_t>& out, uint32_t flightSlot) const;
	uint32_t ForwardDownloaded(TextureStreamer& streamer,
		const uint32_t* gpu, size_t gpuCount, uint32_t streamFrame);

	// CPU-side report in the resident-lod domain (tools / tests / driver
	// fallback): staged until the next ConsumeAndForward. Same
	// min-semantics as the GPU path.
	void ReportCpu(uint32_t bindlessSlot, uint32_t residentLod);

	uint32_t GetSlotCount() const { return slotCount_; }
	uint32_t GetLastReported() const { return lastReported_; }

private:
	ResourceManager& ctx_;
	uint32_t slotCount_ = 0;
	std::vector<uint32_t> cpuMirror_;
	uint32_t lastReported_ = 0;
};

} // namespace smvk
