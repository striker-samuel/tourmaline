
#pragma once

#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>

namespace smvk {

	using ClusterStaticMeshHandle = Handle<StaticMesh>;
	using ClusterMaterialHandle = Handle<Material>;
	using ClusterTextureHandle = Handle<Texture>;

	struct Cluster {
		explicit Cluster(VulkanResourceManager& ctx);
		~Cluster();

		Cluster(const Cluster&) = delete;
		Cluster& operator=(const Cluster&) = delete;

		VulkanResourceManager& ctx_;

		Scene scene;
		//
		Pool<StaticMesh, StaticMesh> staticMeshes;
		Pool<Material, Material> materials;
		Pool<Texture, Holder<TextureHandle>> textures;

		ClusterStaticMeshHandle UploadStaticMesh(const StaticMeshUpload& data);
		ClusterMaterialHandle CreateMaterial(TextureHandle diffuse);
		void DestroyMesh(ClusterStaticMeshHandle handle);
		void DestroyMaterial(ClusterMaterialHandle handle);

	private:
		void AllocateGeometryBuffers();
		void BindStaticMeshBuffers(VkCommandBuffer cmd) const;
		//
		std::unordered_map<std::string, ClusterStaticMeshHandle> meshByUri;
		std::unordered_map<std::string, ClusterMaterialHandle> materialByUri;
		std::unordered_map<std::string, TextureHandle> textureByUri;
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


