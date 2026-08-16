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


        // -------------------------------------------------------------------------
        // Buffers used by renderer
        // -------------------------------------------------------------------------

        BufferHandle GetPositionBuffer() const {
            return vertexPosBuffer_;
        }

        BufferHandle GetShadingBuffer() const {
            return vertexShadingBuffer_;
        }

        BufferHandle GetIndexBuffer() const {
            return indexBuffer_;
        }
        void BindStaticMeshBuffers(VkCommandBuffer cmd) const;

  
    //private:
        void CreateGeometryBuffers();
        void DestroyGeometryBuffers();
 
    //private:
        // -------------------------------------------------------------------------
        // Physical GPU buffers
        // -------------------------------------------------------------------------
        BufferHandle vertexPosBuffer_{};
        BufferHandle vertexShadingBuffer_{};
        BufferHandle indexBuffer_{};

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