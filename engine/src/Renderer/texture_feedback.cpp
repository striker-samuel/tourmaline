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

void TextureFeedback::SetSlotCount(uint32_t maxTextures)
{
	slotCount_ = maxTextures;
	cpuMirror_.assign(maxTextures, kNoReport);
	lastReported_ = 0;
}

void TextureFeedback::ClearForFrame(uint32_t flightSlot)
{
	if (slotCount_ == 0)
		return;
	// 0xFF fill of the whole slice (RHI buffers always hold MAX_TEXTURES
	// slots; clearing the full slice keeps the upload size constant and
	// the sync pattern identical to the exposure zero-fill). Same upload
	// path as all per-frame buffers, so the transfer->fragment acquire
	// orders the clear before this frame's Main writes.
	static std::vector<uint32_t> s_clear;
	if (s_clear.empty())
		s_clear.assign(MAX_TEXTURES, kNoReport);
	BufferHandle slice = ctx_.GetTextureFeedbackBuffer(flightSlot);
	if (slice.empty())
		return;
	ctx_.UploadDataToBuffer(slice, s_clear.data(),
		s_clear.size() * sizeof(uint32_t),
		PipelineStageBit_Fragment,
		AccessFlags::ShaderWrite, 0, 0);
}

void TextureFeedback::ReportCpu(uint32_t bindlessSlot, uint32_t residentLod)
{
	if (bindlessSlot >= cpuMirror_.size())
		return;
	cpuMirror_[bindlessSlot] =
		std::min(cpuMirror_[bindlessSlot], residentLod);
}

uint32_t TextureFeedback::ConsumeAndForward(TextureStreamer& streamer,
	uint32_t flightSlot, uint32_t streamFrame)
{
	std::vector<uint32_t> gpu;
	if (!DownloadSlice(gpu, flightSlot))
		return 0;
	return ForwardDownloaded(streamer, gpu.data(), gpu.size(), streamFrame);
}

bool TextureFeedback::DownloadSlice(std::vector<uint32_t>& out, uint32_t flightSlot) const
{
	if (slotCount_ == 0)
		return false;
	out.assign(slotCount_, kNoReport);
	BufferHandle slice = ctx_.GetTextureFeedbackBuffer(flightSlot);
	if (slice.empty())
		return false;
	return ctx_.DownloadDataFromBuffer(slice, out.data(),
		out.size() * sizeof(uint32_t));
}

uint32_t TextureFeedback::ForwardDownloaded(TextureStreamer& streamer,
	const uint32_t* gpu, size_t gpuCount, uint32_t streamFrame)
{
	lastReported_ = 0;
	if (gpu == nullptr || gpuCount == 0 || slotCount_ == 0)
		return 0;
	const std::vector<TextureHandle> slotMap = streamer.BuildBindlessSlotMap();
	uint32_t n = static_cast<uint32_t>(std::min<size_t>(gpuCount, slotMap.size()));
	n = std::min(n, slotCount_);
	uint32_t reported = 0;
	for (uint32_t slot = 0; slot < n; ++slot)
	{
		if (!slotMap[slot].valid())
			continue;
		const uint32_t lod = std::min(gpu[slot], cpuMirror_[slot]);
		if (lod == kNoReport)
			continue;
		streamer.RequestMipFromLod(slotMap[slot], lod, streamFrame);
		++reported;
	}

	std::fill(cpuMirror_.begin(), cpuMirror_.end(), kNoReport);
	lastReported_ = reported;
	return reported;
}

} // namespace smvk
