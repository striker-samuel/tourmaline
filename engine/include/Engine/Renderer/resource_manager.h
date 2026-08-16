#pragma once

#include <Engine/Vulkan/core.h>
#include <Engine/Shared/common.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/mesh.h>


namespace smvk {

	// using prefix CLuster to differentiate them from Vulkan resources like buffers or images
	using ClusterStaticMeshHandle = smvk::Handle<struct StaticMesh>;
	using ClusterSkinnedMeshHandle = smvk::Handle<struct SkinnedMesh>;
	using ClusterMaterialHandle = smvk::Handle<struct Material>;

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

	struct Cluster {

		Cluster(smvk::ResourceContext& ctx);
		~Cluster() {};

		ClusterStaticMeshHandle UploadData(void* data, size_t vertexCount);

		ClusterMaterialHandle CreateMaterial(smvk::TextureHandle diffuse);
		smvk::TextureHandle GetOrCreateTexture(std::string uri, ktxTexture2& tex);

		//static constexpr uint32_t MAX_VERTEX_BUFFERS = 4;
		//smvk::Holder<smvk::BufferHandle> buffers[MAX_VERTEX_BUFFERS];
		//size_t bufferCount = 0;


	private:
		smvk::ResourceContext& ctx_;
		smvk::Pool<StaticMesh, StaticMesh> staticMeshes;
		//smvk::Pool<SkinnedMesh, SkinnedMesh> skinnedMeshes;
		smvk::Pool<Material, Material> materials;
	};

	template <typename HandleType>
	struct AssetRef {
		std::string uri{}; // DAM identifier
		HandleType handle{};
		Cluster* cluster = nullptr;
	};

};
