#pragma once

// GPU readback texture-streaming feedback (Skull & Bones, GPU Zen 3).
//
// Stage 2 wiring (NOT yet connected – see below). This header defines the
// ABI and the CPU consume path so the shader side and the streamer agree:
//
//   * One uint per bindless texture slot (indexed by bindless index).
//   * 0xFFFFFFFF = "no pixel reported this texture this frame".
//   * Otherwise the MINIMUM wanted full-chain mip (0 = full res wins).
//   * Shaders publish with InterlockedMin (sharpest demand wins); see
//     engine/shaders/texture_streaming.hlsli (opt-in, no material includes
//     it yet).
//
// Remaining stage-2 work: a compute (or end-of-main-pass) clear of the
// buffer to 0xFF, binding slot 4 in the global set layout, one
// TS_ReportUsage() call per sampled texture in the materials, and a
// per-N-frames ConsumeReadback() from the frame loop. The v1 CPU heuristic
// (TextureStreamer::WantedMipFor*) drives residency until then.
//
// Threading: render thread only. ConsumeReadback() stalls on a GPU->CPU
// copy by design (use it every 4-8 frames, exactly like Skull & Bones).

#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>

#include <cstdint>
#include <vector>

namespace smvk {

class ResourceManager;
class TextureStreamer;

class TextureFeedback {
public:
	static constexpr uint32_t kNoReport = 0xFFFFFFFFu;

	explicit TextureFeedback(ResourceManager& ctx);
	~TextureFeedback();

	TextureFeedback(const TextureFeedback&) = delete;
	TextureFeedback& operator=(const TextureFeedback&) = delete;

	// (Re)allocates the GPU buffer for maxTextures slots. Cheap when the
	// size already matches. Must run on the render thread.
	void Ensure(uint32_t maxTextures);

	// CPU-side report (tools / tests / driver fallback): stages a demand
	// that the next ConsumeReadback() forwards. Same min-semantics as GPU.
	void ReportCpu(uint32_t bindlessSlot, uint32_t wantedFullMip);

	// Downloads the GPU buffer, forwards min-demands to the streamer as
	// RequestMip() calls, then re-arms the CPU mirror. `slotToHandle` maps
	// bindless slot -> TextureHandle (only streamed textures matter).
	// Returns the number of textures that reported. frameIndex is passed
	// through to RequestMip for the streamer's staleness tracking.
	// When `fromCpuMirrorOnly` is true, skips the GPU download (useful
	// before the stage-2 shader wiring exists).
	uint32_t ConsumeReadback(TextureStreamer& streamer,
		const std::vector<TextureHandle>& slotToHandle,
		uint32_t frameIndex, bool fromCpuMirrorOnly = true);

	BufferHandle GetBuffer() const { return buffer_; }
	uint32_t GetSlotCount() const { return slotCount_; }

private:
	ResourceManager& ctx_;
	BufferHandle buffer_{};
	uint32_t slotCount_ = 0;
	std::vector<uint32_t> cpuMirror_;
};

} // namespace smvk
