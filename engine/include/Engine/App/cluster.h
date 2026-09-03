
#pragma once


#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>

namespace smvk {

	struct World;

	template<typename HandleT>
	struct AssetRef;

	using ClusterStaticMeshHandle = Handle<StaticMesh>;
	using ClusterMaterialHandle = Handle<Material>;
	using ClusterTextureHandle = Handle<struct Texture>;

	struct Cluster {
		// ctx may be nullptr for GPU-less cook clusters (see SetCookMode);
		// any GPU upload with a null context is a programming error.
		explicit Cluster(ResourceManager* ctx);
		~Cluster();
		Cluster(const Cluster&) = delete;
		Cluster& operator=(const Cluster&) = delete;

		ResourceManager* ctx_ = nullptr;
		Scene scene;
		World* world = nullptr;
		Cluster* sibling = nullptr;
		// linked list for import chain if multiple clusters belong to one input file.

		Pool<StaticMesh, StaticMesh> staticMeshes;
		//Pool<SkinnedMesh, SkinnedMesh> skinnedMesh;
		Pool<Material, Material> materials;
		Pool<Texture, Holder<TextureHandle>> textures;

		ClusterStaticMeshHandle UploadStaticMesh(const StaticMeshUpload& data);
		ClusterMaterialHandle CreateMaterial(TextureHandle diffuse);
		ClusterMaterialHandle CreateMaterial(const MaterialCreateInfo& info);
		// Pure value assembly (no texture resolution): shared core of
		// CreateMaterial and the GPU-free cook path. textureIds use the
		// same integer domain as MaterialValues (RHI indices at runtime,
		// cook-table ids when cooking).
		ClusterMaterialHandle CreateMaterialFromValues(
			const MaterialValues& values,
			ShadingModel shadingModel,
			AlphaMode alphaMode,
			float alphaCutoff,
			bool doubleSided);

		// Cook support: when enabled, UploadStaticMesh keeps a CPU-side copy
		// of every upload keyed by pool index so the pak cooker can embed
		// full vertex/index payloads (runtime Clusters never see this cost:
		// the default is off, and GPU pools hold no CPU vertex data after
		// upload). DestroyMesh drops the retained copy.
		void SetRetainUploads(bool retain);
		const StaticMeshUpload* GetRetainedUpload(ClusterStaticMeshHandle handle) const;

		// GPU-free cook mode (pak cooker without a Vulkan device).
		// UploadStaticMesh records CPU geometry + retained uploads only
		// (no VMA/streamer touch); textures mint fake RHI handles whose
		// indices are stable cook ids (see Handle::Mint). The cook cluster
		// is serializable and nothing else: never register it with a World
		// or render from it. Implies SetRetainUploads(true).
		void SetCookMode(bool cook);
		bool IsCookMode() const { return cookMode_; }
		// Allocates the next cook texture id (valid Handle::Mint index for
		// MaterialValues texture slots).
		uint32_t AllocCookTextureId();

		void DestroyMesh(ClusterStaticMeshHandle handle);
		void DestroyMaterial(ClusterMaterialHandle handle);

		// ------------------------------------------------------------------
		// URI-mapped resource lookup (AssetRef backing store).
		//
		// Identity = VFS path with optional '#'-fragment (see
		// Engine/Asset/asset_registry.h). The VFS overlay decides which
		// bytes back a path; these maps only resolve an already-loaded
		// asset inside this Cluster. Main-thread only (Cluster pools are
		// externally synchronized, see Memory/pool.h).
		// ------------------------------------------------------------------
		bool RegisterMeshUri(const std::string& uri, ClusterStaticMeshHandle handle);
		bool RegisterMaterialUri(const std::string& uri, ClusterMaterialHandle handle);
		// Textures live in the global RHI pool for binding (materials store
		// RHI TextureHandles); the Cluster pool only owns lifetime Holders.
		// The uri map therefore stores the RHI handle used at bind time.
		bool RegisterTextureUri(const std::string& uri, TextureHandle handle);
		void UnregisterUri(const std::string& uri);

		std::optional<ClusterStaticMeshHandle> FindMeshByUri(const std::string& uri) const;
		std::optional<ClusterMaterialHandle> FindMaterialByUri(const std::string& uri) const;
		std::optional<TextureHandle> FindTextureByUri(const std::string& uri) const;

		// Resolves an AssetRef in place: keeps ref.uri, fills cluster/handle
		// on hit, clears resolution on miss. Returns true on hit.
		template<typename HandleT>
		bool ResolveAssetRef(AssetRef<HandleT>& ref);

		// Reverse lookup for serialization: asset handle -> stored uri.
		// Returns "" when the handle was created without a registered uri
		// (procedural/default assets); the serializer then assigns a
		// "<cluster-local>#..." fallback uri.
		std::string GetMeshUri(ClusterStaticMeshHandle handle) const;
		std::string GetMaterialUri(ClusterMaterialHandle handle) const;

		// Per-mesh LOD marker (cook-time instrumentation, see
		// Asset/mesh_optimizer.h). Stored into MeshBlobs; the renderer does
		// not branch on it.
		void RegisterMeshLod(const std::string& uri, const LodInfo& lod);
		std::optional<LodInfo> GetMeshLod(const std::string& uri) const;

		// Enumerates registered texture URIs for serialization
		// ({uri, RHI handle} pairs, sorted by uri for deterministic paks).
		std::vector<std::pair<std::string, TextureHandle>> ListTextureUris() const;

		// glTF sampler parameters per texture uri (persisted into paks;
		// the renderer still uses its default sampler until sampler
		// selection is wired up — see Asset/asset_registry.h notes).
		void RegisterTextureSampler(const std::string& uri, const TextureSamplerDesc& desc);
		std::optional<TextureSamplerDesc> GetTextureSampler(const std::string& uri) const;

		// Embedded source bytes per texture uri (populated by the importer
		// so the cooker can embed .ktx2 payloads without re-reading the
		// host fs; also populated on pak load so re-cook works).
		// Call ReleaseTextureSources() to drop the copies at runtime.
		void SetTextureSource(const std::string& uri, std::vector<uint8_t> bytes);
		const std::vector<uint8_t>* GetTextureSource(const std::string& uri) const;
		void ReleaseTextureSources();

	private:
		std::unordered_map<std::string, ClusterStaticMeshHandle> meshByUri_;
		std::unordered_map<std::string, ClusterMaterialHandle> materialByUri_;
		std::unordered_map<std::string, TextureHandle> textureByUri_;
		std::unordered_map<std::string, TextureSamplerDesc> textureSamplers_;
		std::unordered_map<std::string, std::vector<uint8_t>> textureSources_;
		std::unordered_map<std::string, LodInfo> meshLods_;
		bool retainUploads_ = false;
		std::unordered_map<uint32_t, StaticMeshUpload> retainedUploads_;
		bool cookMode_ = false;
		uint32_t cookNextTextureId_ = 0;
	};

	template<typename HandleT>
	struct AssetRef {
		std::string uri;

		Cluster* cluster = nullptr;
		HandleT handle{};

		bool resolved() const {
			return cluster != nullptr && handle.valid();
		}

		void clearResolution() {
			cluster = nullptr;
			handle = {};
		}
	};

	// ResolveAssetRef specializations (defined here; the generic template
	// is intentionally undefined for unsupported handle types).
	template<typename HandleT>
	bool Cluster::ResolveAssetRef(AssetRef<HandleT>& ref) {
		static_assert(!sizeof(HandleT*), "ResolveAssetRef: unsupported handle type");
		return false;
	}

	template<>
	inline bool Cluster::ResolveAssetRef<ClusterStaticMeshHandle>(
		AssetRef<ClusterStaticMeshHandle>& ref) {
		auto hit = FindMeshByUri(ref.uri);
		if (!hit) {
			ref.clearResolution();
			return false;
		}
		ref.cluster = this;
		ref.handle = *hit;
		return true;
	}

	template<>
	inline bool Cluster::ResolveAssetRef<ClusterMaterialHandle>(
		AssetRef<ClusterMaterialHandle>& ref) {
		auto hit = FindMaterialByUri(ref.uri);
		if (!hit) {
			ref.clearResolution();
			return false;
		}
		ref.cluster = this;
		ref.handle = *hit;
		return true;
	}

	template<>
	inline bool Cluster::ResolveAssetRef<ClusterTextureHandle>(
		AssetRef<ClusterTextureHandle>& ref) {
		// Cluster pool holders are lifetime-only; resolution by RHI handle
		// is served through FindTextureByUri (see asset_registry.cpp).
		(void)ref;
		return false;
	}
}



