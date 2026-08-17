#include <Engine/Renderer/cluster.h>

// VkDeviceSize hould be equivalent
static constexpr size_t VertexBufferSize = 512ull * 1024ull * 1024ull;
static constexpr size_t IndexBufferSize = 512ull * 1024ull * 1024ull;

namespace smvk {

	Cluster::Cluster(ResourceContext& ctx)
		: ctx_(ctx) {
		CreateGeometryBuffers();
	}
	Cluster::~Cluster()
	{
		DestroyGeometryBuffers();
	}

	void Cluster::BindStaticMeshBuffers(VkCommandBuffer cmd) const
	{
		VkBuffer vertexBuffers[] = {
			ctx_.buffers_.get(vertexPosBuffer_)->vk_buffer,
			ctx_.buffers_.get(vertexShadingBuffer_)->vk_buffer
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
			ctx_.buffers_.get(indexBuffer_)->vk_buffer,
			0,
			VK_INDEX_TYPE_UINT32
		);
	}
	// -----------------------------------------------------------------------------
	// Buffer initialization
	// -----------------------------------------------------------------------------

	void Cluster::CreateGeometryBuffers()
	{
		vertexPosBuffer_ = ctx_.CreateBuffer({
			VertexBufferSize,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT
			});

		vertexShadingBuffer_ = ctx_.CreateBuffer({
			VertexBufferSize,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT
			});

		indexBuffer_ = ctx_.CreateBuffer({
			IndexBufferSize,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT
			});


		// -------------------------------------------------------------------------
		// Vertex virtual allocator
		//
		// IMPORTANT:
		// This allocator is measured in VERTICES, not bytes.
		//
		// Both physical vertex streams use this same allocation.
		// -------------------------------------------------------------------------

		const VkDeviceSize maxVertices = std::min(
			VertexBufferSize / sizeof(VertexPos),
			VertexBufferSize / sizeof(VertexShading)
		);

		{
			VmaVirtualBlockCreateInfo info{};
			info.size = maxVertices;

			VK_CHECK(
				vmaCreateVirtualBlock(
					&info,
					&vertexBlock_
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
					&indexBlock_
				)
			);
		}
	}


	void Cluster::DestroyGeometryBuffers()
	{
		if (vertexBlock_) {
			vmaClearVirtualBlock(vertexBlock_);
			vmaDestroyVirtualBlock(vertexBlock_);
			vertexBlock_ = VK_NULL_HANDLE;
		}

		if (indexBlock_) {
			vmaClearVirtualBlock(indexBlock_);
			vmaDestroyVirtualBlock(indexBlock_);
			indexBlock_ = VK_NULL_HANDLE;
		}

		// Whatever destruction API your ResourceContext uses:
		//
		// ctx_.DestroyBuffer(vertexPosBuffer_);
		// ctx_.DestroyBuffer(vertexShadingBuffer_);
		// ctx_.DestroyBuffer(indexBuffer_);
	}


	// -----------------------------------------------------------------------------
	// Mesh upload
	// -----------------------------------------------------------------------------

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
				vertexBlock_,
				&allocInfo,
				&result.allocation.vertex,
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
				indexBlock_,
				&allocInfo,
				&result.allocation.index,
				&indexByteOffset
			);

			if (res != VK_SUCCESS) {

				vmaVirtualFree(
					vertexBlock_,
					result.allocation.vertex
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
			vertexPosBuffer_,
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
			vertexShadingBuffer_,
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
				indexBuffer_,
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

		if (mesh->allocation.vertex) {

			vmaVirtualFree(
				vertexBlock_,
				mesh->allocation.vertex
			);

			mesh->allocation.vertex = VK_NULL_HANDLE;
		}

		if (mesh->allocation.index) {

			vmaVirtualFree(
				indexBlock_,
				mesh->allocation.index
			);

			mesh->allocation.index = VK_NULL_HANDLE;
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

		material.values.tex[0] =
			static_cast<int>(diffuse.index());

		return materials.create(
			std::move(material)
		);
	}

} // namespace smvk