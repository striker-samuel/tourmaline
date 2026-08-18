#pragma once

#include <string>
//#include <unordered_map>

#include <Engine/Vulkan/core.h>
#include <Engine/Shared/common.h>
#include <Engine/Renderer/mesh.h>

namespace smvk {

	//struct Cluster;
	//template<typename HandleType>
	//struct AssetRef {
	//	std::string uri;
	//	HandleType handle{};
	//	Cluster* cluster = nullptr;
	//};

	using ClusterStaticMeshHandle = smvk::Handle<StaticMesh>;
	using ClusterMaterialHandle = smvk::Handle<struct Material>;

	struct Cluster {

		explicit Cluster(VulkanResourceManager& ctx);
		~Cluster();
		Cluster(const Cluster&) = delete;
		Cluster& operator=(const Cluster&) = delete;

		VulkanResourceManager& ctx_;

		[[nodiscard]] ClusterStaticMeshHandle UploadStaticMesh(const StaticMeshUpload& data);
		[[nodiscard]] ClusterMaterialHandle CreateMaterial(TextureHandle diffuse);
		void BindStaticMeshBuffers(VkCommandBuffer cmd) const;

		void AllocateGeometryBuffers();
		void DestroyGeometryBuffers();

		void DestroyMesh(ClusterStaticMeshHandle handle);
		void DestroyMaterial(ClusterStaticMeshHandle handle);

		//private:
			// -------------------------------------------------------------------------
			// Physical GPU buffers
			// -------------------------------------------------------------------------
		Holder<BufferHandle> vertexPosBuffer_{};
		Holder<BufferHandle> vertexShadingBuffer_{};
		Holder<BufferHandle> indexBuffer_{};

		// -------------------------------------------------------------------------
		// Virtual allocations
		// -------------------------------------------------------------------------
		VmaVirtualBlock vertexBlock_ = VK_NULL_HANDLE;
		VmaVirtualBlock indexBlock_ = VK_NULL_HANDLE;
		// -------------------------------------------------------------------------
		// Resources
		// -------------------------------------------------------------------------

		Pool<StaticMesh, StaticMesh> staticMeshes;
		Pool<Material, Material> materials;

		std::vector<Holder<TextureHandle>> textures;

	};


} // namespace smvk