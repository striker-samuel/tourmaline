#include <Engine/Renderer/mesh.h>
#include <Engine/App/cluster.h>

#include <Engine/Vulkan/impl.h>
#include <Engine/smvk.h>

namespace {
	constexpr size_t VertexBufferSize = 512ull * 1024ull * 1024ull;
	constexpr size_t IndexBufferSize = 512ull * 1024ull * 1024ull;
} // anonymous namespace

namespace smvk {
	Cluster::~Cluster() {
		// staticMeshes have vmaAllocations that need to be destroyed properly
		staticMeshes.forEachHandle([&](ClusterStaticMeshHandle han) {
			DestroyMesh(han);
		});
		//these can just go atm
		materials.clear();
		textures.clear();
	}
	ClusterStaticMeshHandle Cluster::UploadStaticMesh(
		const StaticMeshUpload& data)
	{
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());

		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());


		if (vertexCount == 0)
			return {};


		StaticMesh result{};


		// =========================================================================
		// 1. Allocate vertex range
		//
		// vertexBase is in VERTEX UNITS, not bytes.
		// =========================================================================

		VkDeviceSize vertexBase = 0;

		{
			VmaVirtualAllocationCreateInfo allocInfo{};
			allocInfo.size = vertexCount;
			allocInfo.alignment = 1;

			const VkResult res = vmaVirtualAllocate(
				ctx_.m_staticVertexBlock,
				&allocInfo,
				&result.allocation.vertexAllocation,
				&vertexBase
			);

			if (res != VK_SUCCESS) {
				return {};
			}
		}


		// =========================================================================
		// 2. Allocate index range
		// =========================================================================

		const VkDeviceSize indexBytes =
			sizeof(uint32_t) *
			static_cast<VkDeviceSize>(indexCount);

		VkDeviceSize indexByteOffset = 0;

		if (indexCount > 0) {

			VmaVirtualAllocationCreateInfo allocInfo{};
			allocInfo.size = indexBytes;
			allocInfo.alignment = alignof(uint32_t);

			const VkResult res = vmaVirtualAllocate(
				ctx_.m_staticIndexBlock,
				&allocInfo,
				&result.allocation.indexAllocation,
				&indexByteOffset
			);

			if (res != VK_SUCCESS) {

			vmaVirtualFree(
				ctx_.m_staticVertexBlock,
				result.allocation.vertexAllocation
			);

				return {};
			}
		}


		// =========================================================================
		// 3. Convert AoS -> SoA-ish GPU streams
		// =========================================================================

		std::vector<VertexPos> positions(vertexCount);
		std::vector<VertexShading> shading(vertexCount);

		for (uint32_t i = 0; i < vertexCount; ++i) {

			const StaticVertex& src = data.vertices[i];

			positions[i].pos = src.pos;

			shading[i].color = src.color;
			shading[i].texCoord = src.texCoord;
			shading[i].normal = src.normal;
			shading[i].tangent = src.tangent;
		}


		// =========================================================================
		// 4. Upload position stream
		// =========================================================================

		const VkDeviceSize positionOffset =
			vertexBase * sizeof(VertexPos);

		const VkDeviceSize positionBytes =
			vertexCount * sizeof(VertexPos);

		ctx_.UploadDataToBuffer(
			ctx_.m_staticVertexPosBuffer,
			positions.data(),
			positionBytes,

			VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
			VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,

			0,
			positionOffset
		);


		// =========================================================================
		// 5. Upload shading stream
		// =========================================================================

		const VkDeviceSize shadingOffset =
			vertexBase * sizeof(VertexShading);

		const VkDeviceSize shadingBytes =
			vertexCount * sizeof(VertexShading);

		ctx_.UploadDataToBuffer(
			ctx_.m_staticVertexShadingBuffer,
			shading.data(),
			shadingBytes,

			VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
			VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,

			0,
			shadingOffset
		);


		// =========================================================================
		// 6. Upload indices
		// =========================================================================

		if (indexCount > 0) {

			ctx_.UploadDataToBuffer(
				ctx_.m_staticIndexBuffer,
				data.indices.data(),
				indexBytes,

				VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
				VK_ACCESS_2_INDEX_READ_BIT,

				0,
				indexByteOffset
			);
		}


		// =========================================================================
		// 7. Construct mesh
		// =========================================================================

		StaticMesh& mesh = result;

		mesh.geometry.topology =
			PrimitiveTopology::TriangleList;

		mesh.geometry.vertexCount = vertexCount;
		mesh.geometry.indexCount = indexCount;

		mesh.geometry.aabbMin = data.aabbMin;
		mesh.geometry.aabbMax = data.aabbMax;


		Surface surface{};

		surface.firstIndex =
			static_cast<uint32_t>(
				indexByteOffset / sizeof(uint32_t)
				);

		surface.indexCount = indexCount;

		surface.baseVertex =
			static_cast<int32_t>(vertexBase);

		mesh.geometry.surfaces.push_back(surface);


		return staticMeshes.create(
			std::move(result)
		);
	}


	// -----------------------------------------------------------------------------
	// Mesh destruction
	// -----------------------------------------------------------------------------

	void Cluster::DestroyMesh(
		ClusterStaticMeshHandle handle)
	{
		if (handle.empty())
			return;

		StaticMesh* mesh =
			staticMeshes.get(handle);

		if (mesh->allocation.vertexAllocation) {

			vmaVirtualFree(
				ctx_.m_staticVertexBlock,
				mesh->allocation.vertexAllocation
			);

			mesh->allocation.vertexAllocation = VK_NULL_HANDLE;
		}

		if (mesh->allocation.indexAllocation) {

			vmaVirtualFree(
				ctx_.m_staticIndexBlock,
				mesh->allocation.indexAllocation
			);

			mesh->allocation.indexAllocation = VK_NULL_HANDLE;
		}

		staticMeshes.destroy(handle);
	}


	// -----------------------------------------------------------------------------
	// Material
	// -----------------------------------------------------------------------------

	ClusterMaterialHandle Cluster::CreateMaterial(
		TextureHandle diffuse)
	{
		assert(
			!diffuse.empty() &&
			"Invalid diffuse texture handle"
		);

		Material material{};
		material.values.textures[0] = static_cast<int>(diffuse.index());
		return materials.create(std::move(material));
	}
	void Cluster::DestroyMaterial(ClusterMaterialHandle handle)
	{
		materials.destroy(handle);
	}

	void BindStaticVertexFmtBuffers(VulkanResourceManager& ctx, VkCommandBuffer cmd)
	{

#ifndef N_DEBUG
		assert(ctx.m_staticVertexPosBuffer.valid());
		assert(ctx.m_staticVertexShadingBuffer.valid());
		assert(ctx.m_staticIndexBuffer.valid());
#endif
		VkBuffer vertexBuffers[] = {
			ctx.buffers_.get(ctx.m_staticVertexPosBuffer)->vk_buffer,
			ctx.buffers_.get(ctx.m_staticVertexShadingBuffer)->vk_buffer
		};

		VkDeviceSize offsets[] = {
			0,
			0
		};

		vkCmdBindVertexBuffers(
			cmd,
			0,
			2,
			vertexBuffers,
			offsets
		);

		vkCmdBindIndexBuffer(
			cmd,
			ctx.buffers_.get(ctx.m_staticIndexBuffer)->vk_buffer,
			0,
			VK_INDEX_TYPE_UINT32
		);
	}

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
		ctx.m_staticVertexPosBuffer = ctx.CreateBuffer(VertexBufferSize, smvk::BufferType_Vertex, "static vertex pos").release();
		ctx.m_staticVertexShadingBuffer = ctx.CreateBuffer(VertexBufferSize, smvk::BufferType_Vertex, "static vertex shading").release();
		ctx.m_staticIndexBuffer = ctx.CreateBuffer(IndexBufferSize, smvk::BufferType_Index, "static index").release();

		const VkDeviceSize maxVertices = std::min(
			VertexBufferSize / sizeof(smvk::VertexPos),
			VertexBufferSize / sizeof(smvk::VertexShading)
		);

		{
			VmaVirtualBlockCreateInfo info{};
			info.size = maxVertices;

			VK_CHECK(
				vmaCreateVirtualBlock(
					&info,
					&ctx.m_staticVertexBlock
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
			info.size = IndexBufferSize;

			VK_CHECK(
				vmaCreateVirtualBlock(
					&info,
					&ctx.m_staticIndexBlock
				)
			);
		}
	}



};