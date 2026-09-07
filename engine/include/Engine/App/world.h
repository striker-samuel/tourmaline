#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/mesh_optimizer.h>
//#include <Engine/Utils/Bvh.h>
#include <Engine/Scene/scene.h>

namespace smvk {

	class PakReader;

	enum struct CullType {
		CPU
		//, GPU
	};

	struct CullConfiguration {
		CullType cullType = CullType::CPU;
		glm::mat4 viewProj{};
	};

	typedef enum IteratorFlagBits {
		IteratorFlagBit_None = 0,
		IteratorFlagBit_Renderable = 1u << 0,
		IteratorFlagBit_Opaque = 1u << 1,
		IteratorFlagBit_Transparent = 1u << 2,
		IteratorFlagBit_PassedCullTest = 1u << 3,
		IteratorFlagBit_StaticVertexFmt = 1u << 4,
		IteratorFlagBit_CastShadow = 1u << 5,
		IteratorFlagBit_LightDirectional = 1u << 6,
		IteratorFlagBit_LightSpot = 1u << 7,
		IteratorFlagBit_LightPoint = 1u << 8,
		// Set when the node sits inside a static subtree (see Scene::
		// staticRoot/staticNode). Static nodes skip per-frame transform
		// updates and dynamic BVH rebuilds; collapsed mega-mesh nodes keep
		// this flag as well.
		IteratorFlagBit_Static = 1u << 9,
		//IteratorFlagBit_Skinned,
		//IteratorFlagBit_Static
	} IteratorFlagBits;
	typedef uint32_t IteratorFlags;

	// World is the single runtime scene graph. It is *not* an imported scene.
	// It owns (has-a) the merged Scene hierarchy, owns the Clusters that hold
	// the actual mesh/material/texture resources, and resolves per-node mesh/
	// material indices stored in the Scene to the owning Cluster + handle.
	struct World {
		explicit World() {};

		// Merged runtime scene graph. Imported Cluster scenes are folded into
		// this via ResolveAndFixup_clusters().
		Scene scene;

		mutable bool dirtyCull = true;
		std::vector<IteratorFlags> flags; // indexed by node
		std::vector<std::unique_ptr<Cluster>> clusters;

		// Resource lookup into clusters
		struct MeshLocation {
			Cluster* cluster;
			ClusterStaticMeshHandle handle;
		};
		struct MaterialLocation {
			Cluster* cluster;
			ClusterMaterialHandle handle;
		};
		struct TextureLocation {
			Cluster* cluster;
			ClusterTextureHandle handle;
		};
		// Indexed by the mesh/material index stored in scene.meshForNode /
		// scene.materialForNode. Each entry points back to the owning
		// Cluster so resources stay owned by clusters.
		std::vector<MeshLocation> meshLocations;
		std::vector<MaterialLocation> materialLocations;
		std::vector<TextureLocation> textureLocations;
		// Per-mesh LOD markers parallel to meshLocations (cook-time chain:
		// LOD0 + "/lodN" siblings with world-scaled error — tooling/ImGui
		// only, the renderer ignores it).
		std::vector<LodInfo> meshLodInfos;

		Cluster& LoadCluster(ResourceManager& ctx);
		// Deserializes a cooked pak (PakReader over caller-owned bytes) into
		// a new registered Cluster. Throws std::runtime_error on failure.
		// The caller issues ctx.FlushUploads() after ResolveAndFixup.
		Cluster& LoadClusterFromPak(ResourceManager& ctx, const class PakReader& reader);
		void UnloadCluster(smvk::Cluster&);

		void RegisterCluster(std::unique_ptr<Cluster> cluster);
		std::unique_ptr<Cluster> UnregisterCluster(Cluster&);
		void ResolveAndFixup_clusters();

		smvk::StaticMesh* GetMesh(uint32_t meshIndex);
		smvk::Material* GetMaterial(uint32_t materialIndex);
		// LOD marker for tooling (LOD0 + cooked "/lodN" siblings; the
		// renderer still ignores lodIndex/screenThreshold). Null when the
		// index is out of range. Never consulted by the renderer.
		const LodInfo* GetLodInfo(uint32_t meshIndex) const;

		// First-hit URI resolution across clusters in registration order
		// (mirrors the VFS overlay rule). Used for cross-pak references.
		std::optional<MeshLocation> ResolveMeshAcrossClusters(const std::string& uri) const;
		std::optional<MaterialLocation> ResolveMaterialAcrossClusters(const std::string& uri) const;

		//void NewFrame();
			//if (dirtyCull && flags.size() == scene.hierarchy.size())
			//	Cull(CullConfiguration{});
		//void EndFrame();

		template<typename Func>
		void forEachNode(IteratorFlags requiredFlags, Func&& func)
		{
			for (uint32_t i = 0; i < scene.hierarchy.size(); ++i) {
				const IteratorFlags nf =
					(i < flags.size()) ? flags[i]
					: static_cast<IteratorFlags>(IteratorFlagBit_None);

				if ((nf & requiredFlags) != requiredFlags)
					continue;

				func(static_cast<NodeId>(i));
			}
		}

	private:
		void Cull(CullConfiguration config);
	};

};
