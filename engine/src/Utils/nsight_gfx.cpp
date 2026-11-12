// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Utils/nsight_gfx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The whole body is gated: shipping/custom builds compile an empty object so
// the engine never depends on the vendored NVIDIA headers unless the
// diagnostics profile explicitly opts in (see docs/nsight_profiles.md).
#ifdef TOURMALINE_NGFX_SDK

#include <NGFX_GPUTrace_Vulkan.h>
#include <NGFX_GraphicsCapture_Vulkan.h>
#include <NGFX_Vulkan.h>

namespace tour::nsight_gfx {
namespace {

enum class Activity { None, GraphicsCapture, GpuTrace };

Activity g_activity = Activity::None;
bool g_initialized = false;
bool g_boundaryEnabled = false;

uint64_t g_captureFrame = 0; // 0 = none; else request a 1-frame capture here
uint64_t g_traceStart = 0;   // 0 = none
uint64_t g_traceEnd = 0;     // 0 = none
bool g_tracing = false;

uint64_t ReadFramePin(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v)
        return 0;
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v)
        return 0;
    return uint64_t(n);
}

bool InitActivity(Activity activity) {
    if (activity == Activity::GraphicsCapture) {
        NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params params = {
            NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params_VER};
        const NGFX_Result r = NGFX_GraphicsCapture_InitializeActivity_Vulkan(&params);
        if (r != NGFX_Result_Success) {
            std::fprintf(
                stderr, "[nsight_gfx] Graphics Capture init: %d (unattached run?)\n", int(r));
            return false;
        }
        std::fprintf(stderr, "[nsight_gfx] Graphics Capture activity ready\n");
        return true;
    }
    if (activity == Activity::GpuTrace) {
        NGFX_GPUTrace_InitializeActivity_Vulkan_Params params = {
            NGFX_GPUTrace_InitializeActivity_Vulkan_Params_VER};
        const NGFX_Result r = NGFX_GPUTrace_InitializeActivity_Vulkan(&params);
        if (r != NGFX_Result_Success) {
            std::fprintf(stderr, "[nsight_gfx] GPU Trace init: %d (unattached run?)\n", int(r));
            return false;
        }
        std::fprintf(stderr, "[nsight_gfx] GPU Trace activity ready\n");
        return true;
    }
    return false;
}

} // namespace

bool Initialize() {
    if (g_initialized)
        return g_activity != Activity::None || g_boundaryEnabled;
    g_initialized = true;

    // Required before any injection/activity use. NoVerification is the
    // documented dev/diagnostics loader (never use in production).
    NGFX_SetLibraryLoadFn(NGFX_LoadLib_NoVerification);

    g_captureFrame = ReadFramePin("TOURMALINE_NGFX_CAPTURE_FRAME");
    g_traceStart = ReadFramePin("TOURMALINE_NGFX_TRACE_START");
    g_traceEnd = ReadFramePin("TOURMALINE_NGFX_TRACE_END");

    // Exactly ONE activity per process (the SDK contract). Prefer capture,
    // then trace.
    if (g_captureFrame != 0) {
        g_activity =
            InitActivity(Activity::GraphicsCapture) ? Activity::GraphicsCapture : Activity::None;
    } else if (g_traceStart != 0) {
        g_activity = InitActivity(Activity::GpuTrace) ? Activity::GpuTrace : Activity::None;
    } else {
        g_activity = Activity::None;
    }
    // Frame boundaries are opt-in (headless / non-present workloads); the
    // App presents, so capture/trace delimit on Present instead.
    g_boundaryEnabled = ReadFramePin("TOURMALINE_NGFX_BOUNDARY") != 0;
    return true;
}

bool Available() {
    return true;
}

void FrameBoundary(void* vkQueue) {
    if (!g_initialized || !g_boundaryEnabled)
        return;
    NGFX_FrameBoundary_Vulkan_Params params = {NGFX_FrameBoundary_Vulkan_Params_VER};
    params.queue = static_cast<VkQueue>(vkQueue);
    NGFX_FrameBoundary_Vulkan(&params);
}

void Poll(uint64_t frameIndex) {
    if (!g_initialized || frameIndex == 0)
        return;
    if (g_activity == Activity::GraphicsCapture && frameIndex == g_captureFrame) {
        NGFX_GraphicsCapture_RequestCapture_Vulkan_Params params = {
            NGFX_GraphicsCapture_RequestCapture_Vulkan_Params_VER};
        params.delimiter = NGFX_GraphicsCapture_Delimiter_Present;
        params.framesBeforeStart = 0;
        params.framesToCapture = 1;
        const NGFX_Result r = NGFX_GraphicsCapture_RequestCapture_Vulkan(&params);
        std::fprintf(stderr,
                     "[nsight_gfx] RequestCapture @frame %llu: %d\n",
                     (unsigned long long)frameIndex,
                     int(r));
        g_captureFrame = 0; // one-shot
        return;
    }
    if (g_activity == Activity::GpuTrace) {
        if (!g_tracing && frameIndex == g_traceStart) {
            NGFX_GPUTrace_StartTrace_Vulkan_Params params = {
                NGFX_GPUTrace_StartTrace_Vulkan_Params_VER};
            const NGFX_Result r = NGFX_GPUTrace_StartTrace_Vulkan(&params);
            std::fprintf(stderr,
                         "[nsight_gfx] StartTrace @frame %llu: %d\n",
                         (unsigned long long)frameIndex,
                         int(r));
            g_tracing = (r == NGFX_Result_Success);
        } else if (g_tracing && frameIndex == g_traceEnd) {
            NGFX_GPUTrace_StopTrace_Vulkan_Params params = {
                NGFX_GPUTrace_StopTrace_Vulkan_Params_VER};
            params.flags = NGFX_GPUTrace_StopTraceFlag_None;
            const NGFX_Result r = NGFX_GPUTrace_StopTrace_Vulkan(&params);
            std::fprintf(stderr,
                         "[nsight_gfx] StopTrace @frame %llu: %d\n",
                         (unsigned long long)frameIndex,
                         int(r));
            g_tracing = false;
        }
    }
}

} // namespace tour::nsight_gfx

#else // TOURMALINE_NGFX_SDK

namespace tour::nsight_gfx {
bool Initialize() {
    return false;
}
bool Available() {
    return false;
}
void FrameBoundary(void*) {}
void Poll(uint64_t) {}
} // namespace tour::nsight_gfx

#endif // TOURMALINE_NGFX_SDK
