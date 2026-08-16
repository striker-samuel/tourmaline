#pragma once

#include <string>
//#include <unordered_map>

#include <Engine/Vulkan/core.h>
#include <Engine/Shared/common.h>
#include <Engine/Renderer/mesh.h>

namespace smvk {

    using ClusterStaticMeshHandle = smvk::Handle<StaticMesh>;
    using ClusterMaterialHandle = smvk::Handle<struct Material>;


    class Cluster {
    public:

        ResourceContext& ctx_;
        explicit Cluster(ResourceContext& ctx);
        ~Cluster();

        Cluster(const Cluster&) = delete;
        Cluster& operator=(const Cluster&) = delete;


        // -------------------------------------------------------------------------
        // Meshes
        // -------------------------------------------------------------------------

        [[nodiscard]]
        ClusterStaticMeshHandle UploadStaticMesh(
            const StaticMeshUpload& data
        );

        void DestroyMesh(ClusterStaticMeshHandle handle);


        // -------------------------------------------------------------------------
        // Materials
        // -------------------------------------------------------------------------

        [[nodiscard]]
        ClusterMaterialHandle CreateMaterial(TextureHandle diffuse);

        void BindStaticMeshBuffers(VkCommandBuffer cmd) const;

  
    //private:
        void CreateGeometryBuffers();
        void DestroyGeometryBuffers();
 
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

    };

    //template<typename HandleType>
    //struct AssetRef {
    //    std::string uri;
    //    HandleType handle{};
    //    Cluster* cluster = nullptr;
    //};

} // namespace smvk