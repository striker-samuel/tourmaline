#include <Engine/Renderer/texture_feedback.h>
#include <Engine/Renderer/texture_streamer.h>
#include <Engine/RHI/resource_manager.h>

#include <algorithm>

namespace smvk {

TextureFeedback::TextureFeedback(ResourceManager& ctx)
	: ctx_(ctx)
{
}

TextureFeedback::~TextureFeedback() = default;

void TextureFeedback::Ensure(uint32_t maxTextures)
{
	if (maxTextures == 0)
		return;
	if (maxTextures == slotCount_ && buffer_.valid())
		return;
	// Storage buffer: one uint demand per bindless slot. Written by GPU
	// atomics (stage 2), read back here every N frames.
	Holder<BufferHandle> holder = ctx_.CreateBuffer(
		static_cast<size_t>(maxTextures) * sizeof(uint32_t),
		BufferType_Storage, "TextureFeedback");
	if (!holder.valid())
		return;
	buffer_ = holder.release();
	slotCount_ = maxTextures;
	cpuMirror_.assign(maxTextures, kNoReport);
}

void TextureFeedback::ReportCpu(uint32_t bindlessSlot, uint32_t wantedFullMip)
{
	if (bindlessSlot >= cpuMirror_.size())
		return;
	cpuMirror_[bindlessSlot] =
		std::min(cpuMirror_[bindlessSlot], wantedFullMip);
}

uint32_t TextureFeedback::ConsumeReadback(TextureStreamer& streamer,
	const std::vector<TextureHandle>& slotToHandle,
	uint32_t frameIndex, bool fromCpuMirrorOnly)
{
	std::vector<uint32_t> demands = cpuMirror_;

	if (!fromCpuMirrorOnly && buffer_.valid() && slotCount_ > 0)
	{
		// GPU snapshot (stalls; call every 4-8 frames). The shader side
		// writes MIN wanted mips, so a plain min-merge with the CPU mirror
		// preserves "sharpest demand wins" across both sources.
		std::vector<uint32_t> gpu(slotCount_, kNoReport);
		if (ctx_.DownloadDataFromBuffer(buffer_, gpu.data(),
			gpu.size() * sizeof(uint32_t)))
		{
			for (uint32_t i = 0; i < slotCount_; ++i)
				demands[i] = std::min(demands[i], gpu[i]);
		}
	}

	uint32_t reported = 0;
	const uint32_t n = std::min<uint32_t>(slotCount_,
		static_cast<uint32_t>(slotToHandle.size()));
	for (uint32_t slot = 0; slot < n; ++slot)
	{
		const uint32_t want = demands[slot];
		if (want == kNoReport || !slotToHandle[slot].valid())
			continue;
		streamer.RequestMip(slotToHandle[slot], want, frameIndex);
		++reported;
	}

	std::fill(cpuMirror_.begin(), cpuMirror_.end(), kNoReport);
	return reported;
}

} // namespace smvk
