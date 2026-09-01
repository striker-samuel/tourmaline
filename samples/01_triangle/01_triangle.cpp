#pragma once

#include <iostream>

#include <Engine/core.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/dxc_integration.h>

static constexpr const char* kTriangleVertShader = R"(

struct VSOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

VSOutput mainVS(uint vertexID : SV_VertexID)
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

    static const float4x4 projection =
    {
        1.0,  0.0, 0.0, 0.0,
        0.0, -1.0, 0.0, 0.0,
        0.0,  0.0, 1.0, 0.0,
        0.0,  0.0, 0.0, 1.0
    };

    VSOutput output;

    float4 position = float4(positions[vertexID], 0.0, 1.0);

    output.position = mul(projection, position);
//projection is needed beacuse of vulkan defaults viewport behaviour
    output.color = colors[vertexID];

    return output;
}

)";

static constexpr const char* kTriangleFragShader = R"(

float4 mainPS(
    float4 position : SV_Position,
    float3 color    : COLOR0
) : SV_Target
{
    return float4(color, 1.0);
}

)";

class TriangleApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;

public:
	TriangleApp() = default;
	~TriangleApp() {
		for (int i = 0; i < m_program.shaderCount; i++) {
			smvk::destroy(m_context.get(), m_program.shaders[i].module);
		}
	}
private:
	bool Initialize() override {
		if (!Super::Initialize())
			return false;

		//void CreateResources();
		auto vertCode = CompileHLSLToSPIRV(
			kTriangleVertShader,
			DxcShaderStage::Vertex,
			L"mainVS"
		);

		auto fragCode = CompileHLSLToSPIRV(
			kTriangleFragShader,
			DxcShaderStage::Fragment,
			L"mainPS"
		);

		smvk::Shader vert = {
			.module = m_context->CreateShaderModule(vertCode).release(),
			.entryPoint = "mainVS",
			.stage = smvk::ShaderStage::Vertex
		};
		smvk::Shader frag = {
			.module = m_context->CreateShaderModule(fragCode).release(),
			.entryPoint = "mainPS",
			.stage = smvk::ShaderStage::Fragment
		};

		m_program = {
			.shaders = {vert, frag},
			.shaderCount = 2
		};

		swapViews.clear();
		for (int i = 0; i < m_swapchain->GetImageCount(); i++) {
			swapViews.push_back(
				m_rendergraph->AddExternalImageView(
					m_swapchain->GetImageViewHandle(i),
					m_swapchain->GetSurfaceFormat()
				)
			);
		}
		return true;
	}

	void DrawFrame() {
		auto fb = m_swapchain->AcquireFramebuffer();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
	}

	void RenderFrame(smvk::Framebuffer fb) {
		m_rendergraph->NewFrame();
		std::vector<smvk::LogicalImageViewHandle> colors{
			swapViews[fb.imageIndex]
		};

		m_rendergraph->AddPass()
			.SetOffset(0, 0)
			.SetExtent(
				{
					fb.swapchain->GetExtent().width,
					fb.swapchain->GetExtent().height
				}
				//fb.swapchain.extent_
			)
			.SetColor(
				colors,
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0, 0, 0, 1)
			)
			.SetRecordingFunc(
				[this](smvk::CommandBuffer& cmd)
		{ //recording
			cmd.BindRenderPipeline(
				m_program,
				smvk::VertexFormat::Empty
			)
				;
			cmd.Draw(
				3,  // vertex count
				1,  // instance count
				0,  // first vertex
				0   // first instance
			);
		} //end recording
			);

		m_rendergraph->Execute(fb);
		m_context->ProcessDeferrredTasks(); // Not needed here
	}

private:
	std::vector<smvk::LogicalImageViewHandle> swapViews;
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
		std::cerr << "\nERROR: " << e.what() << std::endl;

		std::cerr << "\nPress Enter to exit...";
		std::cin.get();

		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
