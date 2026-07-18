#pragma once

#include <Engine/Vulkan/core.h>
#include <Engine/Shared/common.h>
#include <Engine/Utils/gltf.h> 
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


struct ktxTexture2;
struct MeshManager {
	smvk::Context& ctx_;
	MeshManager(smvk::Context& ctx);
	~MeshManager() {};
	MeshHandle UploadVertexData(Mesh::DataUpload& mesh_data);
	MaterialHandle CreateMaterial(Material::Textures textures);
	smvk::TextureHandle GetOrCreateTexture(std::string uri, ktxTexture2& tex);
	std::vector<smvk::BufferHandle> GetVertexBuffers();

private:
	void InitalizeGeoBuffers();
private:
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
Mesh::DataUpload SurfaceDataFromGltfPrimitive(const tinygltf::Model& model,const tinygltf::Primitive& primitive);

constexpr uint32_t DEFAULT_MATERIAL_INDEX = 0;
struct GltfToSceneImporter {
	Scene& scene;
	MeshManager& manager_;
	const tinygltf::Model& model;
	std::filesystem::path gltfModelPath; // full path to .gltf/.glb on disk
	static constexpr uint32_t kInvalidMat = 0xFFFFFFFFu;
	std::vector<uint32_t> materialCache;

	GltfToSceneImporter
	(
		Scene& inScene
		, MeshManager& manager 
		, const tinygltf::Model& inModel
		, std::filesystem::path inGltfModelPath
/*		, std::function<uint32_t(void* data)> createMeshCallback
		, std::function<uint32_t()> createMaterialCallback*/
	)
		: scene(inScene)
		, manager_(manager)
		, model(inModel)
		, gltfModelPath(std::move(inGltfModelPath))
		, materialCache(inModel.materials.size(), kInvalidMat)
	{}

	uint32_t getOrCreateMaterial(int gltfMaterialIndex);
	uint32_t getOrCreateMesh(tinygltf::Primitive primitive);
	void importNodeRecursive(int gltfNodeIndex, int parentSceneNode, int level);
};

