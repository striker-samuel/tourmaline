#pragma once

#include <Engine/Vulkan/core.h>
#include <Engine/Shared/common.h>

#include <Engine/Scene/scene.h>
#include <Engine/Renderer/vertex.h>

using MeshHandle = smvk::Handle<struct Mesh>;
using MaterialHandle = smvk::Handle<struct Material>;

struct Mesh {
	struct DataUpload {
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
	//VertexDeclaration
	struct Alloc {
		VmaVirtualAllocation index{};
		VmaVirtualAllocation vertex{};
	} vma_alloc;
	glm::vec3 aabbMin;
	float     sphereRadius;
	glm::vec3 aabbMax;
	float     spherePad;
	// removed lods for now.
	std::vector<Surface> surfaces;
};


struct Material {
	// this is bad
	struct Textures {
		smvk::TextureHandle diffuse{};
		smvk::TextureHandle specular{};
		smvk::TextureHandle opacity{};
		smvk::TextureHandle normal{};
	} textures_;
	struct Values {
		// Color *=
		// TODO - to 0.0f
		glm::vec4 ambient = glm::vec4(0.06f);
		glm::vec4 diffuse = glm::vec4(1.0f);
		// This can easily go outdated form .textures. bad design!!!
		int tex[4] = { -1, -1, -1, -1 }; //diffuse ,ambient, alpha, normals
	} values;
	static_assert(sizeof(Values) % 16 == 0);
};



// this should just instead be moved to mesh class or maybe even level.
struct MeshManager {
	smvk::Context& ctx_;
	MeshManager(smvk::Context& ctx);
	~MeshManager() {};

	uint32_t vertex_cursor = 0;
	//uint32_t max_vertices = 0;
	VmaVirtualBlock index_blk{};
	VmaVirtualBlock vertex_units_blk{}; // this is hacked to be 1wide to index into vertexPos and shadingBuffe

	smvk::Pool<Mesh, Mesh> meshes;
	smvk::Pool<Material, Material> materials;
	// this is bad because there are more vertex formats that may not be like this
	smvk::Holder<smvk::BufferHandle> indexBuffer;
	smvk::Holder<smvk::BufferHandle> vertexPosBuffer;
	smvk::Holder<smvk::BufferHandle> vertexShadingBuffer;

	MeshHandle UploadVertexData(Mesh::DataUpload& mesh_data);
	MaterialHandle CreateMaterial(Material::Textures textures);
	smvk::TextureHandle GetOrCreateTexture(std::string uri, ktxTexture2& tex);

	std::map<std::string, smvk::TextureHandle> textureCache;

	void InitalizeGeoBuffers();

	std::vector<smvk::BufferHandle> GetVertexBuffers();
};

