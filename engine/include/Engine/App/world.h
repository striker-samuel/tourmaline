#pragma once
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
//#include <Engine/Utils/Bvh.h>
#include <Engine/Scene/scene.h>

namespace smvk {

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


		//AssetRef // Uri mapped assetref to avoid copy of same assets. this needs cooking

		// Merged runtime scene graph. Imported Cluster scenes are folded into
		// this via ResolveAndFixup_clusters().
		Scene scene;

		mutable bool dirtyCull = true;
		std::vector<IteratorFlags> flags; // indexed by node

		std::vector<std::unique_ptr<smvk::Cluster>> clusters;

		// Resource lookup into clusters
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
		// Indexed by the mesh/material index stored in scene.meshForNode /
		// scene.materialForNode. Each entry points back to the owning
		// Cluster so resources stay owned by clusters.
		std::vector<MeshLocation> meshLocations;
		std::vector<MaterialLocation> materialLocations;
		std::vector<TextureLocation> textureLocations;

		smvk::Cluster& LoadCluster(VulkanResourceManager& ctx);
		void UnloadCluster(smvk::Cluster&);

		void RegisterCluster(std::unique_ptr<smvk::Cluster> cluster);
		std::unique_ptr<smvk::Cluster> UnregisterCluster(smvk::Cluster&);
		void ResolveAndFixup_clusters();

		smvk::StaticMesh* GetMesh(uint32_t meshIndex);
		smvk::Material* GetMaterial(uint32_t materialIndex);

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
