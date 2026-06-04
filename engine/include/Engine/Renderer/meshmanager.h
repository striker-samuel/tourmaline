#pragma once

#include <Engine/Shared/common.h>


#include <Engine/Scene/scene.h>
#include <Engine/Renderer/vertex.h>
#include <Engine/Renderer/context.h>

//struct MeshData {
//	void* vertices;
//	void* indices;
//	size_t verticesSize;
//	size_t indicesSize;
//
//	// 
//	glm::vec3 aabb_min{};
//	glm::vec3 aabb_max{};
//};

struct MeshUploadInput {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	glm::vec3 aabb_min{};
	glm::vec3 aabb_max{};
};

struct Surface {
	uint32_t first_index; 
	uint32_t index_count;
	int32_t  base_vertex;
};

struct GeometryPage {

	DeviceBufferHandle indexBuffer;
	DeviceBufferHandle vertexPosBuffer;
	DeviceBufferHandle vertexShadingBuffer;

	//	uint32_t vertex_cursor = 0;   // per page
	uint32_t max_vertices = 0;    // per page capacity in vertices

	VmaVirtualBlock index_blk;         // bytes (as you already do)
	VmaVirtualBlock vertex_units_blk;  // vertices (NEW)
};

struct Mesh {

	struct Alloc {
		// page could be useful for separating tiles into pages... Lets get rid of them for the moment, or go monopage.
		// they also may make sense for vertex specializaiton.
		GeometryPageHandle page{};
		VmaVirtualAllocation index{};
		VmaVirtualAllocation vertex{};

	} alloc;

	glm::vec3 aabbMin;
	float     sphereRadius;
	glm::vec3 aabbMax;
	float     spherePad;

	// This is not good 
	uint32_t lods = 1;

	std::vector<uint32_t> lod_offsets;
	std::vector<Surface> surfaces;

};

using MaterialHandle = Handle<struct Material>;

struct Material {
	// RasterSettings. BRDF and else
	enum MaterialModel {
		//RasterSettings raster;
	};

	struct Textures {
		TextureHandle diffuse{};
		TextureHandle specular{};
		TextureHandle opacity{};
	} textures;

	// --------------------------------------------

	//GpuBlob
	struct Values {
		// Color *=
		// TODO - to 0.0f
		glm::vec4 ambient = glm::vec4(0.06f);
		glm::vec4 diffuse = glm::vec4(1.0f);

		// This can easily go outdated form .textures. bad design!!!
		int tex[4] = { -1, -1, -1, -1 }; //diffuse ,ambient, alpha // unused 
	} values;
	static_assert(sizeof(Values) % 16 == 0);

	PipelineHandle pipeline{};

	// This could be PipelineKey blob or somehting like that. not the handle to the resource.
	// Materials dont own the pipeline really

	//Material model maybek key.- This is to translate gltf materials into my own, using codified spec MaterialModel 
	PipelineHandle GetPipelineFromMaterialModel(Context* ctx, MaterialModel material_model);

};

//TODO
[[nodiscard]] PipelineHandle GetPipelineFromMaterialModel(Context* ctx, Material::MaterialModel material_model) {
	// Obviusly incomplete 
	return ctx->pipelines_.getHandle(0);
};

struct MeshManager {
	Context* ctx = nullptr;

	//MeshManager(Context* context_)
	//	:
	//	ctx(context_)
	//{

	//};

	uint32_t vertex_cursor = 0;

	Pool<Mesh, Mesh> meshes;
	Pool<Material, Material> materials;

	Pool<GeometryPage, GeometryPage> geometry_pages;
	GeometryPageHandle current_geometry_page;

	MeshHandle UploadVertexData(MeshUploadInput& mesh_data);
	MaterialHandle CreateMaterial(Material::Textures textures);

	void InitalizeGeoBuffers();

	// TODO. customizable vertex formats via spec
	void BindVertex(VkCommandBuffer commandBuffer) {

		auto page = geometry_pages.get(current_geometry_page);

		VkBuffer bufs[] = {
			ctx->buffers_.get(page->vertexPosBuffer)->buffer,
			ctx->buffers_.get(page->vertexShadingBuffer)->buffer
		};

		VkDeviceSize offs[] = { 0, 0 };

		vkCmdBindVertexBuffers(commandBuffer, 0, 2, bufs, offs);
		vkCmdBindIndexBuffer(commandBuffer, ctx->buffers_.get(page->indexBuffer)->buffer, 0, VK_INDEX_TYPE_UINT32);
	};

};



[[nodiscard]] MaterialHandle MeshManager::CreateMaterial(Material::Textures textures /*Type/Pipeline type*/) {
	assert(textures.diffuse.valid() && "Invalid handle for diffuse");

	Material mat{};
	mat.textures.diffuse = textures.diffuse;
	mat.values.tex[0] = textures.diffuse.index();

	//mat.values.tex[0] = textures.diffuse.index();
	//mat.values.tex[0] = textures.diffuse.index();
	//mat.values.tex[0] = textures.diffuse.index();

	return  materials.create(std::move(mat));

}


[[nodiscard]] MeshHandle MeshManager::UploadVertexData(MeshUploadInput& geometry_data)
{

	const uint32_t vcount = static_cast<uint32_t>(geometry_data.vertices.size());
	auto page = geometry_pages.get(current_geometry_page);

	Mesh mesh_desc{};
	mesh_desc.alloc.page = current_geometry_page;


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

		if (vmaVirtualAllocate(page->vertex_units_blk, &aV, &mesh_desc.alloc.vertex, &baseVertex) != VK_SUCCESS) {
			// Not enough contiguous vertex space in this page (or fragmented)
			return {};
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
		sizeof(IndexT) * static_cast<VkDeviceSize>(geometry_data.indices.size());

	VkDeviceSize index_offset_bytes = 0;

	{
		VmaVirtualAllocationCreateInfo aIdx{};
		aIdx.size = indexBytes;
		aIdx.alignment = sizeof(IndexT);

		if (vmaVirtualAllocate(page->index_blk /*or index_blk*/, &aIdx, &mesh_desc.alloc.index, &index_offset_bytes) != VK_SUCCESS) {
			// Roll back vertex allocation
			vmaVirtualFree(page->vertex_units_blk, mesh_desc.alloc.vertex);
			return {};
		}

		auto staging_h = ctx->CreateStagingBuffer(geometry_data.indices.data(), indexBytes);
		auto staging = ctx->buffers_.get(staging_h);

		ctx->copyBuffer(staging->buffer, ctx->buffers_.get(page->indexBuffer)->buffer, indexBytes, 0, index_offset_bytes);
		destroy(ctx, staging_h);
	}

	// ------------------------------------------
	// 2) Split vertices into two CPU arrays
	// ------------------------------------------
	std::vector<VertexPos> pos(vcount);
	std::vector<VertexShading> shd(vcount);

	for (uint32_t i = 0; i < vcount; ++i) {
		const Vertex& v = geometry_data.vertices[i];

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

		auto staging_h = ctx->CreateStagingBuffer(pos.data(), posBytes);
		auto staging = ctx->buffers_.get(staging_h);

		ctx->copyBuffer(
			staging->buffer,
			ctx->buffers_.get(page->vertexPosBuffer /*or vertex_buffer*/)->buffer,
			posBytes,
			0,
			pos_byte_offset
		);

		destroy(ctx, staging_h);
	}

	// ------------------------------------------
	// 4) Upload shading to binding 1 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize shdBytes = sizeof(VertexShading) * static_cast<VkDeviceSize>(vcount);

		auto staging_h = ctx->CreateStagingBuffer(shd.data(), shdBytes);
		auto staging = ctx->buffers_.get(staging_h);

		ctx->copyBuffer(
			staging->buffer,
			ctx->buffers_.get(page->vertexShadingBuffer /*or vertex_shading_buffer*/)->buffer,
			shdBytes,
			0,
			shd_byte_offset
		);

		destroy(ctx, staging_h);
	}

	// ------------------------------------------
	// 5) Surface (bind-once style)
	// ------------------------------------------
	Surface ds{};
	ds.index_count = static_cast<uint32_t>(geometry_data.indices.size());
	ds.first_index = static_cast<uint32_t>(index_offset_bytes / sizeof(IndexT));
	ds.base_vertex = static_cast<int32_t>(baseVertex);

	// IMPORTANT: store allocs somewhere if you want to free later.
	// e.g. ds.vertex_alloc = vtx_alloc; ds.index_alloc = index_alloc; (requires fields)

	mesh_desc.lods = 1;
	mesh_desc.lod_offsets = { 0 };
	mesh_desc.surfaces = { ds };
	mesh_desc.aabbMin = geometry_data.aabb_min;
	mesh_desc.aabbMax = geometry_data.aabb_max;
	//mesh_desc.alloc.page = current_geometry_page;

	return meshes.create(std::move(mesh_desc));
}

void MeshManager::InitalizeGeoBuffers() {

	{
		uint32_t totalVertexBytes = 512u * 1024u * 1024u;
		uint32_t totalIndexBytes = 512u * 1024u * 1024u;

		// Create the actual GPU buffers (these are the page's buffers)
		DeviceBufferHandle vb_pos = ctx->createBuffer(
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		);

		DeviceBufferHandle vb_shd = ctx->createBuffer(
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		);

		DeviceBufferHandle ib = ctx->createBuffer(
			totalIndexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		);

		// Single page.
		GeometryPage page{};
		page.vertexPosBuffer = vb_pos;
		page.vertexShadingBuffer = vb_shd;
		page.indexBuffer = ib;

		// Capacity in vertices must satisfy both streams
		page.max_vertices = std::min(
			totalVertexBytes / uint32_t(sizeof(VertexPos)),
			totalVertexBytes / uint32_t(sizeof(VertexShading))
		);

		// Index allocator (bytes)
		{
			VmaVirtualBlockCreateInfo ci{};
			ci.size = totalIndexBytes;
			VK_CHECK(vmaCreateVirtualBlock(&ci, &page.index_blk));
		}

		// Vertex allocator (VERTEX UNITS!)
		{
			VmaVirtualBlockCreateInfo vu{};
			vu.size = page.max_vertices; // vertices, not bytes
			VK_CHECK(vmaCreateVirtualBlock(&vu, &page.vertex_units_blk));
		}

		current_geometry_page = geometry_pages.create(std::move(page));

	}

}

void destroy(MeshManager* ctx, MeshHandle handle);
void destroy(MeshManager* ctx, GeometryPageHandle handle);
