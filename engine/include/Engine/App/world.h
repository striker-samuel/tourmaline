#pragma once
#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>

namespace smvk // this builds on top of the rhi so maybe smvk is not proper-
{
	// Only supported mode now
	enum struct CullType {
		CPU
		//, GPU
	};

	typedef enum IteratorFlagBits {
		IteratorFlagBit_None = 0,
		IteratorFlagBit_Renderable = 1u << 0,
		IteratorFlagBit_Opaque = 1u << 1,
		IteratorFlagBit_Transparent = 1u << 2,
		IteratorFlagBit_PassedCullTest = 1u << 3,
		IteratorFlagBit_StaticVertexFmt = 1u << 4,
		//IteratorFlagBit_SkinnedVertexFmt = 1u << 5,
		//IteratorFlagBit_Dag = 1u << 4, ?? Maybe
	} IteratorFlagBits;
	typedef uint32_t IteratorFlags;

	struct World : Scene // dont quite like this
	{
		explicit World() {};

		mutable bool dirtyCull = true;
		std::vector<IteratorFlagBits> flags; // indexed by node

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

		template<typename Func>
		void forEachNode(IteratorFlags flags, Func&& func)
		{
			func();
			//for (uint32_t i = 0; i < objects_.size(); ++i) {
			//	auto& object = objects_[i];

			//	if (!object.occupied_)
			//		continue;

			//	if (HasFlag(flags, IteratorFlag::MESH)) {
			//		// check whether this object is a mesh
			//	}

			//	if (HasFlag(flags, IteratorFlag::MATERIAL)) {
			//		// check whether this object is a material
			//	}

			//	func(Handle<ObjectType>(
			//		i,
			//		object.gen_
			//	));
			//}
		}		//
		//BvhTree staticNodesTree{};
		//mutable bool staticBvhDone = true;
		//void BuildBvh() {
		//	treeFromScene(
		//		scene,
		//		[&](uint32_t index) -> const smvk::StaticMesh& {return this->meshes[index].obj_; }
		//	);
		//}


	private:
		void Cull(CullType cullType, glm::mat4 viewProj) {
			dirtyCull = false;
		};
	};


};
