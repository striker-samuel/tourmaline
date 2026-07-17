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

Mesh::DataUpload SurfaceDataFromGltfPrimitive(const tinygltf::Model& model,const tinygltf::Primitive& primitive);

#define DEFAULT_MATERIAL_INDEX 0;
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

	uint32_t getOrCreateMaterial(int gltfMaterialIndex) {
		if (gltfMaterialIndex < 0) {
			return DEFAULT_MATERIAL_INDEX;
		}

		// glTF guarantees material indices are within model.materials
		// (assuming the file is valid). If you want belt-and-suspenders:
		if ((size_t)gltfMaterialIndex >= materialCache.size()) {
			return DEFAULT_MATERIAL_INDEX;
		}

		uint32_t& slot = materialCache[(size_t)gltfMaterialIndex];
		if (slot != kInvalidMat) {
			return slot;
		}

		// NOTE: Avoid storing baseDir as a dangling c_str(). Build it here.
		const std::string baseDir = gltfModelPath.parent_path().string();

		// TODO : This should be done onlt once instead of here!
		auto mat = ExtractMaterials(model)[gltfMaterialIndex];

		smvk::TextureHandle colorTexHandle{};

		// TODO : Upload all textures
		{

			auto color_uri = UriFromTexture(model, *mat.baseColorTex);
			ktxTexture2* ktxTex = nullptr;
			KTX_error_code res = ktxTexture2_CreateFromNamedFile(
				(baseDir.c_str() / color_uri).string().c_str(),
				KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, // <<–– correct flags
				&ktxTex
			);

			if (res != KTX_SUCCESS) {
				throw std::runtime_error("KTX load failed");
			}

			//colorTexHandle = manager_.ctx_.CreateTexture(color_uri.string(), *ktxTex);
			colorTexHandle = manager_.ctx_.CreateTexture(*ktxTex);
			ktxTexture2_Destroy(ktxTex);
		}

		return manager_.CreateMaterial(
			{
				.diffuse = colorTexHandle
			}
		).index();

	}

	uint32_t getOrCreateMesh(tinygltf::Primitive primitive) {
		auto mdata = SurfaceDataFromGltfPrimitive(model, primitive);
		//return createMeshCallback_(&mdata).index();
		return manager_.UploadVertexData(mdata).index();
	}

	void importNodeRecursive(int gltfNodeIndex, int parentSceneNode, int level) {
		const tinygltf::Node& gltfNode = model.nodes[gltfNodeIndex];

		// Always create a "group" node that owns name + transform and preserves hierarchy.
		const int groupNode = addNode(scene, parentSceneNode, level);

		{
			const int nameIndex = (int)scene.nodeNames.size();
			scene.nodeNames.push_back(gltfNode.name.empty() ? "Node" : gltfNode.name);
			scene.nameForNode[groupNode] = nameIndex;
		}

		scene.localTransform[groupNode] = GetNodeLocalTransform(gltfNode);
		scene.globalTransform[groupNode] = glm::mat4(1.0f); // resolved later

		// Unroll primitives into separate meshes (your current approach).
		if (gltfNode.mesh >= 0) {
			const tinygltf::Mesh& gltfMesh = model.meshes[gltfNode.mesh];

			for (int primIndex = 0; primIndex < (int)gltfMesh.primitives.size(); ++primIndex) {
				const tinygltf::Primitive& primitive = gltfMesh.primitives[primIndex];

				const int primNode = addNode(scene, groupNode, level + 1);

				scene.localTransform[primNode] = glm::mat4(1.0f);
				scene.globalTransform[primNode] = glm::mat4(1.0f);

				scene.meshForNode[primNode] = getOrCreateMesh(primitive);
				scene.materialForNode[primNode] = getOrCreateMaterial(primitive.material);

				markAsChanged(scene, primNode);
			}
		}

		// Recurse children under the group node to preserve hierarchy.
		for (int child : gltfNode.children) {
			importNodeRecursive(child, groupNode, level + 1);
		}
	}
};

