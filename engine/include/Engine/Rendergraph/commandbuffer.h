#pragma once
#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>
// depending on Mesh abstaction for vertexFormat here. not sure I like it

namespace smvk {
	struct RenderGraph;
	struct RenderPass;
	struct CommandBuffer final
	{
		CommandBuffer(CommandBufferImpl cmd, RenderGraph& graph, RenderPass& pass);
		void BindRenderPipeline(
			ShaderProgram& shaderProgram,
			VertexFormat vertexFormat = VertexFormat::Empty,
			const DepthSettings& depthSettings =
			DepthSettings::DepthTest(),
			const StencilSettings& stencilSettings =
			StencilSettings::DefaultDisabled(),
			const BlendSettings& blendSettings =
			BlendSettings::Opaque(),
			const MSAASettings& msaaSettings =
			MSAASettings::Count1());

		void PushConstants(
			uint32_t offset,
			uint32_t size,
			const void* data,
			ShaderStage stages =
			ShaderStage::AllGraphics);

		void Draw(
			uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0);

		void DrawIndexed(
			uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			int32_t vertexOffset = 0,
			uint32_t firstInstance = 0
		);

		void BindStaticVertexFmt();

	private:
		CommandBufferImpl cmd_{};
		RenderGraph* graph_ = nullptr;
		RenderPass* renderPass_ = nullptr;
		PipelineImpl currentPipeline_{};
	};
}; //namespace smvk
