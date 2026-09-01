#pragma once
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <GLFW/glfw3.h>

namespace smvk {

class ImGuiOverlay {
public:
    ImGuiOverlay() = default;
    ~ImGuiOverlay();

    ImGuiOverlay(const ImGuiOverlay&) = delete;
    ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;
    ImGuiOverlay(ImGuiOverlay&&) noexcept;
    ImGuiOverlay& operator=(ImGuiOverlay&&) noexcept;

    // Must be called after ResourceManager + Swapchain are created and window exists.
    // Creates ImGui context, GLFW + Vulkan backends (dynamic rendering, no manual RenderPass).
    bool Init(ResourceManager& ctx, Swapchain& swapchain, GLFWwindow* window);
    void Shutdown();

    bool IsInitialized() const { return initialized_; }

    // Call at start of each frame BEFORE building ImGui UI.
    // Handles ImGui_ImplVulkan_NewFrame / ImGui_ImplGlfw_NewFrame / ImGui::NewFrame
    void NewFrame();

    // Renders ImGui draw data into the given rendergraph CommandBuffer.
    // Safe to call inside a RenderGraph pass recording func.
    // If ImGui::Render() hasn't been called yet this frame, it will be called.
    void Render(CommandBuffer& cmd);

    // Convenience: appends a rendergraph pass that loads the swapchain attachment
    // and renders ImGui in one draw. Exactly analogous to a classic "UI pass".
    //
    // Usage (inside RenderFrame, after all scene passes, before Execute):
    //   overlay->AddPass(*m_rendergraph, swapView, fb.swapchain->GetExtent());
    //
    // The pass uses LoadOp::Load / StoreOp::Store so scene output is preserved.
    void AddPass(RenderGraph& rg,
                 LogicalImageViewHandle swapView,
                 Extent2D extent);

    // Input helpers – can be used to skip camera/game input when UI captures it.
    bool WantsCaptureMouse() const;
    bool WantsCaptureKeyboard() const;
    bool WantsCaptureAny() const;

private:
    bool initialized_ = false;
    GLFWwindow* window_ = nullptr;
    void* imguiCtx_ = nullptr;
    void* descriptorPoolOpaque_ = nullptr; // VkDescriptorPool
    ResourceManager* ctx_ = nullptr;
    Swapchain* swapchain_ = nullptr;
};

} // namespace smvk
