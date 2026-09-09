#pragma once
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <GLFW/glfw3.h>

namespace smvk {

// Init-time configuration: UI font + DPI handling.
struct ImGuiOverlayDesc {
    // Optional TTF (readable filesystem path, e.g. "content/fonts/...").
    // When null or unreadable, ImGui falls back to its built-in default font.
    const char* fontPath = nullptr;
    // Base pixel size the font is loaded at; ImGui 1.92 dynamic fonts
    // rasterize glyphs on demand at base * DPI scale, so this stays unscaled.
    float fontBaseSize = 16.0f;
    // <= 0.0f -> auto (GLFW window content scale).
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
    // Creates ImGui context, GLFW + Vulkan backends (dynamic rendering, no manual RenderPass),
    // loads the desc font and applies the desc DPI scale.
    bool Init(ResourceManager& ctx, Swapchain& swapchain, GLFWwindow* window,
              const ImGuiOverlayDesc& desc = {});
    void Shutdown();

    bool IsInitialized() const { return initialized_; }

    // Call at start of each frame BEFORE building ImGui UI.
    // Handles ImGui_ImplVulkan_NewFrame / ImGui_ImplGlfw_NewFrame / ImGui::NewFrame.
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
    GLFWwindow* window_ = nullptr;
    void* imguiCtx_ = nullptr;
    void* descriptorPoolOpaque_ = nullptr; // VkDescriptorPool
    ResourceManager* ctx_ = nullptr;
    Swapchain* swapchain_ = nullptr;
    ImGuiOverlayDesc desc_{};
    float appliedDpiScale_ = 0.0f;
};

} // namespace smvk
