#pragma once

#include <iostream>

#include <Engine/App/app.h>
#include <Engine/Renderer/rendergraph.h>
#include <Engine/Renderer/dxc_integration.h>

static constexpr const char* kTriangleVertShader = R"(

struct VSOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    static const float2 positions[3] =
    {
        float2( 0.0, -0.5),
        float2( 0.5,  0.5),
        float2(-0.5,  0.5)
    };

    static const float3 colors[3] =
    {
        float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0),
        float3(0.0, 0.0, 1.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexID], 0.0, 1.0);
    output.color = colors[vertexID];

    return output;
}

)";

static constexpr const char* kTriangleFragShader = R"(

float4 main(
    float4 position : SV_Position,
    float3 color    : COLOR0
) : SV_Target
{
    return float4(color, 1.0);
}

)";
class TriangleApp : public smvk::VulkanApp {
    using Super = smvk::VulkanApp;

public:
    TriangleApp() = default;
    ~TriangleApp() = default;

private:
    bool Initialize() override {
        if (!Super::Initialize())
            return false;

        auto vertCode = CompileHLSLToSPIRV(
            kTriangleVertShader,
            ShaderStage::Vertex
        );

        auto fragCode = CompileHLSLToSPIRV(
            kTriangleFragShader,
            ShaderStage::Fragment
        );

        m_program = {
            .vert = m_context->CreateShaderModule(vertCode),
            .frag = m_context->CreateShaderModule(fragCode),
        };

        return true;
    }

    void EarlyUpdate() override {
        Super::EarlyUpdate();
    }

    void Update() override {
        Super::Update();
    }

    void LateUpdate() override {
        Super::LateUpdate();
    }

    void DrawFrame() override {
        auto fb = m_swapchain->AcquireFramebuffer();

        RenderFrame(fb);

        m_swapchain->EndAndPresentFramebuffer(fb);
    }

    void RenderFrame(smvk::FrameBuffer fb) {
        m_rendergraph->NewFrame();

        auto swapchainImage = m_rendergraph->AddImageView(
            fb.swapchain.imageViews_[fb.imageIndex],
            fb.swapchain.vk_surfaceFormat_
        );

        std::vector<smvk::LogicalImageViewHandle> colors{
            swapchainImage
        };

        m_rendergraph->AddPass()
            .setOffset({ 0, 0 })
            .setExtent(fb.swapchain.extent_)
            .setColor(
                colors,
                VK_ATTACHMENT_LOAD_OP_CLEAR,
                VK_ATTACHMENT_STORE_OP_STORE,
                { { 0.0f, 0.0f, 0.0f, 1.0f } }
            )
            .setRecordingFunc(
                [this, fb](smvk::RenderPassContext pass) {
            m_rendergraph->BindRenderPipeline(
                fb.commandBuffer,
                m_program
            );

            vkCmdDraw(
                fb.commandBuffer,
                3,  // vertex count
                1,  // instance count
                0,  // first vertex
                0   // first instance
            );
        }
            );

        m_rendergraph->Execute(fb);
        m_context->ProcessDeferrredTasks();
    }

private:
    smvk::ShaderProgram m_program{};
};


int main(int argc, const char* argv[]) {
    if (argc > 1) {
        std::cout << argv[0];
    }

    TriangleApp app = TriangleApp();
    try {
        app.Run();
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
