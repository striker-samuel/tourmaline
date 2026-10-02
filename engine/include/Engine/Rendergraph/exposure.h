#pragma once
// Histogram-based auto-exposure metering (CPU eye adaptation).
//
// Ports the pattern used by ForwardPlus / BloomChain into one place:
// resource creation (histogram storage buffer + rendergraph alias) +
// default compute program ownership + rendergraph pass setup. The HDR
// source is sampled bindless (textures[] indexed by push), barriers come
// from AddImageHint (read) + AddBufferHint (write).
//
// Pipeline (see app/src/world_renderer.cpp):
//   1. Per frame, the CPU downloads the flight slice's 64-bin buffer FIRST
//      (this slot retired at Acquire, so the bins are the completed
//      accumulation ~2 frames old — no stall), adapts the smoothed
//      exposure, then zeroes the slice via UploadDataToBuffer
//      (TransferWrite; the compute hint below promotes it to Compute R/W).
//      The download MUST precede the zero-fill and the main submit: Execute
//      only records into the frame command buffer (submitted later in
//      EndAndPresent), so a post-Execute download's copy runs BEFORE the
//      histogram compute and observes only zeros. The buffer is ringed per
//      (like the gfx filter's batch/draw/indirect slices): the zero-fill
//      is an immediate transfer-queue submit at record time, so a single
//      shared buffer would let frame N+1's zero clobber frame N's still
//      accumulating bins once the pipeline fills (startup lockstep works,
//      then the meter goes quiet) — per-flight slices make the clobber
//      structurally impossible.
//   2. ExposureHistogram compute (16x16 threads/group, sampleStep stride)
//      builds a log2-luminance histogram of the (possibly motion-blurred)
//      scene HDR into the slice via groupshared combine + global atomics.
//   3. At the next frame's record time, the app downloads the slice's
//      256-byte buffer (DownloadDataFromBuffer; the slot retired at Acquire
//      so no extra stall — natural ~2-frame lag), finds the percentile luminance on the CPU and exponentially
//      damps the smoothed exposure toward keyValue * 2^evBias / meteredLum.
//   4. The smoothed exposure feeds the tonemap push next frame, and the
//      bloom threshold/knee are scaled exposure-relative so bloom strength
//      stays constant as exposure shifts.
//
// Usage:
//   smvk::ExposureMeter meter;
//   meter.Ensure(ctx, rg);                                        // once
//   meter.AddHistogramPass(rg, hdrLogical, hdrBindless, linearExtent...);
//   ... after Execute: meter.Download + Adapt on the CPU (app side).

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace smvk {

// Fixed bin count: must match EXPOSURE_BINS in exposure_histogram.cs.hlsl
// and the groupshared tileBins[64] there.
inline constexpr uint32_t kExposureBinCount = 64;
inline constexpr size_t kExposureHistogramBytes =
	sizeof(uint32_t) * kExposureBinCount;

struct ExposureDesc {
	uint32_t bins = kExposureBinCount; // <= kExposureBinCount (shader cap)
	float minLogLum = -10.0f;          // log2 luminance of bin 0
	float maxLogLum = 12.0f;           // log2 luminance of bin N (exclusive)
	uint32_t sampleStep = 4;           // sample every Nth pixel per axis
	float keyValue = 0.18f;            // target middle-grey luminance
	float percentile = 0.6f;           // histogram percentile driving exposure
	const char* name = "exposure";
};

// Must match exposure_histogram.cs.hlsl ExposureHistogramPush (40 bytes).
struct ExposureHistogramPush {
	uint64_t histogramAddress = 0; // BDA of ExposureHistogramBuffer
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t width = 0;
	uint32_t height = 0;
	float minLogLum = -10.0f;
	float maxLogLum = 12.0f;
	uint32_t binCount = kExposureBinCount;
	uint32_t sampleStep = 4;
	uint32_t pad = 0;
};
static_assert(sizeof(ExposureHistogramPush) == 40);

// Default SPV (histogram builder CS).
inline constexpr const char* kExposureHistogramCsPath =
	"content/shaders/exposure_histogram.cs.hlsl.spv";

struct ExposureMeter {
	// Histogram storage, ringed per frame in flight (see pipeline note
	// above): frames overlap (the swapchain only retires the same slot),
	// and the zero-fill is an immediate transfer submit at record time, so
	// any single-copy buffer would be clobbered by the next frame while the
	// current one still accumulates. Same rule as the gfx filter's per-view
	// ring slices and RenderViewSet's slab.
	Holder<BufferHandle> histogramBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle histogramLogicals[MAX_FRAMES_IN_FLIGHT]{};
	ExposureDesc desc{};

	Holder<ShaderModuleHandle> histogramModule;
	ShaderProgram histogramProgram{};

	// Last downloaded bins (CPU copy for adaptation + ImGui plot).
	std::array<uint32_t, kExposureBinCount> lastBins{};
	uint32_t lastTotal = 0;

	[[nodiscard]] bool Valid() const {
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			if (histogramBuffers[f].empty() || histogramLogicals[f].empty())
				return false;
		}
		return true;
	}
	[[nodiscard]] BufferHandle HistogramBuffer(uint32_t frameIndex) const {
		assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
		return histogramBuffers[frameIndex];
	}
	[[nodiscard]] LogicalBufferHandle HistogramLogical(uint32_t frameIndex) const {
		assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
		return histogramLogicals[frameIndex];
	}
	[[nodiscard]] bool ShadersValid() const {
		return histogramProgram.shaderCount != 0 && !histogramModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* csPath = kExposureHistogramCsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string cs = csPath ? csPath : kExposureHistogramCsPath;
			if (histogramModule.empty())
				histogramModule = ctx.CreateShaderModule(
					readFile(cs), "exposureHistogramCS");
		} catch (...) {
			return false;
		}
		if (histogramModule.empty())
			return false;
		Shader cs{
			.module = histogramModule,
			.entryPoint = "mainCS",
			.stage = ShaderStage::Compute };
		histogramProgram = { .shaders = { cs }, .shaderCount = 1 };
		return true;
	}

	void DestroyShaders() {
		histogramProgram = ShaderProgram{};
		histogramModule = {};
	}

	// Extent-independent (each slice is always 64 uints); creates all
	// flight slices once.
	bool Create(ResourceManager& ctx, RenderGraph& rg, const ExposureDesc& d,
		const char* csPath = kExposureHistogramCsPath) {
		if (d.bins == 0 || d.bins > kExposureBinCount)
			return false;
		Destroy(rg);
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			Holder<BufferHandle> buf = ctx.CreateBuffer(
				kExposureHistogramBytes, BufferType_Storage, "exposure histogram");
			if (buf.empty()) {
				Destroy(rg);
				return false;
			}
			LogicalBufferHandle logical = rg.AddExternalBuffer(buf, "exposure histogram");
			if (logical.empty()) {
				Destroy(rg);
				return false;
			}
			histogramBuffers[f] = std::move(buf);
			histogramLogicals[f] = logical;
			// No zero-fill here: every slice is zeroed at record time
			// before its frame's histogram pass runs (same ordering
			// argument as the ds-counters Create zero in
			// deferredshadow.h); a Create-time copy would join the same
			// transfer batch and trip sync validation the same way.
		}
		desc = d;
		// Zero so the first adaptation (before any GPU write lands) sees
		// total == 0 and holds the seed exposure instead of garbage.
		lastBins.fill(0);
		lastTotal = 0;
		return EnsureShaders(ctx, csPath);
	}

	void Destroy(RenderGraph& rg) {
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			if (!histogramLogicals[f].empty())
				rg.FreeBuffer(histogramLogicals[f]);
			histogramLogicals[f] = {};
			histogramBuffers[f] = {};
		}
		lastBins.fill(0);
		lastTotal = 0;
	}

	bool Ensure(ResourceManager& ctx, RenderGraph& rg, const ExposureDesc& d,
		const char* csPath = kExposureHistogramCsPath) {
		if (Valid()) {
			desc = d; // live-update metering range without realloc
			return EnsureShaders(ctx, csPath);
		}
		return Create(ctx, rg, d, csPath);
	}

	uint64_t HistogramAddress(ResourceManager& ctx, uint32_t frameIndex) const {
		return ctx.GetBufferDeviceAddress(HistogramBuffer(frameIndex));
	}

	// Dispatch dimensions for the current desc/extent (16x16 threads,
	// each covering a sampleStep-spaced pixel).
	static uint32_t DispatchXFor(Extent2D extent, uint32_t sampleStep) {
		const uint32_t tile = 16 * (sampleStep == 0 ? 4 : sampleStep);
		return (extent.width + tile - 1) / tile;
	}
	static uint32_t DispatchYFor(Extent2D extent, uint32_t sampleStep) {
		const uint32_t tile = 16 * (sampleStep == 0 ? 4 : sampleStep);
		return (extent.height + tile - 1) / tile;
	}

	// Compute pass shell for one flight slice: caller zeroes the slice
	// first (CPU upload above), then sets the recording func (bind program,
	// push, dispatch). Captures only copyable handles/values, safe once
	// this returns.
	RenderPass& AddHistogramPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		const ExposureHistogramPush& push,
		uint32_t frameIndex,
		const char* name = "ExposureHistogram") const {
		assert(Valid());
		assert(!hdrView.empty());
		assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
		RenderPass& pass = rg.AddComputePass(name);
		ImageSyncRequest hdrReq{};
		hdrReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		hdrReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		hdrReq.layout = ImageLayout::ShaderReadOnlyOptimal;
		pass.AddImageHint(hdrView, hdrReq);
		BufferSyncRequest histReq{};
		histReq.barrierMasks.accessMask =
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		histReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(histogramLogicals[frameIndex], histReq);
		// Recording func is set by the caller (needs program + dispatch).
		(void)push;
		return pass;
	}

	// ---- CPU adaptation ----
	// Percentile luminance of the last downloaded histogram (exposure-
	// independent: the HDR buffer is pre-exposure). Returns <= 0 when the
	// histogram is empty (caller must hold the current exposure).
	[[nodiscard]] float PercentileLuminance() const {
		if (lastTotal == 0)
			return 0.0f;
		const uint32_t bins = desc.bins == 0 ? kExposureBinCount : desc.bins;
		const uint64_t want = static_cast<uint64_t>(
			static_cast<double>(lastTotal) * static_cast<double>(desc.percentile));
		uint64_t accum = 0;
		for (uint32_t i = 0; i < bins; ++i) {
			accum += lastBins[i];
			if (accum >= want) {
				const float t =
					(static_cast<float>(i) + 0.5f) / static_cast<float>(bins);
				const float logLum =
					desc.minLogLum + t * (desc.maxLogLum - desc.minLogLum);
				return exp2f(logLum);
			}
		}
		return exp2f(desc.maxLogLum);
	}

	// Damped exposure step: target = key * 2^evBias / meteredLum, clamped,
	// approached exponentially with separate to-dark / to-bright rates.
	// Returns the new smoothed exposure (== current when no measurement).
	[[nodiscard]] float Adapt(float currentExposure, float evBias,
		float speedToDark, float speedToBright, float minExposure,
		float maxExposure, float dt) const {
		const float metered = PercentileLuminance();
		if (!(metered > 0.0f) || !(dt > 0.0f))
			return currentExposure;
		float target = desc.keyValue * exp2f(evBias) / metered;
		target = target < minExposure ? minExposure : target;
		target = target > maxExposure ? maxExposure : target;
		const float rate = target > currentExposure ? speedToDark : speedToBright;
		const float blend = 1.0f - expf(-rate * dt);
		return currentExposure + (target - currentExposure) * blend;
	}
};

} // namespace smvk
