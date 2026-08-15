#pragma once

#include <Engine/Vulkan/core.h>
#include <Engine/Shared/common.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/mesh.h>

using MeshHandle = smvk::Handle<struct Mesh>;
using MaterialHandle = smvk::Handle<struct Material>;



struct Material {
	struct Values {
		// Color *=
		// TODO - to 0.0f
		glm::vec4 ambient = glm::vec4(0.06f);
		glm::vec4 diffuse = glm::vec4(1.0f);
		int tex[4] = { -1, -1, -1, -1 }; //diffuse ,ambient, alpha, normals
	} values;
	static_assert(sizeof(Values) % 16 == 0);
};

struct MeshManager {
	smvk::ResourceContext& ctx_;
	MeshManager(smvk::ResourceContext& ctx);
	~MeshManager() {};

	MeshHandle UploadVertexData(Mesh::DataUpload& mesh_data);
	MaterialHandle CreateMaterial(smvk::TextureHandle diffuse);
	smvk::TextureHandle GetOrCreateTexture(std::string uri, ktxTexture2& tex);

	smvk::BufferHandle GetIndexBuffer();
	std::vector<smvk::BufferHandle> GetVertexBuffers();

	void BindMesh(
		VkCommandBuffer commandBuffer,
		const Mesh& mesh
	) {
		auto vertexBuffers = mesh.vertexBuffers();

		std::vector<VkBuffer> buffers;
		std::vector<VkDeviceSize> offsets;

		buffers.reserve(vertexBuffers.size());
		offsets.reserve(vertexBuffers.size());

		for (const auto& vertexBuffer : vertexBuffers) {
			buffers.push_back(vertexBuffer.buffer.vk_buffer);
			offsets.push_back(vertexBuffer.offset);
		}

		vkCmdBindVertexBuffers(
			commandBuffer,
			0,
			static_cast<uint32_t>(buffers.size()),
			buffers.data(),
			offsets.data()
		);

		if (auto* index = mesh.indexBuffer()) {
			vkCmdBindIndexBuffer(
				commandBuffer,
				index->buffer.vk_buffer,
				index->offset,
				index->indexType
			);
		}
	}
	//void BindIndexVertexBuffers(VkCommandBuffer commandBuffer) {
	//	auto indexBuffer = ctx_.buffers_.get(GetIndexBuffer());
	//	auto vertexBuffers = GetVertexBuffers();
	//	std::vector<VkBuffer> bufs{};
	//	bufs.reserve(2);
	//	for (auto han : vertexBuffers) {
	//		bufs.push_back(
	//			ctx_.buffers_.get(han)->vk_buffer
	//		);
	//	}
	//	VkDeviceSize offs[] = { 0, 0 };
	//	vkCmdBindIndexBuffer(commandBuffer, indexBuffer->vk_buffer, 0, VK_INDEX_TYPE_UINT32);
	//	vkCmdBindVertexBuffers(commandBuffer, 0, bufs.size(), bufs.data(), offs);
	//};

	//private:
	void InitalizeGeoBuffers();
	//private:
	std::map<std::string, smvk::TextureHandle> textureCache;
	smvk::Pool<Mesh, Mesh> meshes;
	smvk::Pool<Material, Material> materials;

	smvk::Holder<smvk::BufferHandle> indexBuffer;
	smvk::Holder<smvk::BufferHandle> vertexPosBuffer;
	smvk::Holder<smvk::BufferHandle> vertexShadingBuffer;

	//uint32_t max_vertices = 0;
	VmaVirtualBlock index_blk{};
	VmaVirtualBlock vertex_units_blk{};
	uint32_t vertex_cursor = 0;
};

void destroy(MeshManager* ctx, MeshHandle handle);
