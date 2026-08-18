#include <Engine/Renderer/cluster.h>

struct World {
    std::vector<std::unique_ptr<smvk::Cluster>> clusters;

    struct MeshLocation {
        smvk::Cluster* cluster;
        smvk::ClusterStaticMeshHandle handle;
    };

    struct MaterialLocation {
        smvk::Cluster* cluster;
        smvk::ClusterMaterialHandle handle;
    };

    struct TextureLocation {
        smvk::Cluster* cluster;
        smvk::ClusterTextureHandle handle;
    };

    std::unordered_map<std::string, MeshLocation> meshes;
    std::unordered_map<std::string, MaterialLocation> materials;
    std::unordered_map<std::string, TextureLocation> textures;

    smvk::Cluster& LoadCluster(...);
    void UnloadCluster(smvk::Cluster&);

    void RegisterCluster(smvk::Cluster&);
    void UnregisterCluster(smvk::Cluster&);

    void ResolveAndFixupClusters();
};