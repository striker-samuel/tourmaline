#include <Engine/Renderer/resource_manager.h>

MeshManager::MeshManager(smvk::Context& ctx) :ctx_(ctx) {}
void MeshManager::InitalizeGeoBuffers() {
	{
		uint32_t totalVertexBytes = 512u * 1024u * 1024u;
		uint32_t totalIndexBytes = 512u * 1024u * 1024u;

		// Create the actual GPU buffers (these are the page's buffers)
		smvk::BufferHandle vb_pos = ctx_.CreateBuffer(
			{
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		smvk::BufferHandle vb_shd = ctx_.CreateBuffer(
			{
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		smvk::BufferHandle ib = ctx_.CreateBuffer(
			{
				totalIndexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		auto max_vertices = std::min(
			totalVertexBytes / uint32_t(sizeof(VertexPos)),
			totalVertexBytes / uint32_t(sizeof(VertexShading))
		);

		// Index allocator (bytes)
		{
			VmaVirtualBlockCreateInfo ci{};
			ci.size = totalIndexBytes;
			VK_CHECK(vmaCreateVirtualBlock(&ci, &index_blk));
		}

		// Vertex allocator (VERTEX UNITS!)
		{
			VmaVirtualBlockCreateInfo vu{
				.size = max_vertices,
				.flags = 0
			};
			VK_CHECK(vmaCreateVirtualBlock(&vu, &vertex_units_blk));
		}

	}
}
MeshHandle MeshManager::UploadVertexData(Mesh::DataUpload& mesh_data)
{
	const uint32_t vcount = static_cast<uint32_t>(mesh_data.vertices.size());
	Mesh mesh_desc{};
	// This is a really hacky way to implement separate buffer pos and shading virtual alloc,
	// we alloc vertex count on a virtual allocator with vertex size and use that as bookkeeping, 
	// but then we build our offsets into the real buffers. 

	// ------------------------------------------
	// 0) Allocate vertices from ONE vertex-units block
	// ------------------------------------------
	VkDeviceSize baseVertex = 0; // <-- returned "offset" in VERTEX UNITS (not bytes)

	{
		VmaVirtualAllocationCreateInfo aV{};
		aV.size = vcount;     // vertex units
		aV.alignment = 1;     // vertex units

		if (vmaVirtualAllocate(vertex_units_blk, &aV, &mesh_desc.vma_alloc.vertex, &baseVertex) != VK_SUCCESS) {
			// Not enough contiguous vertex space in this page (or fragmented)
			return {}; // TODO :: cath this result!
		}
	}

	// Derived byte offsets for each stream (for copy destinations)
	const VkDeviceSize pos_byte_offset = baseVertex * sizeof(VertexPos);
	const VkDeviceSize shd_byte_offset = baseVertex * sizeof(VertexShading);

	// ------------------------------------------
	// 1) INDICES
	// ------------------------------------------
	using IndexT = uint32_t;
	const VkDeviceSize indexBytes =
		sizeof(IndexT) * static_cast<VkDeviceSize>(mesh_data.indices.size());

	VkDeviceSize index_offset_bytes = 0;

	{
		VmaVirtualAllocationCreateInfo aIdx{};
		aIdx.size = indexBytes;
		aIdx.alignment = sizeof(IndexT);

		if (vmaVirtualAllocate(index_blk /*or index_blk*/, &aIdx, &mesh_desc.vma_alloc.index, &index_offset_bytes) != VK_SUCCESS) {
			vmaVirtualFree(vertex_units_blk, mesh_desc.vma_alloc.vertex); // Roll back vertex allocation
			return {};
		}

		ctx_.UploadDataToBuffer(indexBuffer, mesh_data.indices.data(), indexBytes, 0, index_offset_bytes);
	}

	// ------------------------------------------
	// 2) Split vertices into two CPU arrays
	// ------------------------------------------
	std::vector<VertexPos> pos(vcount);
	std::vector<VertexShading> shd(vcount);

	for (uint32_t i = 0; i < vcount; ++i) {
		const Vertex& v = mesh_data.vertices[i];

		pos[i].pos = v.pos;

		shd[i].color = v.color;
		shd[i].texCoord = v.texCoord;
		shd[i].normal = v.normal;
		shd[i].tangent = v.tangent;
	}

	// ------------------------------------------
	// 3) Upload positions to binding 0 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize posBytes = sizeof(VertexPos) * static_cast<VkDeviceSize>(vcount);
		ctx_.UploadDataToBuffer(vertexPosBuffer, pos.data(), posBytes, 0, 0, pos_byte_offset);
	}

	// ------------------------------------------
	// 4) Upload shading to binding 1 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize shdBytes = sizeof(VertexShading) * static_cast<VkDeviceSize>(vcount);
		ctx_.UploadDataToBuffer(vertexShadingBuffer, pos.data(), shdBytes, 0, 0, shd_byte_offset);
	}

	// ------------------------------------------
	// 5) Surface (bind-once style)
	// ------------------------------------------
	Mesh::Surface ds{};
	ds.index_count = static_cast<uint32_t>(mesh_data.indices.size());
	ds.first_index = static_cast<uint32_t>(index_offset_bytes / sizeof(IndexT));
	ds.base_vertex = static_cast<int32_t>(baseVertex);

	// IMPORTANT: store allocs somewhere if you want to free later.
	// e.g. ds.vertex_alloc = vtx_alloc; ds.index_alloc = index_alloc; (requires fields)

	//mesh_desc.lods = 1;
	//mesh_desc.lod_offsets = { 0 };
	mesh_desc.surfaces = { ds };
	mesh_desc.aabbMin = mesh_data.aabb_min;
	mesh_desc.aabbMax = mesh_data.aabb_max;
	//mesh_desc.alloc.page = current_geometry_page;

	return meshes.create(std::move(mesh_desc));
}
[[nodiscard]] MaterialHandle MeshManager::CreateMaterial(Material::Textures textures) {
	assert(textures.diffuse.valid() && "Invalid handle for diffuse");
	Material mat{};
	mat.textures_.diffuse = textures.diffuse;
	mat.values.tex[0] = textures.diffuse.index();
	return  materials.create(std::move(mat));
}
smvk::TextureHandle MeshManager::GetOrCreateTexture(std::string uri, ktxTexture2& tex) {

	auto it = textureCache.find(uri);
	if (it == textureCache.end()) {
		//auto han = ctx_.CreateTexture(tex);
		//textureCache[uri] = han;
		//return han;
	}
	return it->second;
}
std::vector<smvk::BufferHandle> MeshManager::GetVertexBuffers() {
	return std::vector<smvk::BufferHandle>{
		indexBuffer,
			vertexPosBuffer,
			vertexShadingBuffer,
	};
}
void destroy(MeshManager* ctx, MeshHandle handle) {};

