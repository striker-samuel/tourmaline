#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include <tiny_gltf.h>

// ---------------------------------------------------------------------------
// Artifact-cache hook (optional): the scratch-disk store adapter the
// editor and PakCooker inject. Consulted BEFORE a texture transcode,
// fed AFTER — a hit serves exactly the payload a fresh transcode of
// the same source would produce; a miss (or no hook at all) simply
// transcodes. The cache NEVER gates correctness: .tour -> pak cooks
// succeed regardless of cache state.
// ---------------------------------------------------------------------------
struct GltfArtifactCache {
	virtual ~GltfArtifactCache() = default;
	// klass: "texture" today. sourceHash: TextureConversionHash().
	// False = miss (transcode proceeds).
	virtual bool Get(const std::string& klass, const std::string& uri,
		std::vector<uint8_t>& out) = 0;
	// Stage the cooked payload (the owner Flushes after the import
	// batch; a failed Put never blocks — the payload is used anyway).
	virtual bool Put(const std::string& klass, const std::string& uri,
		const void* payload, size_t payloadSize,
		const std::string& sourceHash, const std::string& meta) = 0;
	// Registry-only hash probe (the "is this cached entry CURRENT?"
	// gate). Default true = callers without registry access accept
	// any Get hit (legacy behavior); real adapters override with the
	// actual lookup. A Has-miss before Get prevents serving STALE
	// bytes (a different source under the same uri).
	virtual bool Has(const std::string& klass, const std::string& uri,
		const std::string& sourceHash) {
		(void)klass; (void)uri; (void)sourceHash;
		return true;
	}
};

// Asset-GUID resolver hook (optional): the editor injects its project
// asset directory so tour `reference -guid <hex>` lines resolve
// identity-first (the GUID tracks the source file across moves/
// renames through its .meta sidecar; the recorded -source path stays
// as the fallback). Null (cooker, baker, tests) keeps path-only
// resolution — GUIDs stored in tours are simply unused there.
struct GltfGuidResolver {
	virtual ~GltfGuidResolver() = default;
	// "" when unknown/invalid — the caller falls back to -source.
	virtual std::string PathForGuid(const std::string& guid) = 0;
};

// Deterministic texture-conversion cache key: FNV-1a 64 over the
// SOURCE bytes with the transfer (srgb/linear) mixed in. The ONLY
// producer and consumer of these keys is gltf_import (the store
// treats them as opaque strings), so identical inputs always map to
// the same artifact across editor sessions and cooks.
std::string TextureConversionHash(const void* data, size_t size,
	bool srgb);

#include <Engine/App/cluster.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/tour.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Vfs/vfs.h>

namespace smvk { class TextureStreamer; }

struct GltfToSceneImporter
{
	GltfToSceneImporter(
		smvk::Cluster& inCluster,
		const tinygltf::Model& inModel,
		std::filesystem::path inGltfModelPath
	);
	void setTextureStreamer(smvk::TextureStreamer* streamer) { textureStreamer_ = streamer; }	void import(int gltfNodeIndex) {
		importNodeRecursive(gltfNodeIndex, 0, 1, true);
	}
	// Attach this glTF node's subtree UNDER an existing cluster scene
	// node (nested scene references: the placeholder group node created
	// by ApplySceneReferences). Same recursion as import().
	// TOP-OF-CHAIN BASIS: every import entry (import/importUnder — each
	// independent glTF chain) applies the coordFix basis ONCE at its
	// first node (C * M_top): node locals stay RAW (identity nodes stay
	// identity — the authoring frame is honest), vertices/AABBs/
	// normals/tangents stay raw, and every node's world basis is
	// C * gltfWorld (the render lands exactly where the old per-node
	// conjugation put it).
	void importUnder(int gltfNodeIndex, int parentSceneNode, int level) {
		importNodeRecursive(gltfNodeIndex, parentSceneNode, level, true);
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
		// Host filesystem path backing imageUri when the source is
		// host-backed (sidecar cache placement); nullopt for VFS sources.
		virtual std::optional<std::filesystem::path> HostPath(
			const std::string& imageUri) const {
			(void)imageUri;
			return std::nullopt;
		}
	};
	void setTextureSource(std::shared_ptr<TextureSource> source);
	void setModelUriBase(const std::string& vfsPath);
	// Optional artifact-cache hook (see GltfArtifactCache above):
	// consulted before texture transcodes. Not owned; outlives the
	// importer call scope (owner flushes staged puts afterwards).
	void setArtifactCache(GltfArtifactCache* cache) {
		artifactCache_ = cache;
	}
	void setMeshOptConfig(const smvk::MeshOptConfig& config);
	// Meshlet cook config (disabled by default). When enabled,
	// getOrCreateMesh builds meshlets from the optimized upload and stamps
	// them on the Cluster keyed by mesh URI.
	void setMeshletConfig(const smvk::MeshletConfig& config);
	// Cooked file nodes (tour fileNode records / editor image defs):
	// per-texture colorspace + sampler intent for KTX conversion, matched
	// to glTF images by path basename then stem. Empty = usage inference
	// only (baseColor/emissive = sRGB, everything else linear).
	void setFileNodes(const std::vector<smvk::tour::FileNode>& nodes);
	// KTX auto-convert (default on): non-KTX sources transcode in-process
	// (stb decode + libktx write, `.ktx2` sidecar cached next to the
	// source). Off restores warn-and-skip for non-KTX payloads.
	void setAutoKtx(bool enabled) { autoKtx_ = enabled; }
	void setCoordFix(int mode) { coordFix_ = mode; }
	void setPreTransform(const glm::mat4& m) { preTransform_ = m; }
	// Multiplies the scene root's local transform by preTransform_ (left).
	void applyPreTransform(Scene& scene, int rootNode);
	// glTF node index -> cluster scene node id (filled by the import
	// traversal; -1 = not imported). The ASSET DERIVATION uses it to
	// retarget glTF animation targets + material bindings onto the
	// deterministic cluster ids (the same order the .mesh asset stores).
	const std::vector<int>& gltfNodeMap() const { return gltfNodeMap_; }
private:
	smvk::ClusterMaterialHandle
		getOrCreateMaterial(int gltfMaterialIndex);

	smvk::ClusterStaticMeshHandle
		getOrCreateMesh(
			int gltfMeshIndex,
			int primitiveIndex
		);

	smvk::ClusterLightHandle
		getOrCreateLight(int gltfLightIndex, bool staticLight);
	void importNodeRecursive(
		int gltfNodeIndex,
		int parentSceneNode,
		int level,
		bool topOfImport = false // the chain's first node carries the
		                         // coordFix basis (see importUnder)
	);
	smvk::Cluster& cluster;
	const tinygltf::Model& model;
	std::filesystem::path gltfModelPath;
	std::shared_ptr<TextureSource> textureSource;
	std::string modelUriBase; // normalized VFS file part for asset URIs
	smvk::MeshOptConfig meshOptConfig{}; // disabled by default (runtime parity)
	smvk::MeshletConfig meshletConfig{}; // disabled by default (opt-in cook)
	bool autoKtx_ = true;
	std::vector<smvk::tour::FileNode> fileNodes_;
	int coordFix_ = 0; // 0=engine fix, 1=skip, 2=invert Y, 3=cancel root -90°X + 100x scale
	std::vector<int> gltfNodeMap_; // glTF node idx -> cluster node id (-1 = none)
	glm::mat4 preTransform_{ 1.0f };
	std::vector<smvk::ClusterMaterialHandle> materialCache;
	std::vector<std::vector<smvk::ClusterStaticMeshHandle>> meshCache;
	std::vector<smvk::ClusterLightHandle> lightCache; // gltf light index -> handle
	std::unordered_map<int, smvk::TextureHandle> textureCache; // imageIndex -> handle
	// Optional texture-streaming sink: when set (and not cooking), uploads
	// go through TextureStreamer::StreamTexture (tail-resident) instead of
	// the full-res LoadKtxTexture* path. Owned by the caller (app/world).
	smvk::TextureStreamer* textureStreamer_ = nullptr;
	// Optional artifact cache (scratch-disk store adapter): texture
	// transcode acceleration. Not owned.
	GltfArtifactCache* artifactCache_ = nullptr;

	smvk::TextureHandle loadTextureForImage(int imageIndex, int textureIndex);
	// KTX auto-convert: file-node match for an image URI (basename, then
	// stem; null when no file node covers it).
	const smvk::tour::FileNode* matchFileNode(const std::string& imageUri) const;
	// Usage inference: true when any material samples the image as
	// baseColor/emissive (sRGB); data maps (normal/metallic/roughness/
	// occlusion) and unknown uses are linear.
	bool inferSrgb(int imageIndex) const;
	// KTX2 bytes for a source payload: KTX2 passes through; anything
	// else promotes a fresh `.ktx2` sidecar or transcodes (+caches the
	// sidecar when host-backed). False + err = keep today's skip.
	bool resolveKtxBytes(const std::string& imageUri, std::vector<uint8_t> srcBytes,
		int imageIndex, std::vector<uint8_t>& out, std::string& err) const;
	smvk::ClusterMaterialHandle defaultMaterialHandle{};
};


bool LoadModelFromAsciiFile(
	tinygltf::Model& model,
	const char* filename
);

// glTF punctual aim axis (KHR_lights_punctual: the light node's -Z
// direction). Raw locals (the coordFix basis rides the chain top):
// resolve world aims as globalTransform * (0,0,-1) in every mode.
glm::vec3 GltfPunctualAimAxis(int coordFix);

// Host-file texture source (default importer behavior): reads
// baseDir/imageUri from the host filesystem; identity is the normalized
// host-relative path so asset URIs match the VFS layout 1:1.
struct HostTextureSource final : GltfToSceneImporter::TextureSource {
	explicit HostTextureSource(std::filesystem::path baseDir);
	std::optional<std::vector<uint8_t>> ReadBytes(const std::string& imageUri) override;
	std::string ResolveIdentity(const std::string& imageUri) const override;
	std::optional<std::filesystem::path> HostPath(
		const std::string& imageUri) const override;
private:
	std::filesystem::path baseDir_;
};

// In-process texture conversion (tex_transcode.cpp): stb-decodes LDR
// sources (PNG/JPG/TGA/BMP/...) and writes an uncompressed RGBA8 KTX2
// with a full mip chain (gamma-correct for sRGB) via libktx, attempting
// zstd supercompression. False + err when the source isn't decodable
// (HDR .hdr/.exr must be checked by the caller — stb would mangle them).
bool TranscodeImageToKtx2(const uint8_t* srcBytes, size_t srcSize, bool srgb,
	const std::string& logName, std::vector<uint8_t>& outKtx2, std::string& err);

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

// Lists the scenes of a VFS glTF model (index order, names may be empty).
// Lightweight: parses the JSON only, no buffers/textures. Used by the
// cooker to fan out per-scene cooks. Returns false on error (err filled).
bool ListGltfScenesFromVfs(
	const smvk::OverlayVfs& vfs,
	const std::string& modelVfsPath,
	std::vector<std::string>& outNames,
	std::string* err = nullptr);

// VFS-first import: reads the .gltf JSON, external .bin buffers and all
// textures through vfs (the cooker mounts its content tree), stamps asset
// URIs from modelVfsPath, and optionally runs the meshopt cook pass.
// ctx may be nullptr together with a cook-mode cluster (see
// Cluster::SetCookMode) for a fully GPU-free cook. Example:
//   auto cluster = std::make_unique<Cluster>(nullptr);
//   cluster->SetCookMode(true);
//   GltfToClusterFromVfs(nullptr, vfs, "models/Sponza/sponza.gltf", *cluster, opt);
// gltfSceneIndex selects which glTF scene to import (<0 = default scene,
// matching the historic behavior); out of range fails loudly.
// autoKtx gates in-process KTX conversion of non-KTX sources (default on);
// fileNodes carries cooked file-node records for conversion intent (null =
// usage inference only — the VFS path has no tour).
void GltfToClusterFromVfs(
	smvk::ResourceManager* ctx,
	const smvk::OverlayVfs& vfs,
	const std::string& modelVfsPath,
	smvk::Cluster& out,
	const smvk::MeshOptConfig& meshOpt = smvk::MeshOptConfig{},
	const smvk::MeshletConfig& meshlets = smvk::MeshletConfig{},
	int coordFix = 0,
	const glm::mat4& preTransform = glm::mat4(1.0f),
	int gltfSceneIndex = -1,
	bool autoKtx = true,
	const std::vector<smvk::tour::FileNode>* fileNodes = nullptr,
	GltfArtifactCache* artifactCache = nullptr);

// ---------------------------------------------------------------------------
// Clarisse-style nested SCENE REFERENCES.
//
// `reference` lines in a .tour describe placeholder nodes whose subtree is
// ANOTHER scene, kept as a reference (structure + link to the source, never
// a flattened duplicate). This loader inlines them into the cluster scene:
//   - one placeholder group node per reference, under the recorded parent
//     (cluster node id), named "REF <name>" (deterministic ids: the import
//     stays reproducible, so tour commands and overrides addressing the
//     referenced nodes keep working);
//   - the referenced source's glTF imported beneath the placeholder;
//   - NESTING: each referenced source's own .tour (<stem>.tour next to it)
//     can carry its own `reference` lines — resolved recursively, so
//     shot -> car -> wheel chains assemble in one pass. Cycle-guarded
//     (a source never references itself up its own chain) + depth-capped.
//
// Host filesystem when vfs == nullptr (editor); VFS-resolved when set
// (cooker). outPlaceholderNodes receives the created placeholder CLUSTER
// node ids in doc.references order (level-1 refs only). Returns the
// number of references applied (level-1 + nested).
// ---------------------------------------------------------------------------
	size_t ApplySceneReferences(
	smvk::Cluster& cluster,
	const smvk::tour::Doc& doc,
	const std::string& ownerBaseDir,
	smvk::ResourceManager* ctx,
	smvk::TextureStreamer* streamer,
	const smvk::OverlayVfs* vfs,
	const smvk::MeshOptConfig& meshOpt = smvk::MeshOptConfig{},
	const smvk::MeshletConfig& meshlets = smvk::MeshletConfig{},
	int coordFix = 0,
	std::vector<int>* outPlaceholderNodes = nullptr,
	// Every failure (unresolved source, parse error, skipped command in
	// a referenced .tour, cycle...) collected for the editor's reference
	// list; nullptr keeps the stderr-only behavior.
	std::vector<std::string>* outIssues = nullptr,
	// Canonical RESOLVED source per top-level reference (in doc.references
	// order; empty when the source did not resolve). Lets path-solver /
	// symlink users verify what actually bound versus what the tour says.
	std::vector<std::string>* outResolvedSources = nullptr,
	// KTX conversion: autoKtx gates in-process conversion of non-KTX
	// sources (default on); fileNodes carries cooked file-node records
	// for conversion intent (null = usage inference only). Appended last
	// so existing positional callers keep compiling. artifactCache: the
	// scratch-disk store hook (null = no acceleration).
	bool autoKtx = true,
	const std::vector<smvk::tour::FileNode>* fileNodes = nullptr,
	GltfArtifactCache* artifactCache = nullptr,
	// GUID resolver (see GltfGuidResolver): identity-first reference
	// resolution; null = path-only (cooker/baker/tests).
	GltfGuidResolver* guidResolver = nullptr);

inline void GltfToCluster(
	smvk::ResourceManager& ctx,
	const tinygltf::Model& inModel,
	std::filesystem::path inGltfModelPath,
	smvk::Cluster& out,
	const smvk::MeshOptConfig& meshOpt = smvk::MeshOptConfig{},
	const smvk::MeshletConfig& meshlets = smvk::MeshletConfig{},
	int coordFix = 0, // 0=engine fix, 1=skip, 2=invert Y, 3=cancel root -90°X + 100x scale
	const glm::mat4& preTransform = glm::mat4(1.0f),
	smvk::TextureStreamer* streamer = nullptr, // tail-resident uploads when set (see texture_streamer.h)
	GltfArtifactCache* artifactCache = nullptr
) {
	GltfToSceneImporter importer{
		out,
		inModel,
		inGltfModelPath
	};
	importer.setMeshOptConfig(meshOpt);
	importer.setMeshletConfig(meshlets);
	importer.setArtifactCache(artifactCache);
	importer.setCoordFix(coordFix);
	importer.setPreTransform(preTransform);
	importer.setTextureStreamer(streamer);
	// Stamp VFS-style asset URIs ("<path>#mat.N", "<path>#mesh.N/prim.M").
	// setModelUriBase falls back to a content-relative/filename base for
	// absolute host paths (which NormalizeAssetUri rejects) so mesh URIs
	// — and with them LOD chains + meshlets — are never silently dropped.
	// URI-backed materials let tools (e.g. the glTF editor) map runtime
	// materials back to glTF material indices exactly.
	importer.setModelUriBase(inGltfModelPath.generic_string());

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

	importer.applyPreTransform(out.scene, root);

	RebuildStaticCache(out.scene);
	markAsChanged(out.scene, root);
	recalculateGlobalTransforms(out.scene);
	// Bake node world scale into cooked LOD errors (max over instances).
	smvk::FinalizeLodWorldErrors(out);
	// Flush batched uploads (256MB threshold, or manual at end of whole scene)
	if (out.ctx_)
		out.ctx_->FlushUploads();
}

