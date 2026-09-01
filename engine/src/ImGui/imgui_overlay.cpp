#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"

#include <Engine/ImGui/imgui_overlay.h>
#include "../Vulkan/detail/vulkan_impl.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

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
        descriptorPoolOpaque_ = o.descriptorPoolOpaque_;
        ctx_ = o.ctx_;
        swapchain_ = o.swapchain_;
        o.initialized_ = false;
        o.window_ = nullptr;
        o.imguiCtx_ = nullptr;
        o.descriptorPoolOpaque_ = nullptr;
        o.ctx_ = nullptr;
        o.swapchain_ = nullptr;
    }
    return *this;
}

bool ImGuiOverlay::Init(ResourceManager& ctx, Swapchain& swapchain, GLFWwindow* window) {
    if (initialized_) return true;
    ctx_ = &ctx;
    swapchain_ = &swapchain;
    window_ = window;

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
    // Enable docking / viewports optionally:
    // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // ---- Init GLFW backend ----
    // Use install_callbacks = true so ImGui chains to App's already-installed callbacks.
    // App::Initialize sets its callbacks BEFORE this Init, so chaining works.
    if (!ImGui_ImplGlfw_InitForVulkan(window, true)) {
        fprintf(stderr, "[ImGuiOverlay] ImGui_ImplGlfw_InitForVulkan failed\n");
        return false;
    }

    // ---- Create descriptor pool for ImGui ----
    // ImGui needs a fairly large pool. Use Vulkan directly.
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 100 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100 },
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 100;
    poolInfo.poolSizeCount = (uint32_t)(sizeof(poolSizes)/sizeof(poolSizes[0]));
    poolInfo.pPoolSizes = poolSizes;

    VkDevice device = GetVkDevice();
    if (device == VK_NULL_HANDLE) {
        fprintf(stderr, "[ImGuiOverlay] GetVkDevice returned null\n");
        return false;
    }
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        fprintf(stderr, "[ImGuiOverlay] vkCreateDescriptorPool failed\n");
        return false;
    }
    descriptorPoolOpaque_ = reinterpret_cast<void*>(pool);

    // ---- Init Vulkan backend (dynamic rendering) ----
    VkInstance instance = GetVkInstance();
    VkPhysicalDevice physicalDevice = GetVkPhysicalDevice();
    VkQueue graphicsQueue = GetVkGraphicsQueue();

    VkFormat colorFormat = toVkFormat(swapchain.GetSurfaceFormat());
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // UI pass has no depth; could use swap depthFormat if we ever depth-test UI.

    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.colorAttachmentCount = 1;
    renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    renderingCreateInfo.depthAttachmentFormat = depthFormat;
    renderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    ImGui_ImplVulkan_InitInfo vulkanInit{};
    vulkanInit.Instance = instance;
    vulkanInit.PhysicalDevice = physicalDevice;
    vulkanInit.Device = device;
    vulkanInit.QueueFamily = GetVkGraphicsQueueFamily();
    vulkanInit.Queue = graphicsQueue;
    vulkanInit.DescriptorPool = pool;
    vulkanInit.MinImageCount = swapchain.GetMinImageCount();
    vulkanInit.ImageCount = swapchain.GetImageCount();
    vulkanInit.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    vulkanInit.UseDynamicRendering = true;
    vulkanInit.PipelineRenderingCreateInfo = renderingCreateInfo;

    // ImGui_ImplVulkan_Init with dynamic rendering requires ApiVersion >= 1.3 when using volk.
    // volk loads 1.3 entry points; we pass VK_API_VERSION_1_3.
    vulkanInit.ApiVersion = VK_API_VERSION_1_3;

    if (!ImGui_ImplVulkan_Init(&vulkanInit)) {
        fprintf(stderr, "[ImGuiOverlay] ImGui_ImplVulkan_Init failed\n");
        vkDestroyDescriptorPool(device, pool, nullptr);
        descriptorPoolOpaque_ = nullptr;
        return false;
    }

    // Fonts: ImGui_ImplVulkan_CreateFontsTexture is deprecated since 1.91 – fonts are
    // created on demand via the backend. No manual upload needed with dynamic rendering
    // if we let ImGui create fonts lazily. However to ensure first frame has fonts,
    // we can trigger an upload via a one-time command buffer if needed.
    // The modern backend handles it internally; no action required.

    initialized_ = true;
    return true;
}

void ImGuiOverlay::Shutdown() {
    if (!initialized_) return;
    // Ensure GPU idle before tearing down ImGui pipelines/descriptor pool.
    VkDevice device = GetVkDevice();
    if (device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device);

    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));

    if (descriptorPoolOpaque_) {
        VkDescriptorPool pool = reinterpret_cast<VkDescriptorPool>(descriptorPoolOpaque_);
        vkDestroyDescriptorPool(device, pool, nullptr);
        descriptorPoolOpaque_ = nullptr;
    }

    imguiCtx_ = nullptr;
    initialized_ = false;
    ctx_ = nullptr;
    swapchain_ = nullptr;
}

void ImGuiOverlay::NewFrame() {
    if (!initialized_) return;
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(imguiCtx_));
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
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

    VkCommandBuffer vkCmd = reinterpret_cast<VkCommandBuffer>(cmd.GetNativeHandle());
    ImGui_ImplVulkan_RenderDrawData(drawData, vkCmd);
}

void ImGuiOverlay::AddPass(RenderGraph& rg, LogicalImageViewHandle swapView, Extent2D extent) {
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
        // Load existing swapchain color (scene already rendered), Store after UI compositing.
        .SetColor({swapView}, AttachmentLoadOp::Load, AttachmentStoreOp::Store, ClearColor{0,0,0,1});

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

} // namespace smvk
