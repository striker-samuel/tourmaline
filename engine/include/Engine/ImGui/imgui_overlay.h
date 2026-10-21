// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/Rendergraph/rendergraph.h>

#include <utility>
#include <vector>

// ImTextureID (the UI texture identifier the binding below returns; the
// overlay owns the UI side of the engine, so the ImGui type lives here).
#include <imgui.h>

// SDL3 windowing (forward-declared; .cpp includes <SDL3/SDL_video.h>).
struct SDL_Window;

namespace smvk {

// Init-time configuration: UI font + DPI handling.
struct ImGuiOverlayDesc {
    // Optional TTF (readable filesystem path, e.g. "content/fonts/...").
    // When null or unreadable, ImGui falls back to its built-in default font.
    const char* fontPath = nullptr;
    // Base pixel size the font is loaded at; ImGui 1.92 dynamic fonts
    // rasterize glyphs on demand at base * DPI scale, so this stays unscaled.
    float fontBaseSize = 16.0f;
    // <= 0.0f -> auto (SDL window display scale).
    float dpiScale = 0.0f;
    // Color attachment format ImGui renders into (drives the backend
    // pipeline's rendering format). Undefined = the swapchain surface
    // format (direct-to-swapchain draws). A separated UI target must pass
    // its own format - the UNORM sibling of the surface format, since
    // ImGui authors sRGB-encoded colors that must be stored raw.
    Format colorFormat = Format::Undefined;
};

class ImGuiOverlay {
public:
    ImGuiOverlay() = default;
    ~ImGuiOverlay();

    ImGuiOverlay(const ImGuiOverlay&) = delete;
    ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;
    ImGuiOverlay(ImGuiOverlay&&) noexcept;
    ImGuiOverlay& operator=(ImGuiOverlay&&) noexcept;

    // Must be called after ResourceManager + Swapchain are created and window exists.
    // Creates ImGui context, SDL3 + Vulkan backends (dynamic rendering, no manual RenderPass),
    // loads the desc font and applies the desc DPI scale.
    bool Init(ResourceManager& ctx, Swapchain& swapchain, SDL_Window* window,
              const ImGuiOverlayDesc& desc = {});
    void Shutdown();

    bool IsInitialized() const { return initialized_; }

    // Call at start of each frame BEFORE building ImGui UI.
    // Handles ImGui_ImplVulkan_NewFrame / ImGui_ImplSDL3_NewFrame / ImGui::NewFrame.
    // targetExtent: pixel size of the color target ImGui will be rendered into
    // (the separated UI pass target, or the swapchain). When non-zero, sets
    // io.DisplayFramebufferScale = targetExtent / DisplaySize so draw data and
    // font rasterizer density land exactly on that target. Zero = backend default.
    void NewFrame(Extent2D targetExtent = {});

    // Renders ImGui draw data into the given rendergraph CommandBuffer.
    // Safe to call inside a RenderGraph pass recording func.
    // If ImGui::Render() hasn't been called yet this frame, it will be called.
    void Render(CommandBuffer& cmd);

    // Convenience: appends a rendergraph pass that renders ImGui into the
    // given color view in one draw. Exactly analogous to a classic "UI pass":
    //   - onto the swapchain: AddPass(rg, swapView, swapExtent)          (LoadOp::Load, scene preserved)
    //   - into a separated UI target:   AddPass(rg, uiView, uiExtent,
    //                                      AttachmentLoadOp::Clear, {0,0,0,0}) (transparent clear)
    void AddPass(RenderGraph& rg,
                 LogicalImageViewHandle colorView,
                 Extent2D extent,
                 AttachmentLoadOp loadOp = AttachmentLoadOp::Load,
                 ClearColor clear = ClearColor{0, 0, 0, 1});

    // Input helpers – can be used to skip camera/game input when UI captures it.
    bool WantsCaptureMouse() const;
    bool WantsCaptureKeyboard() const;
    bool WantsCaptureAny() const;

    // Forwards one SDL event to the ImGui SDL3 backend. Wire it to
    // App::imguiEventHook_ after Init so App::PollEvents feeds ImGui:
    //   imguiEventHook_ = &smvk::ImGuiOverlay::ProcessEvent;
    static bool ProcessEvent(const void* sdlEvent);

    // ---- User-texture binding (in-UI image display, e.g. debug previews) ----
    // Binds a LIVE texture's sampled view + a bindless sampler slot through
    // the backend's user-texture descriptor registration; the returned
    // identifier is valid for ImGui::Image until RetireUserTexture. The
    // backend owns one descriptor per Add (leaks are descriptor-pool
    // slots) — always pair them. Staleness is built in: a dead handle
    // (its pool generation bumped — destroyed, or UpdateTexture's
    // recreate path) returns a null id, so a freed view can never slip
    // into a descriptor. Render thread only.
    ImTextureID AddUserTexture(TextureHandle texture, uint32_t samplerIndex);
    // View variant: binds a STANDALONE caller-created view (e.g. a
    // per-face View2D of a cubemap — the texture variant above only
    // reaches the texture's own default view, whose cube/array/3D type
    // cannot be sampled as the 2D the ImGui shader expects). Same
    // liveness contract (dead view -> null id) and pairing rule.
    ImTextureID AddUserTextureView(ImageViewHandle view, uint32_t samplerIndex);
    // DEFERRED free of a binding minted above — never immediate: the
    // binding's descriptor set is referenced by the ImGui draw data of
    // the frame that last drew it (and by up to MAX_FRAMES_IN_FLIGHT
    // prior in-flight submissions), so freeing it under them
    // (ImGui_ImplVulkan_RemoveTexture -> vkFreeDescriptorSets, immediate)
    // binds a freed set and faults the device — the texture-viewer
    // X-close crash: the close-click frame's body still rendered the
    // preview, then the close drop freed the set BEFORE the ImGui pass
    // rendered that very draw data (vkWaitForFences VkResult=-4, device
    // lost, empty fault info; reselection only survived because the
    // pool's LIFO free-list hands the SAME set right back to the re-Add,
    // keeping its contents live). The retire queue holds ids until
    // kUserTextureRetireRenders ImGui renders pass after the retire
    // (each render = a frame boundary; a flight slot's fence is complete
    // before its MAX+1'th successor builds), then frees through the
    // backend. Render-thread only, like the Adds.
    void RetireUserTexture(ImTextureID id);

    // ---- DPI control ----
    // Monitor content scale of the window (1.0 on standard displays).
    float GetWindowDpiScale() const;
    // Applies a UI DPI scale: style.FontScaleDpi (font raster density +
    // rendered size, crisp with 1.92 dynamic fonts) and re-scales all style
    // layout sizes from a freshly initialized style (ScaleAllSizes is lossy,
    // so never scale relative to an already scaled style). Idempotent.
    void ApplyDpiScale(float scale);
    // DPI scale currently applied to the ImGui context (0 before Init).
    float AppliedDpiScale() const { return appliedDpiScale_; }

private:
    bool initialized_ = false;
    SDL_Window* window_ = nullptr;
    void* imguiCtx_ = nullptr;
    void* descriptorPoolOpaque_ = nullptr; // VkDescriptorPool
    ResourceManager* ctx_ = nullptr;
    Swapchain* swapchain_ = nullptr;
    ImGuiOverlayDesc desc_{};
    float appliedDpiScale_ = 0.0f;
    // Deferred user-texture retirement (see RetireUserTexture): one render
    // tick per Render() call; entries free once
    // (renderTicks_ - stamp) > kUserTextureRetireRenders.
    static constexpr uint64_t kUserTextureRetireRenders = 4; // MAX_FRAMES_IN_FLIGHT (2-3) + 1 margin
    std::vector<std::pair<uint64_t, uint64_t>> userTextureRetireQueue_{}; // (id, render stamp)
    uint64_t renderTicks_ = 0;
};

} // namespace smvk
