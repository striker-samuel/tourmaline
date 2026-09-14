// NVIDIA Streamline (DLSS-SR) backend - see Engine/Rendergraph/dlss.h.
//
// Everything Streamline-related lives behind the Dlss* free functions so
// neither the rendergraph headers nor the app ever include sl_*.h or
// link an import library. sl.interposer.dll is loaded dynamically from
// the executable directory; its vkGetInstanceProcAddr is handed to
// volkInitializeCustom (App::Initialize) so the regular - not manual -
// integration path is used: instance/device/swapchain/acquire/present
// proxies keep SL informed and manage tagged-resource states for us.

#include <Engine/Rendergraph/dlss.h>

#include "vulkan_impl.h" // VulkanResourceManager + Image/ImageView pools

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"

namespace {

// ---------------------------------------------------------------------
// Function table (resolved from sl.interposer.dll)
// ---------------------------------------------------------------------
HMODULE g_module = nullptr;
bool g_ready = false;         // slInit succeeded
bool g_supportedChecked = false;
bool g_supported = false;

PFun_slInit* g_slInit = nullptr;
PFun_slShutdown* g_slShutdown = nullptr;
PFun_slIsFeatureSupported* g_slIsFeatureSupported = nullptr;
PFun_slIsFeatureLoaded* g_slIsFeatureLoaded = nullptr;
PFun_slEvaluateFeature* g_slEvaluateFeature = nullptr;
PFun_slGetNewFrameToken* g_slGetNewFrameToken = nullptr;
PFun_slSetConstants* g_slSetConstants = nullptr;
PFun_slAllocateResources* g_slAllocateResources = nullptr;
PFun_slFreeResources* g_slFreeResources = nullptr;
PFun_slGetFeatureFunction* g_slGetFeatureFunction = nullptr;
PFN_vkGetInstanceProcAddr g_vkGetInstanceProcAddr = nullptr;

// Single main viewport shared by every DLSS call.
const sl::ViewportHandle kDlssViewport{ 1u };

void DlssLog(sl::LogType type, const char* msg) {
	std::fprintf(stderr, "[SL] %s%s",
		type == sl::LogType::eError ? "ERROR: " : (type == sl::LogType::eWarn ? "WARN: " : ""),
		msg ? msg : "");
}

template <typename T>
bool ResolveProc(const char* name, T*& out) {
	out = reinterpret_cast<T*>(reinterpret_cast<void*>(GetProcAddress(g_module, name)));
	if (!out) {
		std::fprintf(stderr, "[DLSS] sl.interposer.dll is missing export '%s'\n", name);
		return false;
	}
	return true;
}

[[nodiscard]] sl::DLSSMode ToSlMode(smvk::DlssMode mode) {
	// Ordering matches 1:1 (dlss.h DlssMode mirrors sl::DLSSMode).
	return static_cast<sl::DLSSMode>(static_cast<uint32_t>(mode));
}

[[nodiscard]] VkFormat ToVkFormat(smvk::Format f) {
	switch (f) {
	case smvk::Format::RGBA16_Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case smvk::Format::RG16_Float: return VK_FORMAT_R16G16_SFLOAT;
	case smvk::Format::R32_Float: return VK_FORMAT_R32_SFLOAT;
	case smvk::Format::D32_Float: return VK_FORMAT_D32_SFLOAT;
	// Scene depth is a stencil format (findDepthFormat prefers
	// D32_SFLOAT_S8_UINT, falls back to D24_UNORM_S8_UINT). The DLSS
	// plugin validates the tagged depth format — returning UNDEFINED
	// here trips "Cannot have undefined format" in SL and, in release
	// (assert compiled out), poisons the evaluate that later aborts
	// in VK_CHECK on toggle-off. Map the real formats through.
	case smvk::Format::D32_Float_S8_UInt: return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case smvk::Format::D24_UNorm_S8_UInt: return VK_FORMAT_D24_UNORM_S8_UINT;
	case smvk::Format::D16_UNorm: return VK_FORMAT_D16_UNORM;
	default: break;
	}
	assert(false && "ToVkFormat: format not handled by the DLSS module");
	return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] uint32_t ToVkUsage(smvk::ImageUsage u) {
	uint32_t vk = 0;
	if ((u & smvk::ImageUsage::TransferSrc) != smvk::ImageUsage::None)
		vk |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if ((u & smvk::ImageUsage::TransferDst) != smvk::ImageUsage::None)
		vk |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((u & smvk::ImageUsage::Sampled) != smvk::ImageUsage::None)
		vk |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if ((u & smvk::ImageUsage::Storage) != smvk::ImageUsage::None)
		vk |= VK_IMAGE_USAGE_STORAGE_BIT;
	if ((u & smvk::ImageUsage::ColorAttachment) != smvk::ImageUsage::None)
		vk |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((u & smvk::ImageUsage::DepthStencilAttachment) != smvk::ImageUsage::None)
		vk |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	return vk;
}

// Fills one sl::Resource + tag pair. `layout` must be the image layout at
// the time SL consumes the resource (the rendergraph barriers images to
// exactly these layouts via the pass image hints before recording).
struct TaggedResource {
	sl::Resource resource;
	sl::ResourceTag tag;
	sl::Extent extent;

	// depth: VkImageLayout DEPTH_STENCIL_READ_ONLY_OPTIMAL (the graph's
	// depth read promotion); everything else SHADER_READ_ONLY_OPTIMAL;
	// the storage output GENERAL.
	TaggedResource(smvk::Extent2D extent2d, smvk::Format format, smvk::ImageUsage usage,
		VkImageLayout layout, sl::BufferType type, sl::ResourceLifecycle lifecycle,
		VkImage vkImage, VkImageView vkView)
		: resource(sl::ResourceType::eTex2d,
			reinterpret_cast<void*>(vkImage),
			nullptr,
			reinterpret_cast<void*>(vkView),
			static_cast<uint32_t>(layout))
		, extent{ 0u, 0u, extent2d.width, extent2d.height }
	{
		// Body assignment: the tag copies `extent`, which is only fully
		// initialized after the member-init list ran (declaration order).
		resource.width = extent2d.width;
		resource.height = extent2d.height;
		resource.nativeFormat = ToVkFormat(format);
		resource.mipLevels = 1;
		resource.arrayLayers = 1;
		resource.usage = usage != smvk::ImageUsage::None
			? ToVkUsage(usage) : VK_IMAGE_USAGE_SAMPLED_BIT;
		tag.resource = &resource;
		tag.type = type;
		tag.lifecycle = lifecycle;
		tag.extent = extent;
	}
};

} // namespace

namespace smvk {

// ---------------------------------------------------------------------
// Backend bootstrap / teardown
// ---------------------------------------------------------------------

bool DlssStartup() {
	if (g_module)
		return g_ready;

	// Load from the executable directory (CMake stages the DLLs there).
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	char* lastSlash = std::strrchr(exePath, '\\');
	if (!lastSlash) {
		std::fprintf(stderr, "[DLSS] cannot locate the executable directory\n");
		return false;
	}
	*lastSlash = '\0';
	std::string dllPath = std::string(exePath) + "\\sl.interposer.dll";

	// Silent when the DLL is simply not installed (e.g. git checkout
	// without the vendored binaries): DLSS just stays unavailable.
	if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		return false;

	g_module = LoadLibraryA(dllPath.c_str());
	if (!g_module) {
		std::fprintf(stderr, "[DLSS] LoadLibrary('%s') failed (GetLastError=%lu)\n",
			dllPath.c_str(), GetLastError());
		return false;
	}

	if (!ResolveProc("slInit", g_slInit)
		|| !ResolveProc("slShutdown", g_slShutdown)
		|| !ResolveProc("slIsFeatureSupported", g_slIsFeatureSupported)
		|| !ResolveProc("slIsFeatureLoaded", g_slIsFeatureLoaded)
		|| !ResolveProc("slEvaluateFeature", g_slEvaluateFeature)
		|| !ResolveProc("slGetNewFrameToken", g_slGetNewFrameToken)
		|| !ResolveProc("slSetConstants", g_slSetConstants)
		|| !ResolveProc("slAllocateResources", g_slAllocateResources)
		|| !ResolveProc("slFreeResources", g_slFreeResources)
		|| !ResolveProc("slGetFeatureFunction", g_slGetFeatureFunction)
		|| !ResolveProc("vkGetInstanceProcAddr", g_vkGetInstanceProcAddr)) {
		FreeLibrary(g_module);
		g_module = nullptr;
		g_vkGetInstanceProcAddr = nullptr;
		return false;
	}

	sl::Preferences pref{};
	pref.showConsole = false;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.logMessageCallback = &DlssLog;
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
		| sl::PreferenceFlags::eAllowOTA
		| sl::PreferenceFlags::eLoadDownloadedPlugins;
	const sl::Feature features[] = { sl::kFeatureDLSS };
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = 1;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	// NGX validates this as a GUID string (an arbitrary project name is
	// rejected with "invalid character" and NGX init fails outright).
	pref.projectId = "6f6c1b2e-9d34-4a51-8e0f-2b7c9a3d5e14";
	pref.renderAPI = sl::RenderAPI::eVulkan;

	sl::Result res = g_slInit(pref, sl::kSDKVersion);
	if (res != sl::Result::eOk) {
		// No NVIDIA driver / NGX unsupported: expected on non-NVDA
		// systems, log once and stay on the fallback path.
		std::fprintf(stderr, "[DLSS] slInit failed (result=%u) - DLSS unavailable, "
			"falling back to the native path\n", static_cast<uint32_t>(res));
		FreeLibrary(g_module);
		g_module = nullptr;
		g_vkGetInstanceProcAddr = nullptr;
		return false;
	}
	std::fprintf(stderr, "[DLSS] Streamline initialized (DLSS feature requested)\n");
	g_ready = true;
	return true;
}

void DlssShutdown() {
	if (!g_module)
		return;
	if (g_ready) {
		sl::Result res = g_slShutdown();
		if (res != sl::Result::eOk)
			std::fprintf(stderr, "[DLSS] slShutdown failed (result=%u)\n", static_cast<uint32_t>(res));
		g_ready = false;
	}
	// Deliberately NOT FreeLibrary-ing sl.interposer.dll here: volk's
	// dispatch tables and VMA's imported function table hold pointers
	// into the interposer (App::Initialize routed volk through its
	// vkGetInstanceProcAddr), and every remaining Vulkan teardown call
	// (~Swapchain / ~VulkanResourceManager / ~VulkanCore) runs through
	// them. Unloading now unmaps that code and the next vk* call jumps
	// into freed memory (0xC0000005 at app exit). DlssRelease() unloads
	// the module once the last Vulkan call has happened.
	g_vkGetInstanceProcAddr = nullptr;
	g_supported = false;
	g_supportedChecked = false;
}

void DlssRelease() {
	if (!g_module)
		return;
	// Only safe after the final Vulkan call of the process (end of
	// ~VulkanCore): see the note in DlssShutdown. No-op when Streamline
	// never loaded or was already released.
	FreeLibrary(g_module);
	g_module = nullptr;
}

bool DlssAvailable() {
	return g_ready;
}

const void* DlssVulkanGetInstanceProcAddr() {
	return reinterpret_cast<const void*>(g_vkGetInstanceProcAddr);
}

bool DlssFeatureSupported() {
	if (!g_ready || g_supportedChecked)
		return g_supported;
	g_supportedChecked = true;

	bool loaded = false;
	sl::Result res = g_slIsFeatureLoaded(sl::kFeatureDLSS, loaded);
	if (res != sl::Result::eOk || !loaded) {
		std::fprintf(stderr, "[DLSS] feature not loaded (result=%u loaded=%d)\n",
			static_cast<uint32_t>(res), loaded ? 1 : 0);
		return g_supported = false;
	}

	VkPhysicalDevice pd = GetVkPhysicalDevice();
	if (pd == VK_NULL_HANDLE) {
		std::fprintf(stderr, "[DLSS] no VkPhysicalDevice yet - support check deferred\n");
		g_supportedChecked = false; // re-check once the device exists
		return g_supported = false;
	}
	sl::AdapterInfo info{};
	info.vkPhysicalDevice = reinterpret_cast<void*>(pd);
	res = g_slIsFeatureSupported(sl::kFeatureDLSS, info);
	if (res != sl::Result::eOk) {
		std::fprintf(stderr, "[DLSS] not supported on this adapter (result=%u: "
			"driver out of date / non-NVIDIA GPU / OS check)\n", static_cast<uint32_t>(res));
		return g_supported = false;
	}
	std::fprintf(stderr, "[DLSS] supported on this adapter\n");
	return g_supported = true;
}

bool DlssQueryOptimalSettings(DlssMode mode,
	uint32_t outputWidth, uint32_t outputHeight, DlssOptimalSettings& out) {
	if (!g_ready || mode == DlssMode::Off
		|| outputWidth == 0 || outputHeight == 0)
		return false;

	PFun_slDLSSGetOptimalSettings* fn = nullptr;
	sl::Result res = g_slGetFeatureFunction(sl::kFeatureDLSS,
		"slDLSSGetOptimalSettings", reinterpret_cast<void*&>(fn));
	if (res != sl::Result::eOk || !fn) {
		std::fprintf(stderr, "[DLSS] slDLSSGetOptimalSettings unavailable (result=%u)\n",
			static_cast<uint32_t>(res));
		return false;
	}
	sl::DLSSOptions o{};
	o.mode = ToSlMode(mode);
	o.outputWidth = outputWidth;
	o.outputHeight = outputHeight;
	sl::DLSSOptimalSettings settings{};
	if (SL_FAILED(res, fn(o, settings)))
		return false;
	out.renderWidth = settings.optimalRenderWidth;
	out.renderHeight = settings.optimalRenderHeight;
	return out.renderWidth != 0 && out.renderHeight != 0;
}

bool DlssApplyOptions(DlssMode mode, uint32_t outputWidth, uint32_t outputHeight) {
	if (!g_ready)
		return false;
	if (mode == DlssMode::Off || outputWidth == 0 || outputHeight == 0)
		return false;

	PFun_slDLSSSetOptions* fn = nullptr;
	sl::Result res = g_slGetFeatureFunction(sl::kFeatureDLSS,
		"slDLSSSetOptions", reinterpret_cast<void*&>(fn));
	if (res != sl::Result::eOk || !fn) {
		std::fprintf(stderr, "[DLSS] slDLSSSetOptions unavailable (result=%u)\n",
			static_cast<uint32_t>(res));
		return false;
	}
	sl::DLSSOptions o{};
	o.mode = ToSlMode(mode);
	o.outputWidth = outputWidth;
	o.outputHeight = outputHeight;
	// HDR pipeline: linear RGBA16F in/out, real exposure texture tagged
	// every frame (useAutoExposure stays off - see AddDlssPasses).
	o.colorBuffersHDR = sl::Boolean::eTrue;
	o.useAutoExposure = sl::Boolean::eFalse;
	o.alphaUpscalingEnabled = sl::Boolean::eFalse;
	if (SL_FAILED(res, fn(kDlssViewport, o))) {
		std::fprintf(stderr, "[DLSS] slDLSSSetOptions failed (result=%u)\n",
			static_cast<uint32_t>(res));
		return false;
	}
	// No explicit slAllocateResources here: the DLSS plugin's allocate
	// path runs the same begin-event as evaluate and needs the common
	// constants + tagged resources, which only exist at evaluate time.
	// The NGX feature instance is instead created by the first
	// slEvaluateFeature (ProgrammingGuideDLSS 8.0: "If slAllocateResources
	// is not called DLSS resources would be initialized at this point").
	return true;
}

void DlssDeactivate(uint32_t outputWidth, uint32_t outputHeight) {
	if (!g_ready)
		return;
	PFun_slDLSSSetOptions* setFn = nullptr;
	if (g_slGetFeatureFunction(sl::kFeatureDLSS,
			"slDLSSSetOptions", reinterpret_cast<void*&>(setFn)) == sl::Result::eOk && setFn) {
		sl::DLSSOptions o{};
		o.mode = sl::DLSSMode::eOff;
		// eOff still validates extents: forward the last applied output
		// size instead of the INVALID_UINT defaults. Zero means unknown
		// (never applied) — leave the defaults in that case.
		if (outputWidth != 0 && outputHeight != 0) {
			o.outputWidth = outputWidth;
			o.outputHeight = outputHeight;
		}
		// Keep the HDR/auto-exposure/alpha fields consistent with the
		// enabled path so the transition doesn't look like a format drift.
		o.colorBuffersHDR = sl::Boolean::eTrue;
		o.useAutoExposure = sl::Boolean::eFalse;
		o.alphaUpscalingEnabled = sl::Boolean::eFalse;
		sl::Result setRes = setFn(kDlssViewport, o);
		if (setRes != sl::Result::eOk)
			std::fprintf(stderr, "[DLSS] slDLSSSetOptions(eOff) failed (result=%u)\n",
				static_cast<uint32_t>(setRes));
	}
	sl::Result res = g_slFreeResources(sl::kFeatureDLSS, kDlssViewport);
	if (res != sl::Result::eOk)
		std::fprintf(stderr, "[DLSS] slFreeResources failed (result=%u)\n",
			static_cast<uint32_t>(res));
}

// ---------------------------------------------------------------------
// Jitter helpers (pure CPU)
// ---------------------------------------------------------------------

float DlssHalton(uint32_t index, uint32_t radix) {
	// Van der Corput radical inverse; index >= 1.
	float f = 1.0f;
	float r = 0.0f;
	uint32_t i = index;
	while (i > 0) {
		f /= static_cast<float>(radix);
		r += f * static_cast<float>(i % radix);
		i /= radix;
	}
	return r;
}

uint32_t DlssPhaseCount(uint32_t renderWidth, uint32_t renderHeight,
	uint32_t outputWidth, uint32_t outputHeight) {
	if (renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0)
		return 8;
	const double renderPixels = double(renderWidth) * double(renderHeight);
	const double outputPixels = double(outputWidth) * double(outputHeight);
	const double ratio2 = outputPixels / renderPixels; // output/render pixel area
	const uint32_t scaled = uint32_t(8.0 * ratio2 + 0.5);
	return (scaled < 8u) ? 8u : (scaled > 128u ? 128u : scaled);
}

// ---------------------------------------------------------------------
// Per-frame evaluate
// ---------------------------------------------------------------------

namespace {

bool DlssEvaluate(ResourceManager& ctx, void* vkCommandBuffer,
	uint32_t frameIndex, const DlssFrameInputs& in) {
	assert(g_ready);
	assert(vkCommandBuffer);
	VkCommandBuffer vkCmd = reinterpret_cast<VkCommandBuffer>(vkCommandBuffer);

	auto& vctx = *static_cast<VulkanResourceManager*>(ctx.GetVulkanOpaque());
	auto vkImage = [&vctx](ImageHandle h) {
		auto* img = vctx.images_.get(h);
		assert(img && img->vkImage != VK_NULL_HANDLE);
		return img->vkImage;
	};
	auto vkView = [&vctx](ImageViewHandle h) {
		auto* v = vctx.imageViews_.get(h);
		assert(v && v->vkView != VK_NULL_HANDLE);
		return v->vkView;
	};
	auto usageOf = [&vctx](ImageHandle h) {
		auto* img = vctx.images_.get(h);
		return img ? img->usage : ImageUsage::None;
	};

	// Lifecycles follow the DLSS programming guide's tagging example:
	// depth lives until present, the volatile per-frame inputs are
	// consumed immediately at evaluate.
	TaggedResource colorIn(
		in.renderExtent, in.colorFormat, usageOf(in.colorInImage),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow,
		vkImage(in.colorInImage), vkView(in.colorInView));
	TaggedResource colorOut(
		in.outputExtent, in.colorFormat, usageOf(in.colorOutImage),
		VK_IMAGE_LAYOUT_GENERAL,
		sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow,
		vkImage(in.colorOutImage), vkView(in.colorOutView));
	TaggedResource depth(
		in.renderExtent, in.depthFormat, usageOf(in.depthImage),
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
		vkImage(in.depthImage), vkView(in.depthView));
	TaggedResource mvec(
		in.renderExtent, in.mvecFormat, usageOf(in.mvecImage),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
		vkImage(in.mvecImage), vkView(in.mvecView));
	TaggedResource exposure(
		Extent2D{ 1u, 1u }, in.exposureFormat, usageOf(in.exposureImage),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		sl::kBufferTypeExposure, sl::ResourceLifecycle::eOnlyValidNow,
		vkImage(in.exposureImage), vkView(in.exposureView));

	// Common constants - matrices arrive row-major + unjittered from the
	// app; the 16-float arrays share the sl::float4x4 row-major layout.
	sl::Constants consts{};
	std::memcpy(&consts.cameraViewToClip, in.cameraViewToClip, sizeof(float) * 16);
	std::memcpy(&consts.clipToCameraView, in.clipToCameraView, sizeof(float) * 16);
	std::memcpy(&consts.clipToPrevClip, in.clipToPrevClip, sizeof(float) * 16);
	std::memcpy(&consts.prevClipToClip, in.prevClipToClip, sizeof(float) * 16);
	consts.jitterOffset = sl::float2(in.jitterOffset[0], in.jitterOffset[1]);
	// Motion vectors are UV deltas (prevUV - currUV, y-down): identity
	// scale maps UV -> pixels inside the plugin (MV_Scale = mvecScale *
	// renderSize).
	consts.mvecScale = sl::float2(1.0f, 1.0f);
	consts.cameraPos = sl::float3(in.cameraPos[0], in.cameraPos[1], in.cameraPos[2]);
	consts.cameraUp = sl::float3(in.cameraUp[0], in.cameraUp[1], in.cameraUp[2]);
	consts.cameraRight = sl::float3(in.cameraRight[0], in.cameraRight[1], in.cameraRight[2]);
	consts.cameraFwd = sl::float3(in.cameraFwd[0], in.cameraFwd[1], in.cameraFwd[2]);
	consts.cameraNear = in.cameraNear;
	consts.cameraFar = in.cameraFar;
	consts.cameraFOV = in.cameraFOV;
	consts.cameraAspectRatio = in.cameraAspectRatio;
	// Reversed-Z (near maps to 1), geometry-based vectors carry camera
	// motion and include the sub-pixel jitter (prevViewProj is jittered).
	consts.depthInverted = sl::Boolean::eTrue;
	consts.cameraMotionIncluded = sl::Boolean::eTrue;
	consts.motionVectors3D = sl::Boolean::eFalse;
	consts.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	consts.orthographicProjection = sl::Boolean::eFalse;
	consts.motionVectorsDilated = sl::Boolean::eFalse;
	consts.motionVectorsJittered = sl::Boolean::eTrue;

	sl::FrameToken* token = nullptr;
	sl::Result res = g_slGetNewFrameToken(token, &frameIndex);
	if (res != sl::Result::eOk || !token) {
		std::fprintf(stderr, "[DLSS] slGetNewFrameToken failed (result=%u)\n",
			static_cast<uint32_t>(res));
		return false;
	}
	if (SL_FAILED(res, g_slSetConstants(consts, *token, kDlssViewport))) {
		std::fprintf(stderr, "[DLSS] slSetConstants failed (result=%u)\n",
			static_cast<uint32_t>(res));
		return false;
	}

	// One-shot call: viewport + local tags consumed at this evaluate.
	const sl::BaseStructure* inputs[] = {
		&kDlssViewport,
		&colorIn.tag,
		&colorOut.tag,
		&depth.tag,
		&mvec.tag,
		&exposure.tag,
	};
	if (SL_FAILED(res, g_slEvaluateFeature(sl::kFeatureDLSS, *token,
			inputs, uint32_t(std::size(inputs)),
			reinterpret_cast<sl::CommandBuffer*>(vkCmd)))) {
		std::fprintf(stderr, "[DLSS] slEvaluateFeature failed (result=%u)\n",
			static_cast<uint32_t>(res));
		return false;
	}
	// Throttled heartbeat: positive proof the evaluate path is live
	// (first record + every 600th), mirroring the exposure meter's tick.
	{
		static uint64_t s_evalTick = 0;
		const uint64_t tick = s_evalTick++;
		if (tick < 3 || tick % 600 == 0) {
			std::fprintf(stderr,
				"[DLSS] evaluate tick=%llu %ux%u -> %ux%u jitter=(%.3f,%.3f) reset=%d\n",
				(unsigned long long)tick,
				in.renderExtent.width, in.renderExtent.height,
				in.outputExtent.width, in.outputExtent.height,
				in.jitterOffset[0], in.jitterOffset[1], in.reset ? 1 : 0);
		}
	}
	return true;
}

} // namespace

bool AddDlssPasses(RenderGraph& rg, ResourceManager& ctx,
	Dlss& dlss, const DlssFrameInputs& inputs, float exposureValue,
	uint32_t frameIndex, const char* name) {
	if (!g_ready)
		return false;
	assert(dlss.Valid());
	assert(!inputs.colorInLogical.empty());
	assert(!inputs.colorOutLogical.empty());
	assert(!inputs.depthLogical.empty());
	assert(!inputs.mvecLogical.empty());
	assert(!inputs.exposureLogical.empty());
	assert(inputs.renderExtent.width != 0 && inputs.renderExtent.height != 0);
	assert(inputs.outputExtent.width != 0 && inputs.outputExtent.height != 0);

	// 1) Exposure scalar: 1x1 clear (the value is CPU-known this frame -
	//    the meter downloads with a 1-2 frame lag, see FrameExposure).
	{
		RenderPass& pass = rg.AddPass("DlssExposure");
		pass.SetOffset(0, 0)
			.SetExtent({ 1, 1 })
			.SetColor({ dlss.exposure.logicalView },
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearColor(exposureValue, 0.0f, 0.0f, 0.0f));
		pass.SetRecordingFunc([](CommandBuffer&) {});
	}

	// 2) DLSS evaluate. Compute-style pass: no attachments, the recording
	//    func hands the raw VkCommandBuffer to slEvaluateFeature which
	//    records its own barriers + dispatches (SL restores every tagged
	//    resource to the layout it was tagged in, matching the graph's
	//    committed states).
	{
		RenderPass& pass = rg.AddComputePass(name ? name : "DLSS");
		pass.SetOffset(0, 0)
			.SetExtent(inputs.outputExtent);

		ImageSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = DerivePipelineStage(read.barrierMasks.accessMask);
		read.layout = ImageLayout::ShaderReadOnlyOptimal;
		pass.AddImageHint(inputs.colorInLogical, read);
		pass.AddImageHint(inputs.mvecLogical, read);
		pass.AddImageHint(inputs.exposureLogical, read);
		// Depth: requested as sampled; CollectPassUsages promotes depth
		// formats to DepthStencilReadOnlyOptimal, which is exactly the
		// layout the depth tag declares below.
		pass.AddImageHint(inputs.depthLogical, read);

		ImageSyncRequest write{};
		write.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		write.barrierMasks.stageMask = DerivePipelineStage(write.barrierMasks.accessMask);
		write.layout = ImageLayout::General;
		pass.AddImageHint(inputs.colorOutLogical, write);

		DlssFrameInputs captured = inputs; // handles are copyable PODs
		ResourceManager* ctxPtr = &ctx;
		pass.SetRecordingFunc([ctxPtr, captured, frameIndex](CommandBuffer& cmd) {
			// Runs after the barrier batch the hints above produced;
			// slEvaluateFeature records its own barriers + dispatches and
			// restores tagged resources to their tagged layouts.
			void* vkCmd = cmd.GetNativeHandle();
			DlssEvaluate(*ctxPtr, vkCmd, frameIndex, captured);
		});
	}
	return true;
}

} // namespace smvk
