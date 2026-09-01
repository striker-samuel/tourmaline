#include <Engine/smvk.h>
#include <Engine/RHI/resource_manager.h>
#include "../Vulkan/detail/vulkan_impl.h"

#include <Engine/Renderer/mesh.h>
#include <Engine/App/cluster.h>

namespace {
	constexpr size_t defaultStaticVertexHeapSize = 512ull * 1024ull * 1024ull;
	constexpr size_t defaultStaticIndexHeapSize = 512ull * 1024ull * 1024ull;
} // anonymous namespace

namespace smvk {

	MaterialPipelineState MakePipelineStateForMaterial(
		AlphaMode alphaMode,
		bool doubleSided,
		ShadingModel shadingModel)
	{
		MaterialPipelineState out{};

		// Raster: double-sided disables culling; otherwise back-face cull.
		out.raster.cullMode = doubleSided ? CullMode::None : CullMode::Back;
		out.raster.polygonMode = PolygonMode::Fill;
		out.raster.frontFace = FrontFace::CounterClockwise;

		// Depth: opaque/masked write depth; blend does not write depth
		// (prevents transparent sorting artefacts, caller may override).
		if (alphaMode == AlphaMode::Blend) {
			out.depth = DepthSettings::Disabled();
			// Keep depth test but no write for typical transparent.
			out.depth.depthTestEnable = true;
			out.depth.depthFunc = CompareOp::Less;
			out.depth.writeEnable = false;
		} else {
			out.depth = DepthSettings::DepthTest();
		}

		// Blend: opaque/masked = no blend; Blend = alpha blend.
		// Masked shading still opaque blend + alpha test in shader via alphaCutoff.
		if (alphaMode == AlphaMode::Blend) {
			out.blend = BlendSettings::AlphaBlend();
		} else {
			out.blend = BlendSettings::Opaque();
		}

		// Future: shadingModel selects shader permutation.
		// e.g. Unlit -> different PipelineKey::program; kept here so material
		// remains single source of truth.
		(void)shadingModel;

		return out;
	}

	VertexFormatDescription GetVertexFormatDescription(VertexFormat format) {
		switch (format)
		{
		case VertexFormat::Empty:
			return {};
		case VertexFormat::StaticVertexFormat:
			return StaticVertexFormat;
		}
		return{};
	}
	void AllocateStaticVertexFmt(smvk::ResourceManager& ctx)
	{
		auto* vctx = static_cast<VulkanResourceManager*>(ctx.GetVulkanOpaque());
		staticVertexFmtHeap.m_staticVertexPosBuffer = vctx->CreateBuffer(defaultStaticVertexHeapSize, smvk::BufferType_Vertex, "static vertex pos").release();
		staticVertexFmtHeap.m_staticVertexShadingBuffer = vctx->CreateBuffer(defaultStaticVertexHeapSize, smvk::BufferType_Vertex, "static vertex shading").release();
		staticVertexFmtHeap.m_staticIndexBuffer = vctx->CreateBuffer(defaultStaticIndexHeapSize, smvk::BufferType_Index, "static index").release();
		const VkDeviceSize maxVertices = std::min(
			defaultStaticVertexHeapSize / sizeof(smvk::VertexPos),
			defaultStaticVertexHeapSize / sizeof(smvk::VertexShading)
		);

		{
			VmaVirtualBlockCreateInfo info{};
			info.size = maxVertices;

			VK_CHECK(
				vmaCreateVirtualBlock(
					&info,
					&staticVertexFmtHeap.m_staticVertexBlock
				)
			);
		}


		// -------------------------------------------------------------------------
		// Index virtual allocator
		//
		// This one IS measured in bytes.
		// -------------------------------------------------------------------------

		{
			VmaVirtualBlockCreateInfo info{};
			info.size = defaultStaticIndexHeapSize;

			VK_CHECK(
				vmaCreateVirtualBlock(
					&info,
					&staticVertexFmtHeap.m_staticIndexBlock
				)
			);
		}
	}
};