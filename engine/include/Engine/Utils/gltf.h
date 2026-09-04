#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include <tiny_gltf.h>

#include <Engine/App/cluster.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Vfs/vfs.h>

struct GltfToSceneImporter
{
	GltfToSceneImporter(
		smvk::Cluster& inCluster,
		const tinygltf::Model& inModel,
		std::filesystem::path inGltfModelPath
	);
	void import(int gltfNodeIndex) {
		importNodeRecursive(gltfNodeIndex, 0, 1);
	}
	// Multicore texture read + upload: decodes/transcodes all KTX2 files
	// referenced by the scene's materials on worker threads and uploads
	// them in parallel. Fills textureCache so per-material creation hits
	// the cache. Safe to call once before importing nodes; missing entries
	// still fall back to sequential load in getOrCreateMaterial.
	void preloadTextures();
	// Cook/render configuration. All reads go through the texture source
	// (host files by default, or a VFS overlay for the cooker); every
	// created asset is stamped with its VFS-path asset URI (see
	// Asset/asset_registry.h) so paks round-trip the lookup maps.
	// The texture source resolves a glTF-relative image uri (posix) to
	// decoded source bytes plus the VFS identity path for that texture.
	struct TextureSource {
		virtual ~TextureSource() = default;
		virtual std::optional<std::vector<uint8_t>> ReadBytes(const std::string& imageUri) = 0;
		virtual std::string ResolveIdentity(const std::string& imageUri) const = 0;
	};
	void setTextureSource(std::shared_ptr<TextureSource> source);
	void setModelUriBase(const std::string& vfsPath);
	void setMeshOptConfig(const smvk::MeshOptConfig& config);
	// Meshlet cook config (disabled by default). When enabled,
	// getOrCreateMesh builds meshlets from the optimized upload and stamps
	// them on the Cluster keyed by mesh URI.
	void setMeshletConfig(const smvk::MeshletConfig& config);
private:
	smvk::ClusterMaterialHandle
		getOrCreateMaterial(int gltfMaterialIndex);

	smvk::ClusterStaticMeshHandle
		getOrCreateMesh(
			int gltfMeshIndex,
			int primitiveIndex
		);
	void importNodeRecursive(
		int gltfNodeIndex,
		int parentSceneNode,
		int level
	);
	smvk::Cluster& cluster;
	const tinygltf::Model& model;
	std::filesystem::path gltfModelPath;
	std::shared_ptr<TextureSource> textureSource;
	std::string modelUriBase; // normalized VFS file part for asset URIs
	smvk::MeshOptConfig meshOptConfig{}; // disabled by default (runtime parity)
	smvk::MeshletConfig meshletConfig{}; // disabled by default (opt-in cook)
	std::vector<smvk::ClusterMaterialHandle> materialCache;
	std::vector<std::vector<smvk::ClusterStaticMeshHandle>> meshCache;
	std::unordered_map<int, smvk::TextureHandle> textureCache; // imageIndex -> handle

	smvk::TextureHandle loadTextureForImage(int imageIndex, int textureIndex);
	smvk::ClusterMaterialHandle defaultMaterialHandle{};
};


bool LoadModelFromAsciiFile(
	tinygltf::Model& model,
	const char* filename
);

// Host-file texture source (default importer behavior): reads
// baseDir/imageUri from the host filesystem; identity is the normalized
// host-relative path so asset URIs match the VFS layout 1:1.
struct HostTextureSource final : GltfToSceneImporter::TextureSource {
	explicit HostTextureSource(std::filesystem::path baseDir);
	std::optional<std::vector<uint8_t>> ReadBytes(const std::string& imageUri) override;
	std::string ResolveIdentity(const std::string& imageUri) const override;
private:
	std::filesystem::path baseDir_;
};

// VFS texture source (cooker path): reads through an OverlayVfs mounted
// with the content tree; identity is the VFS path itself.
struct VfsTextureSource final : GltfToSceneImporter::TextureSource {
	VfsTextureSource(const smvk::OverlayVfs* vfs, std::string vfsBaseDir);
	std::optional<std::vector<uint8_t>> ReadBytes(const std::string& imageUri) override;
	std::string ResolveIdentity(const std::string& imageUri) const override;
private:
	const smvk::OverlayVfs* vfs_ = nullptr;
	std::string vfsBaseDir_;
};

// VFS-first import: reads the .gltf JSON, external .bin buffers and all
// textures through vfs (the cooker mounts its content tree), stamps asset
// URIs from modelVfsPath, and optionally runs the meshopt cook pass.
// ctx may be nullptr together with a cook-mode cluster (see
// Cluster::SetCookMode) for a fully GPU-free cook. Example:
//   auto cluster = std::make_unique<Cluster>(nullptr);
//   cluster->SetCookMode(true);
//   GltfToClusterFromVfs(nullptr, vfs, "models/Sponza/sponza.gltf", *cluster, opt);
void GltfToClusterFromVfs(
	smvk::ResourceManager* ctx,
	const smvk::OverlayVfs& vfs,
	const std::string& modelVfsPath,
	smvk::Cluster& out,
	const smvk::MeshOptConfig& meshOpt = smvk::MeshOptConfig{},
	const smvk::MeshletConfig& meshlets = smvk::MeshletConfig{});

inline void GltfToCluster(
	smvk::ResourceManager&,
	const tinygltf::Model& inModel,
	std::filesystem::path inGltfModelPath,
	smvk::Cluster& out
) {
	GltfToSceneImporter importer{
		out,
		inModel,
		inGltfModelPath
	};

	assert(out.scene.hierarchy.empty());

	// Cluster-scene root (index 0, parent = -1). Imported glTF scene roots
	// attach underneath it so addNode() always sees a valid parent index.
	const int root = addNode(out.scene, -1, 0);

	out.scene.localTransform[root] = glm::mat4(1.0f);
	out.scene.globalTransform[root] = glm::mat4(1.0f);

	// Collect the glTF top-level node indices.
	std::vector<int> gltfRoots;
	if (inModel.defaultScene >= 0 && inModel.defaultScene < (int)inModel.scenes.size()) {
		gltfRoots = inModel.scenes[inModel.defaultScene].nodes;
	}
	else if (!inModel.scenes.empty()) {
		gltfRoots = inModel.scenes.front().nodes;
	}
	else {
		// Fallback: a node is a root if it isn't referenced as anyone's child.
		std::vector<bool> referenced(inModel.nodes.size());
		for (const auto& n : inModel.nodes)
			for (int c : n.children)
				if (c >= 0 && c < (int)inModel.nodes.size())
					referenced[c] = true;
		for (int i = 0; i < (int)inModel.nodes.size(); ++i)
			if (!referenced[i]) gltfRoots.push_back(i);
	}

	importer.preloadTextures();

	for (int gltfNode : gltfRoots)
		importer.import(gltfNode);
		//importer.importNodeRecursive(gltfNode, root, 1);

	RebuildStaticCache(out.scene);
	markAsChanged(out.scene, root);
	recalculateGlobalTransforms(out.scene);
	// Flush batched uploads (256MB threshold, or manual at end of whole scene)
	if (out.ctx_)
		out.ctx_->FlushUploads();
}

