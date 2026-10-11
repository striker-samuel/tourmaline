#pragma once
// Central Optick integration for VulkanToy.
//
// Usage (CPU):
//   void Foo() { SMVK_PROFILE_FUNCTION(); ... }                       // auto name, default color
//   { SMVK_PROFILE_SCOPE_COLOR("ShadowPass", Optick::Color::Orange); } // custom color
//   { SMVK_PROFILE_CATEGORY("UpdateCamera", Optick::Category::GameLogic); }
//
// Usage (frame):
//   while (...) { SMVK_PROFILE_FRAME("MainThread"); ... }
//
// Usage (GPU, inside RenderGraph::Execute where a VkCommandBuffer is live):
//   SMVK_PROFILE_GPU_CONTEXT(cmd);
//   { SMVK_PROFILE_GPU_EVENT("Main"); ... }
//
// When Optick is disabled (ENABLE_OPTICK=OFF -> USE_OPTICK=0) every OPTICK_*
// macro below compiles to a no-op, so these wrappers stay zero-cost.
#include <optick.h>

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
// Per-frame marker. Call once at the top of the main loop.
#define SMVK_PROFILE_FRAME(NAME) OPTICK_FRAME(NAME)

// Function scope with automatic (__FUNCSIG__/__PRETTY_FUNCTION__) name.
#define SMVK_PROFILE_FUNCTION() OPTICK_EVENT()

// Named CPU scope, default color.
#define SMVK_PROFILE_SCOPE(NAME) OPTICK_EVENT(NAME)

// Named CPU scope with explicit Optick::Color (e.g. Optick::Color::OrangeRed).
// Encodes the color through a Rendering-filter category so the timeline shows it.
#if USE_OPTICK
#define SMVK_PROFILE_SCOPE_COLOR(NAME, COLOR) \
    OPTICK_EVENT(NAME, OPTICK_MAKE_CATEGORY(Optick::Filter::Rendering, COLOR))
#else
#define SMVK_PROFILE_SCOPE_COLOR(NAME, COLOR) OPTICK_EVENT(NAME)
#endif

// Named scope with a predefined Optick::Category (carries its own color).
// e.g. Optick::Category::Rendering / GameLogic / IO / Streaming / UI / ...
#if USE_OPTICK
#define SMVK_PROFILE_CATEGORY(NAME, CATEGORY) OPTICK_CATEGORY(NAME, CATEGORY)
#else
#define SMVK_PROFILE_CATEGORY(NAME, CATEGORY) OPTICK_EVENT(NAME)
#endif

// Dynamic (run-time string) scope, for quick prototyping.
#define SMVK_PROFILE_SCOPE_DYNAMIC(NAME) OPTICK_EVENT_DYNAMIC(NAME)

// Attach a tag (int/float/string/...) to the current scope.
#define SMVK_PROFILE_TAG(NAME, ...) OPTICK_TAG(NAME, __VA_ARGS__)

// Name the current thread in the profiler timeline.
#define SMVK_PROFILE_THREAD(NAME) OPTICK_THREAD(NAME)

// ---------------------------------------------------------------------------
// GPU (Vulkan)
// ---------------------------------------------------------------------------
#define SMVK_PROFILE_GPU_CONTEXT(CMD_BUFFER) OPTICK_GPU_CONTEXT(CMD_BUFFER)
#define SMVK_PROFILE_GPU_EVENT(NAME) OPTICK_GPU_EVENT(NAME)
#define SMVK_PROFILE_GPU_FLIP(SWAPCHAIN) OPTICK_GPU_FLIP(SWAPCHAIN)

// ---------------------------------------------------------------------------
// Color-coded shortcuts for hot engine systems
// ---------------------------------------------------------------------------
#define SMVK_PROFILE_RENDER(NAME)  SMVK_PROFILE_CATEGORY(NAME, Optick::Category::Rendering)
#define SMVK_PROFILE_SCENE(NAME)   SMVK_PROFILE_CATEGORY(NAME, Optick::Category::Scene)
#define SMVK_PROFILE_IO(NAME)      SMVK_PROFILE_CATEGORY(NAME, Optick::Category::IO)
#define SMVK_PROFILE_STREAM(NAME)  SMVK_PROFILE_CATEGORY(NAME, Optick::Category::Streaming)
#define SMVK_PROFILE_GAMELOGIC(NAME) SMVK_PROFILE_CATEGORY(NAME, Optick::Category::GameLogic)
#define SMVK_PROFILE_UI(NAME)      SMVK_PROFILE_CATEGORY(NAME, Optick::Category::UI)
#define SMVK_PROFILE_GPU_SCENE(NAME)   OPTICK_GPU_EVENT(NAME)
#define SMVK_PROFILE_GPU_SHADOW(NAME)  OPTICK_GPU_EVENT(NAME)
#define SMVK_PROFILE_GPU_POSTFX(NAME)  OPTICK_GPU_EVENT(NAME)

namespace smvk::profiler {

// One-shot GPU init after VkDevice + queues exist (volk must be loaded).
// Safe to call once; subsequent calls are ignored. No-op when USE_OPTICK==0.
void InitVulkanGPU();

// Tear down Optick state (call after vkDeviceWaitIdle, before vkDestroyDevice).
void Shutdown();

// Runtime gates (App flags --use_optick / --use_optick_gpu). Default: both
// off. The GPU profiler's fence-based flip interleaves its own command
// buffer submits with the frame — RenderDoc captures break with them in the
// mix, so Optick is opt-in now. --use_optick gates the CPU capture session
// (bench.opt); --use_optick_gpu additionally brings up the GPU timelines
// (query pools + per-flip submits) and implies --use_optick.
void SetEnabled(bool enabled);
bool Enabled();
void SetGpuEnabled(bool enabled);
bool GpuEnabled();

// ---------------------------------------------------------------------------
// Frame-time histogram (live distribution over the last frames)
// ---------------------------------------------------------------------------
// Opt-in live frame-time distribution for the App (the F12 shortcut) and
// the editor (View menu / the same F12). DISABLED =
// RecordFrameTime() is a single compare-and-return — zero cost; ENABLED
// costs a few ns/frame on fixed storage (no allocation, no locks; the main
// thread is the only writer/reader). While an Optick capture runs, frame
// ms ride along as a tag on the DrawFrame event. (Re)enabling starts a
// fresh history.
void SetFrameHistogramEnabled(bool enabled);
bool FrameHistogramEnabled();
// Records one frame — hooked at the two once-per-frame UpdateDeltaTime
// call sites (the WorldRenderer element; the editor host records itsd;
// EditorApp::DrawFrame). No-op while disabled.
void RecordFrameTime(double milliseconds);
struct FrameHistogramData {
    // Bucket i covers [i*0.5, (i+1)*0.5) ms; the last is the >= 16 ms tail.
    static constexpr size_t kBucketCount = 33;
    size_t count = 0;   // recorded frames (capped at the ring size, 1024)
    double avgMs = 0.0;
    double maxMs = 0.0; // since the (re)enable
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    uint32_t buckets[kBucketCount] = {};
};
FrameHistogramData GetFrameHistogramData(); // by value (UI-side, ~1/frame)
// ImGui: bucket bars + avg/p50/p95/p99/max line. Shared by the App's debug
// panel + the editor's overlay; call only while enabled.
void DrawFrameHistogramUi();

#if USE_OPTICK
// RAII CPU+GPU scope for run-time pass names (e.g. rendergraph passes).
// Static OPTICK_GPU_EVENT can't be used in a loop (description is cached per
// __LINE__), so this resolves a shared description per name instead.
struct GpuPassScope {
    ::Optick::Event cpuEvent;
    ::Optick::GPUEvent gpuEvent;
    GpuPassScope(const char* name, const char* file, int line)
        : cpuEvent(*::Optick::EventDescription::CreateShared(name, file, line))
        , gpuEvent(*::Optick::EventDescription::CreateShared(name, file, line)) {}
};
#else
struct GpuPassScope {
    GpuPassScope(const char*, const char*, int) {}
};
#endif

} // namespace smvk::profiler

// Dynamic CPU+GPU scope for run-time strings (rendergraph pass names, ...).
// Safe when Optick is disabled (expands to a no-op scope object).
#define SMVK_CONCAT_IMPL(x, y) x##y
#define SMVK_CONCAT(x, y) SMVK_CONCAT_IMPL(x, y)
#define SMVK_PROFILE_GPU_PASS(NAME) \
    ::smvk::profiler::GpuPassScope SMVK_CONCAT(_smvk_gpu_pass_, __LINE__)(NAME, __FILE__, __LINE__)
