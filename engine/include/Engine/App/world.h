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
	//maybe specialize in MeshIteratorFLag, MaterialIteratorFLag... else?
	// ScriptIteratorFlag
	enum class IteratorFlag : uint32_t {
		// Renderable
		// Opaque
		// Transparent
		NONE = 0,
		MESH = 1u << 0,
		MATERIAL = 1u << 1,
		TEXTURE = 1u << 2,

		ALL = MESH | MATERIAL | TEXTURE
	};

	constexpr IteratorFlag operator|(IteratorFlag a, IteratorFlag b)
	{
		return static_cast<IteratorFlag>(
			static_cast<uint32_t>(a) |
			static_cast<uint32_t>(b)
			);
	}

	constexpr IteratorFlag operator&(IteratorFlag a, IteratorFlag b)
	{
		return static_cast<IteratorFlag>(
			static_cast<uint32_t>(a) &
			static_cast<uint32_t>(b)
			);
	}

	constexpr bool HasFlag(IteratorFlag flags, IteratorFlag flag)
	{
		return (static_cast<uint32_t>(flags) &
			static_cast<uint32_t>(flag)) != 0;
	}
	struct World : Scene // dont quite like this
	{
		explicit World() {};
		//Scene scene;

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
		void forEach(Func&& func, IteratorFlag flags)
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
		void Cull(CullType cullType, glm::mat4 viewProj);
	};


};
