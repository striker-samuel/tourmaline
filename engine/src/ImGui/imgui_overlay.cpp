// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Backend-neutral: every graphics-backend call goes through the ui:: seam
// (imgui_overlay.h). The SDL3 PLATFORM backend (events/windowing) is
// graphics-API-neutral and stays here; its graphics pairing is set up by
// ui::InitBackend.

#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/platform.h>
#include <Engine/Profile/profile.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cstdio>

namespace smvk {

ImGuiOverlay::~ImGuiOverlay() {
    if (initialized_)
        Shutdown();
}

ImGuiOverlay::ImGuiOverlay(ImGuiOverlay&& o) noexcept {
    *this = std::move(o);
}
ImGuiOverlay& ImGuiOverlay::operator=(ImGuiOverlay&& o) noexcept {
    if (this != &o) {
        if (initialized_) Shutdown();
        initialized_ = o.initialized_;
        window_ = o.window_;
        imguiCtx_ = o.imguiCtx_;
        ctx_ = o.ctx_;
        swapchain_ = o.swapchain_;
        desc_ = o.desc_;
        appliedDpiScale_ = o.appliedDpiScale_;
        o.initialized_ = false;
        o.window_ = nullptr;
        o.imguiCtx_ = nullptr;
        o.ctx_ = nullptr;
        o.swapchain_ = nullptr;
        o.appliedDpiScale_ = 0.0f;
    }
    return *this;
}

bool ImGuiOverlay::Init(ResourceManager& ctx, Swapchain& swapchain, SDL_Window* window,
    const ImGuiOverlayDesc& desc) {
    if (initialized_) return true;
    ctx_ = &ctx;
    swapchain_ = &swapchain;
    window_ = window;
    desc_ = desc;

    // ---- Create ImGui context ----
    IMGUI_CHECKVERSION();
    ImGuiContext* imguiCtx = ImGui::CreateContext();
    if (!imguiCtx) {
        fprintf(stderr, "[ImGuiOverlay] Failed to create ImGui context\n");
        return false;
    }
    imguiCtx_ = imguiCtx;
    ImGui::SetCurrentContext(imguiCtx);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // The HOST owns the keyboard (editor modal keys, placement-graph Del).
    // With nav-capture on (the default), io.WantCaptureKeyboard reads true
    // whenever any window is focused (io.NavActive), which would starve
    // every app-side key handler — see UpdateModalInput's guard, which
    // means to test "an ImGui input is active (typing/dragging) or a
    // modal is open", not "a window has focus".
    io.ConfigNavCaptureKeyboard = false;
    // Enable docking / viewports optionally:
    // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // ---- Init the backend seam ----
    // The paired graphics + SDL3 platform init lives behind ui::InitBackend
    // (the overlay never touches a backend API directly). App::PollEvents
    // forwards every SDL event to ImGui via ProcessEvent (see
    // imguiEventHook_), so no install-callbacks step is needed.
    ui::UiBackendDesc backendDesc{};
    backendDesc.colorFormat = desc_.colorFormat != Format::Undefined
        ? desc_.colorFormat
        : swapchain.GetSurfaceFormat();
    backendDesc.minImageCount = swapchain.GetMinImageCount();
    backendDesc.imageCount = swapchain.GetImageCount();
    // The window is not optional: the SDL3 platform backend owns
    // io.DisplaySize from it every frame (see the UiBackendDesc comment).
    backendDesc.window = window;
    if (!ui::InitBackend(ctx, swapchain, backendDesc)) {
        fprintf(stderr, "[ImGuiOverlay] ui::InitBackend failed (unsupported "
                        "or misconfigured backend)\n");
        return false;
    }

    // (The descriptor pool + the graphics backend init + the paired SDL3
    // platform init all live in ui::InitBackend — the seam owns every
    // backend-native step, including the format the overlay resolved
    // into backendDesc above.)

    initialized_ = true;

    // ---- Font + DPI (after initialized_: ApplyDpiScale needs the context) ----
    // Fonts: ImGui 1.92 dynamic font system – glyphs rasterize on demand at
    // base * FontScaleDpi and the Vulkan backend uploads dirty atlas textures
    // automatically (ImGuiBackendFlags_RendererHasTextures), so no manual
    // texture upload is needed at Init or when the DPI scale changes.
    if (desc_.fontPath) {
        try {
            readFile(desc_.fontPath); // verify readable (loader reads lazily)
            ImFont* font = io.Fonts->AddFontFromFileTTF(desc_.fontPath, desc_.fontBaseSize);
            if (!font)
                fprintf(stderr, "[ImGuiOverlay] AddFontFromFileTTF failed – using default font\n");
        } catch (...) {
            fprintf(stderr, "[ImGuiOverlay] Font '%s' unreadable – using default font\n",
                desc_.fontPath ? desc_.fontPath : "(null)");
        }
    }
    ApplyDpiScale(desc_.dpiScale > 0.0f ? desc_.dpiScale : GetWindowDpiScale());

    return true;
}

void ImGuiOverlay::Shutdown() {
    if (!initialized_) return;
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    // The backend seam owns the wait-idle + the graphics backend shutdown
    // + its pool/heap teardown + the paired SDL3 platform shutdown.
    ui::ShutdownBackend(*ctx_);
    ImGui::DestroyContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));

    imguiCtx_ = nullptr;
    initialized_ = false;
    ctx_ = nullptr;
    swapchain_ = nullptr;
    appliedDpiScale_ = 0.0f;
}

void ImGuiOverlay::NewFrame(Extent2D targetExtent) {
    if (!initialized_) return;
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    ui::NewFrameBackend();
    ImGui_ImplSDL3_NewFrame();
    // Map ImGui screen-space draw data onto the actual render target size
    // (separated UI pass target != window size when ui scale != 1 or on
    // HiDPI). 1.92 also uses this as the font rasterizer density.
    ImGuiIO& io = ImGui::GetIO();
    if (targetExtent.width > 0 && targetExtent.height > 0 &&
        io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        io.DisplayFramebufferScale = ImVec2(
            static_cast<float>(targetExtent.width) / io.DisplaySize.x,
            static_cast<float>(targetExtent.height) / io.DisplaySize.y);
    }
    ImGui::NewFrame();
}

void ImGuiOverlay::Render(CommandBuffer& cmd) {
    if (!initialized_) return;
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));

    // If the app didn't call ImGui::Render() after building UI, do it now.
    // ImGui::GetDrawData() returns nullptr before Render().
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData) {
        ImGui::Render();
        drawData = ImGui::GetDrawData();
    } else if (drawData && drawData->CmdListsCount > 0) {
        // Already rendered – nothing extra.
    } else {
        // Edge: no UI was built but NewFrame was called – still need Render().
        // Check if we are between NewFrame and Render: ImGui::GetCurrentContext()->WithinFrameScope
        // Simplest: if Valid but not yet rendered, call Render if not already.
        // ImGui doesn't expose "needs render" directly; we rely on drawData null check above.
        // If drawData exists but empty, it's fine.
    }

    if (!drawData) return;
    // Nothing to draw?
    if (drawData->CmdListsCount == 0) return;

    // Deferred user-texture retirement drain (see RetireUserTexture): one
    // tick per render (each render = a frame boundary). Entries older than
    // kUserTextureRetireRenders frames free — by then every submission that
    // bound them has hit its flight-slot fence (smvk::limits::MaxFramesInFlight + 1
    // margin), so the backend free (the seam's RemoveTexture) is clean.
    // Drained here (inside the render, after this frame's ImGui::Render())
    // so a queued id can never outlive the queue's own frame cadence.
    ++renderTicks_;
    if (!userTextureRetireQueue_.empty()) {
        size_t keep = 0;
        for (size_t i = 0; i < userTextureRetireQueue_.size(); ++i) {
            const auto [id, stamp] = userTextureRetireQueue_[i];
            if (renderTicks_ - stamp > kUserTextureRetireRenders) {
                ui::RemoveTexture(id);
            } else {
                userTextureRetireQueue_[keep++] = userTextureRetireQueue_[i];
            }
        }
        userTextureRetireQueue_.resize(keep);
    }

    ui::RenderDrawData(cmd.GetNativeHandle(), drawData);
}

void ImGuiOverlay::AddPass(RenderGraph& rg,
    LogicalImageViewHandle colorView, Extent2D extent,
    AttachmentLoadOp loadOp, ClearColor clear) {
    if (!initialized_) return;
    // Ensure ImGui draw data exists before submit. If the user built UI after NewFrame()
    // but didn't call Render(), do it now so the pass has something to draw.
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    if (!ImGui::GetDrawData()) {
        ImGui::Render();
    }

    auto& pass = rg.AddPass("ImGui")
        .SetOffset(0, 0)
        .SetExtent(extent)
        // Swapchain: LoadOp::Load keeps the scene under the UI.
        // Separated UI target: LoadOp::Clear + transparent clear.
        .SetColor({colorView}, loadOp, AttachmentStoreOp::Store, clear);

    // No depth for UI overlay. If caller needs depth-testing UI (rare), they can
    // use SetDepth() manually via Render() path instead of AddPass().
    pass.SetRecordingFunc([this](CommandBuffer& cmd) {
        this->Render(cmd);
    });
}

bool ImGuiOverlay::WantsCaptureMouse() const {
    if (!initialized_) return false;
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    return ImGui::GetIO().WantCaptureMouse;
}
bool ImGuiOverlay::WantsCaptureKeyboard() const {
    if (!initialized_) return false;
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    return ImGui::GetIO().WantCaptureKeyboard;
}
bool ImGuiOverlay::WantsCaptureAny() const {
    return WantsCaptureMouse() || WantsCaptureKeyboard();
}

ImTextureID ImGuiOverlay::AddUserTexture(TextureHandle texture,
    uint32_t samplerIndex) {
    // User-texture binding (see the header contract): the backend's
    // descriptor registration over the texture's sampled view + a
    // bindless sampler slot. Vulkan types resolve through the RMI's
    // opaque handles — nothing leaks past this file. Liveness is
    // verified against the LIVE listing at bind time (a scan at
    // selection-change cadence, not per frame): a dead handle — its pool
    // generation bumped, exactly what UpdateTexture's recreate path
    // does — returns a null id, so a freed view can never slip into a
    // descriptor.
    if (!initialized_ || !ctx_ || texture.empty())
        return ImTextureID(0);
    bool live = false;
    for (const TextureInfo& t : ctx_->ListTextureInfos())
        if (t.handle == texture) {
            live = true;
            break;
        }
    if (!live)
        return ImTextureID(0);
    // The seam turns the RHI texture + sampler into the backend-native
    // binding id (the Vulkan form: a descriptor set over the texture's
    // view + the cached sampler; the D3D12 form: the bindless slot).
    return ui::AddTexture(*ctx_, texture, samplerIndex);
}

ImTextureID ImGuiOverlay::AddUserTextureView(ImageViewHandle view,
    uint32_t samplerIndex) {
    // View variant of AddUserTexture (see the header contract): same
    // binding registration, but over a STANDALONE caller-created view
    // (e.g. a per-face View2D of a cubemap) instead of the texture's own
    // default view. Liveness here is the opaque getter: the pool get
    // returns null once the view's generation bumped (destroyed), so a
    // freed view can never slip into a descriptor. (The texture variant
    // scans the LIVE texture listing instead; views have no listing.)
    if (!initialized_ || !ctx_ || view.empty())
        return ImTextureID(0);
    return ui::AddTextureView(*ctx_, view, samplerIndex);
}

void ImGuiOverlay::RetireUserTexture(ImTextureID id) {
    // Deferred free (see the header contract): the binding is referenced
    // by the draw data of the frame that last drew it (and by up to
    // smvk::limits::MaxFramesInFlight prior in-flight submissions) — freeing it
    // here (the seam's RemoveTexture is an immediate backend free)
    // binds a freed set and faults the device (the texture-viewer
    // X-close crash). Queue it; Render() drains aged entries once the
    // referencing submissions are fence-complete.
    if (!initialized_ || id == ImTextureID(0))
        return;
    userTextureRetireQueue_.emplace_back(
        static_cast<uint64_t>(id), renderTicks_);
}

float ImGuiOverlay::GetWindowDpiScale() const {
    if (!window_) return 1.0f;
    float scale = SDL_GetWindowDisplayScale(window_);
    if (scale == 0.0f) {
        fprintf(stderr, "[ImGuiOverlay] SDL_GetWindowDisplayScale failed: %s\n",
            SDL_GetError());
        return 1.0f;
    }
    return scale;
}

bool ImGuiOverlay::ProcessEvent(const void* sdlEvent) {
    return ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(sdlEvent));
}

void ImGuiOverlay::ApplyDpiScale(float scale) {
    if (!initialized_) return;
    if (scale <= 0.0f)
        scale = 1.0f;

    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    ImGuiStyle& style = ImGui::GetStyle();

    // Fresh style so ScaleAllSizes never accumulates its lossy integer
    // rounding; keep the current color scheme (StyleColorsDark etc.).
    ImGuiStyle fresh{};
    fresh.ScaleAllSizes(scale);
    for (int i = 0; i < ImGuiCol_COUNT; i++)
        fresh.Colors[i] = style.Colors[i];
    style = fresh;

    // Dynamic fonts rasterize glyphs on demand at base * scale: crisp at any
    // DPI, no atlas rebuild or FontGlobalScale blur. Layout + fonts now both
    // track the scale; rasterizer density follows io.DisplayFramebufferScale
    // (see NewFrame).
    style.FontScaleDpi = scale;
    appliedDpiScale_ = scale;
}

} // namespace smvk

// Frame-time histogram bottom strip (Engine/Profile/profile.h): lives
// here — the engine's ImGui TU — so profile.cpp stays ImGui-free for the
// tool links that never pull an overlay. A SEPARATE UI from both hosts'
// panels: a full-width horizontal bucket graph pinned to the bottom of
// the screen (drawn on top; the App and the editor both just call this
// once per UI pass). Call only while the histogram is enabled — the call
// sites gate on FrameHistogramEnabled(), so the feature costs nothing
// while off.
namespace tour::profiler {

void DrawFrameHistogramUi() {
    const FrameHistogramData d = GetFrameHistogramData();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    constexpr float kStripH = 76.0f; // graph + one readout line
    ImGui::SetNextWindowPos(ImVec2(0.0f, disp.y - kStripH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(disp.x, kStripH), ImGuiCond_Always);
    if (!ImGui::Begin("##frameTimeHistogram", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    if (d.count == 0) {
        ImGui::TextDisabled("frame histogram (F12): recording - no frames yet");
        ImGui::End();
        return;
    }
    // Horizontal bucket graph: one bar per 0.5 ms bucket left-to-right
    // (last = >= 16 ms overflow), height = share of the peak bucket,
    // color ramps green -> amber -> red with the bucket's ms. Hovering a
    // bar shows the range + frame count/share.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float graphH = 44.0f;
    uint32_t peak = 1;
    for (size_t b = 0; b < FrameHistogramData::kBucketCount; ++b)
        if (d.buckets[b] > peak)
            peak = d.buckets[b];
    const float barW = (availW - float(FrameHistogramData::kBucketCount - 1) * 2.0f)
        / float(FrameHistogramData::kBucketCount);
    for (size_t b = 0; b < FrameHistogramData::kBucketCount; ++b) {
        const float h = graphH * float(d.buckets[b]) / float(peak);
        const float x = p0.x + float(b) * (barW + 2.0f);
        // Ramp: 0 ms -> green (140,220,120), 8+ ms -> red (230,90,80).
        const float t = std::min(1.0f, float(b) / 16.0f);
        const ImU32 col = ImGui::GetColorU32(ImVec4(
            0.55f + 0.35f * t, 0.86f - 0.51f * t, 0.47f - 0.19f * t, 0.90f));
        if (h > 0.0f)
            dl->AddRectFilled(ImVec2(x, p0.y + graphH - h),
                ImVec2(x + barW, p0.y + graphH), col, 1.0f);
        else
            dl->AddRect(ImVec2(x, p0.y + graphH - 1.5f),
                ImVec2(x + barW, p0.y + graphH),
                ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 0.4f)));
    }
    // Invisible hover surface over the graph for the bucket tooltip.
    ImGui::Dummy(ImVec2(availW, graphH));
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        float fx = (mouse.x - p0.x) / availW;
        fx = fx < 0.0f ? 0.0f : (fx > 0.999f ? 0.999f : fx);
        const size_t b = size_t(fx * float(FrameHistogramData::kBucketCount));
        const bool overflow = b + 1 == FrameHistogramData::kBucketCount;
        const double lo = double(b) * 0.5;
        const uint32_t n = d.buckets[b];
        if (overflow)
            ImGui::SetTooltip(">= %.1f ms: %u frame%s (%.1f%%)",
                lo, n, n == 1 ? "" : "s", 100.0 * n / double(d.count));
        else
            ImGui::SetTooltip("[%.1f .. %.1f) ms: %u frame%s (%.1f%%)",
                lo, lo + 0.5, n, n == 1 ? "" : "s",
                100.0 * n / double(d.count));
    }
    ImGui::Text("frame time - avg %.2f | p50 %.1f | p95 %.1f | p99 %.1f | max %.1f ms   (%zu frames)",
        d.avgMs, d.p50Ms, d.p95Ms, d.p99Ms, d.maxMs, d.count);
    ImGui::TextDisabled("0.5 ms buckets left-to-right (last >= 16 ms), last 1024 frames - F12 toggles");
    ImGui::End();
}

} // namespace tour::profiler
