
#pragma once


#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>

namespace smvk {

	struct World;

	using ClusterStaticMeshHandle = Handle<StaticMesh>;
	using ClusterMaterialHandle = Handle<Material>;
	using ClusterTextureHandle = Handle<struct Texture>;

	struct Cluster {
		explicit Cluster(VulkanResourceManager& ctx);
		~Cluster();
		Cluster(const Cluster&) = delete;
		Cluster& operator=(const Cluster&) = delete;

		ResourceManagerImpl& ctx_;
		Scene scene;
		World* world = nullptr;
		Cluster* sibling = nullptr;
		// linked list for import chain if multiple clusters belong to one input file.

		Pool<StaticMesh, StaticMesh> staticMeshes;
		//Pool<SkinnedMesh, SkinnedMesh> skinnedMesh;
		Pool<Material, Material> materials;
		Pool<Texture, Holder<TextureHandle>> textures;

		ClusterStaticMeshHandle UploadStaticMesh(const StaticMeshUpload& data);
		ClusterMaterialHandle CreateMaterial(TextureHandle diffuse);

		void DestroyMesh(ClusterStaticMeshHandle handle);
		void DestroyMaterial(ClusterMaterialHandle handle);

	private:
		//std::unordered_map<std::string, ClusterStaticMeshHandle> meshByUri;
		//std::unordered_map<std::string, ClusterMaterialHandle> materialByUri;
		//std::unordered_map<std::string, TextureHandle> textureByUri;
	};

	template<typename HandleT>
	struct AssetRef {
		std::string uri;

		Cluster* cluster = nullptr;
		HandleT handle{};

		bool resolved() const {
			return cluster != nullptr && handle.valid();
		}

		void clearResolution() {
			cluster = nullptr;
			handle = {};
		}
	};
}



