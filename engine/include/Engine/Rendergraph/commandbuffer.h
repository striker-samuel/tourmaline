#pragma once
#include <Engine/smvk.h>

namespace smvk {
	struct Rendergraph;
	struct CommandBuffer final
	{
		CommandBuffer() = delete;

		void BindRenderPipeline(
			ShaderProgram& shaderProgram,
			VertexFormat vertexFormat =
			VertexFormat::Empty,
			const DepthSettings& depthSettings =
			DepthSettings::DepthTest(),
			const StencilSettings& stencilSettings =
			StencilSettings::DefaultDisabled(),
			const BlendSettings& blendSettings =
			BlendSettings::Opaque(),
			const MSAASettings& msaaSettings =
			MSAASettings::Count1()
		);

		void PushConstants(
			uint32_t offset,
			uint32_t size,
			const void* data,
			ShaderStage stageMask =
			ShaderStage::AllGraphics
			//ShaderStage::Vertex |
			//ShaderStage::Fragment
		);

		void Draw(
			uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0)
		{
			vkCmdDraw(
				cmd_,
				vertexCount,
				instanceCount,
				firstVertex,
				firstInstance);
		}

		void DrawIndexed(
			uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			uint32_t baseVertex = 0,
			uint32_t firstInstance = 0)
		{
			vkCmdDrawIndexed(
				cmd_,
				indexCount,
				instanceCount,
				firstIndex,
				static_cast<int32_t>(
					baseVertex),
				firstInstance);
		}


	private:
		friend RenderGraph;

		CommandBuffer(
			VkCommandBuffer cmd,
			RenderGraph& graph,
			RenderPass& pass)
			: cmd_(cmd)
			, graph_(&graph)
			, renderPass_(&pass)
		{}

		VkCommandBuffer cmd_ =
			VK_NULL_HANDLE;
		RenderGraph* graph_ =
			nullptr;
		RenderPass* renderPass_ =
			nullptr;
		VkPipeline currentPipeline_ =
			VK_NULL_HANDLE;
		VkPipelineLayout currentPipelineLayout_ =
			VK_NULL_HANDLE;
		void BindStaticVertexFmt();
	};

}; //namespace smvk
