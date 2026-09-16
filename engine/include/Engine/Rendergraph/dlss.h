#pragma once
// DLSS Super Resolution via NVIDIA Streamline (external/streamline, v2.14.1).
//
// Streamline is loaded dynamically from sl.interposer.dll (staged next to
// App.exe); when it is missing the whole feature degrades silently to the
// renderScale/bilinear path. The interposer's vkGetInstanceProcAddr is
// routed through volk (App::Initialize), so instance/device/swapchain/
// acquire/present calls become SL proxies that keep the SDK informed
// without manual hooking (no slSetVulkanInfo / resource-state bookkeeping
// by hand).
//
// Pipeline position (HDR upscaling, before tonemap):
//   Main (HDR renderExtent) -> MotionBlur -> [DLSS: HDR render -> HDR swap]
//   -> Tonemap (swapExtent) -> UI composite
// Inputs per frame: HDR color (render res), depth (reversed-Z D32),
// motion vectors (RG16F prevUV-currUV, UV units, jittered), exposure 1x1.
// Output: HDR color at swap extent (Storage|Sampled target owned here).
//
// Conventions fed to SL constants (see ProgrammingGuideDLSS.md):
//   - matrices ROW-major, NO jitter (jitter is a separate pixel-space offset)
//   - motion vectors are UV deltas (prevUV - currUV, y-down): mvecScale {1,1}
//     maps UV to pixels inside the plugin (MV_Scale = mvecScale * renderSize)
//   - depthInverted (reversed-Z), cameraMotionIncluded, motionVectorsJittered
//
// Jitter: Halton(2,3) pattern in render pixels, phase count scaled by the
// output/render pixel-area ratio (DLSS programming guide 3.7.1.1).
//
// History reset: first DLSS frame, mode/extent change, deactivate, and
// !prevViewProjValid (the same gate the motion-vector pass uses) all set
// Constants::reset for one frame.
//
// Usage (app/src/scene_renderer.cpp):
//   smvk::DlssStartup() before volkInitialize / instance creation (engine)
//   ...
//   m_viewportResources.dlss.Ensure(ctx, rg, swapExtent, desc);
//   smvk::DlssApplyOptions(mode, swapW, swapH);        // on drift
//   smvk::AddDlssPasses(rg, ctx, dlss, frameInputs, frameExposure, "DLSS");
//   ... tonemap consumes dlss.output.logicalView / .bindlessIndex

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>

namespace smvk {

// Quality modes; ordering matches sl::DLSSMode (see DlssModeSl in dlss.cpp).
enum class DlssMode : int {
	Off = 0,
	MaxPerformance = 1,     // a.k.a. Ultra Performance
	Balanced = 2,
	MaxQuality = 3,         // a.k.a. Quality
	UltraPerformance = 4,
	UltraQuality = 5,
	DLAA = 6,
};

struct DlssOptimalSettings {
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
};

// ---------------------------------------------------------------------
// Backend entry points (implemented in engine/src/Renderer/dlss.cpp;
// the app never links or includes Streamline directly).
// ---------------------------------------------------------------------

// Loads sl.interposer.dll + slInit(kFeatureDLSS, eVulkan). Call once,
// BEFORE volkInitialize / VkInstance creation. Idempotent. Returns false
// (and leaves the app on the plain vulkan-1.dll path) when the DLL is
// missing, unsigned-load fails or slInit errors (no NVIDIA GPU etc.).
[[nodiscard]] bool DlssStartup();

// Process-wide kill switch for the Streamline load itself (App::Initialize
// consults it before DlssStartup / pointing volk at the interposer). Must
// be called before the renderer runs (before smvk::App::Initialize); after
// that, volk may already resolve through the interposer and the decision
// is locked in. With startup disabled the app talks straight to
// vulkan-1.dll — no SL layer in the call chain — which is what
// RenderDoc captures need (RenderDoc cannot replay through the SL
// interposer + NGX cubins and crashes on capture). Pair with
// DlssMode::Off so no feature evaluation happens either.
void DlssSetStartupEnabled(bool enabled);

// slShutdown before destroying the VkInstance/VkDevice (App::Shutdown).
// Keeps sl.interposer.dll mapped: volk and VMA hold pointers into the
// interposer that the Vulkan teardown (~Swapchain/~VulkanResourceManager/
// ~VulkanCore) still calls through — unloading it here crashes at exit
// (0xC0000005). Follow with DlssRelease() after the instance is gone.
void DlssShutdown();

// Unloads sl.interposer.dll. Must be the last Vulkan-related action of
// the process (end of ~VulkanCore): every volk/VMA function pointer
// resolves into the interposer until then. No-op without Streamline.
void DlssRelease();

// True between a successful DlssStartup and DlssShutdown.
[[nodiscard]] bool DlssAvailable();

// SL's vkGetInstanceProcAddr for volkInitializeCustom, or nullptr when
// Streamline is unavailable (caller then falls back to volkInitialize()).
[[nodiscard]] const void* DlssVulkanGetInstanceProcAddr();

// True when the loaded DLSS feature reports support on the current
// physical device (requires the device to exist). Sticky per run.
[[nodiscard]] bool DlssFeatureSupported();

// CPU-side query: optimal render resolution for (mode, output size).
[[nodiscard]] bool DlssQueryOptimalSettings(DlssMode mode,
	uint32_t outputWidth, uint32_t outputHeight, DlssOptimalSettings& out);

// slDLSSSetOptions + slAllocateResources for the single main viewport.
// Re-apply on mode/output drift; DlssDeactivate() when DLSS turns off
// (mode=eOff does NOT free plugin resources, slFreeResources does).
[[nodiscard]] bool DlssApplyOptions(DlssMode mode, uint32_t outputWidth, uint32_t outputHeight);
// Tears down the NGX feature instance for the single main viewport.
// outputWidth/Height must be the last applied output size (kept by the
// caller): the eOff SetOptions still validates extents, and a
// default-constructed options (INVALID_UINT extents) trips SL validation
// errors. The caller must ensure no DLSS evaluate is still in flight
// (e.g. WaitForDeviceIdle) before calling — freeing while prior frames
// are on the GPU loses the device and aborts in VK_CHECK.
void DlssDeactivate(uint32_t outputWidth, uint32_t outputHeight);

// ---------------------------------------------------------------------
// Jitter (pure CPU, headless-testable).
// ---------------------------------------------------------------------

// Van der Corput radical inverse (Halton basis), result in [0,1).
[[nodiscard]] float DlssHalton(uint32_t index, uint32_t radix);

// Total phases = base 8 * (output/render) pixel-area ratio (guide 3.7.1.1),
// clamped to [8, 128] and rounded. DLAA (1:1) lands on the base 8.
[[nodiscard]] uint32_t DlssPhaseCount(uint32_t renderWidth, uint32_t renderHeight,
	uint32_t outputWidth, uint32_t outputHeight);

struct DlssJitter {
	float x = 0.0f; // render-target pixels, y-down, range (-0.5, 0.5)
	float y = 0.0f;
};

// Halton(2,3) sample for `phase`, centered so the offset covers the pixel
// around the origin (DLSS jitter-offset range requirement: [-0.5, 0.5]).
[[nodiscard]] inline DlssJitter DlssComputeJitter(uint32_t phase, uint32_t phaseCount) {
	assert(phaseCount > 0);
	return {
		DlssHalton(phase + 1, 2) - 0.5f,
		DlssHalton(phase + 1, 3) - 0.5f,
	};
}

// ---------------------------------------------------------------------
// Targets (lifecycle pattern of SmaaImageTarget / HdrTarget).
// ---------------------------------------------------------------------

struct DlssImageTarget {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };
	Format format = Format::Undefined;

	[[nodiscard]] bool Valid() const { return !image.empty() && !logicalView.empty(); }

	// Storage: NGX writes the upscaled image as a storage resource
	// (tagged GENERAL); Sampled: tonemap reads it bindlessly;
	// ColorAttachment keeps the rendergraph barrier machinery happy on
	// first-use transitions from Undefined.
	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		Format targetFormat, const char* name, const char* viewName, const char* logicalName) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = targetFormat;
		ci.usage = ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::ColorAttachment;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, targetFormat, logicalName);
		if (newLogical.empty())
			return false;

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = targetExtent;
		format = targetFormat;
		return true;
	}

	void Destroy(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindlessIndex = UINT32_MAX;
		bindless = {};
		view = {};
		image = {};
		extent = { 0, 0 };
		format = Format::Undefined;
	}
};

struct DlssDesc {
	// HDR upscaling: the in/out color chain stays linear HDR (RGBA16F)
	// and the tonemapper consumes the upscaled result at swap extent.
	Format colorFormat = Format::RGBA16_Float;
	// 1x1 exposure scalar for NGX (kBufferTypeExposure); written each
	// frame by a 1x1 clear pass (FrameExposure, no auto-exposure mode).
	Format exposureFormat = Format::R32_Float;
};

struct Dlss {
	// Upscaled HDR output at swap extent (feeds the tonemapper).
	DlssImageTarget output;
	// 1x1 exposure scalar (R32F), cleared to the frame exposure.
	DlssImageTarget exposure;

	Extent2D extent{ 0, 0 };
	Format colorFormat = Format::RGBA16_Float;
	Format exposureFormat = Format::R32_Float;

	[[nodiscard]] bool Valid() const { return output.Valid() && exposure.Valid(); }

	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent, const DlssDesc& d = {}) {
		colorFormat = d.colorFormat;
		exposureFormat = d.exposureFormat;
		if (Valid() && extent.width == swapExtent.width && extent.height == swapExtent.height
			&& output.format == d.colorFormat && exposure.format == d.exposureFormat) {
			return true;
		}
		bool ok = true;
		ok = output.Create(ctx, rg, swapExtent, d.colorFormat,
			"dlssOutput", "dlssOutputView", "dlssOutputLogical") && ok;
		ok = exposure.Create(ctx, rg, { 1, 1 }, d.exposureFormat,
			"dlssExposure", "dlssExposureView", "dlssExposureLogical") && ok;
		extent = swapExtent;
		return ok;
	}

	void Destroy(RenderGraph& rg) {
		output.Destroy(rg);
		exposure.Destroy(rg);
		extent = { 0, 0 };
	}
};

// ---------------------------------------------------------------------
// Per-frame resource + constant bundle.
// ---------------------------------------------------------------------

// Matrices are ROW-major float[16] (SL convention) and UNJITTERED; the
// jitter goes to jitterOffset separately. The app fills them by
// transposing its glm column-major matrices.
struct DlssFrameInputs {
	// Raw physical handles (resolved to VkImage/VkImageView in dlss.cpp).
	ImageHandle colorInImage;
	ImageViewHandle colorInView;
	ImageHandle colorOutImage;
	ImageViewHandle colorOutView;
	ImageHandle depthImage;
	ImageViewHandle depthView;
	ImageHandle mvecImage;
	ImageViewHandle mvecView;
	ImageHandle exposureImage;
	ImageViewHandle exposureView;

	// Rendergraph logical views for pass ordering / layout barriers.
	LogicalImageViewHandle colorInLogical;
	LogicalImageViewHandle colorOutLogical;
	LogicalImageViewHandle depthLogical;
	LogicalImageViewHandle mvecLogical;
	LogicalImageViewHandle exposureLogical;

	Extent2D renderExtent{ 0, 0 };
	Extent2D outputExtent{ 0, 0 };
	Format colorFormat = Format::RGBA16_Float;
	Format depthFormat = Format::D32_Float;
	Format mvecFormat = Format::RG16_Float;
	Format exposureFormat = Format::R32_Float;

	// sl::Constants (see sl_consts.h).
	float cameraViewToClip[16]{};
	float clipToCameraView[16]{};
	float clipToPrevClip[16]{};
	float prevClipToClip[16]{};
	float jitterOffset[2]{ 0.0f, 0.0f };   // render px, y-down, [-0.5, 0.5]
	float cameraPos[3]{};
	float cameraUp[3]{};
	float cameraRight[3]{};
	float cameraFwd[3]{};
	float cameraNear = 0.0f;
	float cameraFar = 0.0f;
	float cameraFOV = 0.0f;                // radians (vertical)
	float cameraAspectRatio = 0.0f;
	bool reset = true;                     // invalidate history this frame
};

// Emits:
//   "DlssExposure"  1x1 graphics pass clearing the exposure scalar.
//   "DLSS"          compute-style pass recording slEvaluateFeature.
// frameIndex is the swapchain frame index (fb.frameIndex): used for the
// SL frame token so slSetConstants/slEvaluateFeature share one token.
// Returns false when the backend is unavailable (caller skips the pass
// and keeps its fallback upscale path).
[[nodiscard]] bool AddDlssPasses(RenderGraph& rg, ResourceManager& ctx,
	Dlss& dlss, const DlssFrameInputs& inputs, float exposureValue,
	uint32_t frameIndex, const char* name = nullptr);

} // namespace smvk
