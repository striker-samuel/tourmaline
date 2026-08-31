#include <Engine/Renderer/mesh.h>
#include <Engine/App/cluster.h>
#include <Engine/smvk.h>
// this is hardcoded to the vulkan impl
#include <Engine/Vulkan/impl.h> 

namespace {
	constexpr size_t defaultStaticVertexHeapSize = 512ull * 1024ull * 1024ull;
	constexpr size_t defaultStaticIndexHeapSize = 512ull * 1024ull * 1024ull;
} // anonymous namespace

namespace smvk {
	VertexFormatDescription GetVertexFormatDescription(VertexFormat format) {
		switch (format)
		{
		case VertexFormat::Empty:
			return {};
		case VertexFormat::StaticVertexFormat:
			return StaticVertexFormat;
		}
	}
	void AllocateStaticVertexFmt(smvk::VulkanResourceManager& ctx)
	{
		staticVertexFmtHeap.m_staticVertexPosBuffer = ctx.CreateBuffer(defaultStaticVertexHeapSize, smvk::BufferType_Vertex, "static vertex pos").release();
		staticVertexFmtHeap.m_staticVertexShadingBuffer = ctx.CreateBuffer(defaultStaticVertexHeapSize, smvk::BufferType_Vertex, "static vertex shading").release();
		staticVertexFmtHeap.m_staticIndexBuffer = ctx.CreateBuffer(defaultStaticIndexHeapSize, smvk::BufferType_Index, "static index").release();
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