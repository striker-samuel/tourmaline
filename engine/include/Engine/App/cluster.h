
#pragma once


#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Engine/smvk.h>
#include <Engine/Memory/arena.h>
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>
#include <Engine/Memory/string_view.h>
#include <Engine/Memory/uri_map.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>

namespace smvk {

	struct World;

	template<typename HandleT>
	struct AssetRef;

	// Backend-defined record of a static mesh's renderer allocations
	// (opaque here; complete type lives in the Vulkan detail header).
	struct StaticMeshAllocation;

	using ClusterStaticMeshHandle = Handle<StaticMesh>;
	using ClusterMaterialHandle = Handle<Material>;
	using ClusterTextureHandle = Handle<struct Texture>;
	using ClusterLightHandle = Handle<SceneLight>;

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
		// Cooked punctual lights (KHR_lights_punctual): value-only pool
		// assets like meshes. scene.lightForNode stores pool indices; the
		// pak round-trip is one Light blob per entry (see
		// cluster_serializer.cpp), loaded back through this pool.
		Pool<SceneLight, SceneLight> lights;

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
			bool doubleSided,
			bool castsShadows = true);

		// Cooked punctual light (KHR_lights_punctual): pure value asset,
		// no GPU side. World position/direction resolve from the light
		// node's global transform at use time (see Scene::lightForNode).
		ClusterLightHandle CreateLight(SceneLight def);

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
		//
		// Storage is string-free: keys are StringViewT slices owned by the
		// per-Cluster uriArena_ (freed wholesale with the Cluster) and the
		// maps are open-addressing UriMaps — lookups take caller views and
		// never allocate (the previous std::unordered_map<std::string,...>
		// round-tripped a std::string per lookup).
		// ------------------------------------------------------------------
		bool RegisterMeshUri(StringViewT uri, ClusterStaticMeshHandle handle);
		bool RegisterMaterialUri(StringViewT uri, ClusterMaterialHandle handle);
		bool RegisterLightUri(StringViewT uri, ClusterLightHandle handle);
		// Textures live in the global RHI pool for binding (materials store
		// RHI TextureHandles); the Cluster pool only owns lifetime Holders.
		// The uri map therefore stores the RHI handle used at bind time.
		bool RegisterTextureUri(StringViewT uri, TextureHandle handle);
		void UnregisterUri(StringViewT uri);

		std::optional<ClusterStaticMeshHandle> FindMeshByUri(StringViewT uri) const;
		std::optional<ClusterMaterialHandle> FindMaterialByUri(StringViewT uri) const;
		std::optional<ClusterLightHandle> FindLightByUri(StringViewT uri) const;
		std::optional<TextureHandle> FindTextureByUri(StringViewT uri) const;

		// Resolves an AssetRef in place: keeps ref.uri, fills cluster/handle
		// on hit, clears resolution on miss. Returns true on hit.
		template<typename HandleT>
		bool ResolveAssetRef(AssetRef<HandleT>& ref);

		// Reverse lookup for serialization: asset handle -> stored uri.
		// Returns an empty view when the handle was created without a
		// registered uri (procedural/default assets); the serializer then
		// assigns a "<cluster-local>#..." fallback uri. The view points
		// into the cluster's uri arena and stays valid for the cluster's
		// lifetime.
		StringViewT GetMeshUri(ClusterStaticMeshHandle handle) const;
		StringViewT GetMaterialUri(ClusterMaterialHandle handle) const;
		StringViewT GetLightUri(ClusterLightHandle handle) const;

		// Per-mesh LOD marker (see Asset/mesh_optimizer.h). Stored into
		// MeshBlobs; the renderer does not branch on it.
		void RegisterMeshLod(StringViewT uri, const LodInfo& lod);
		std::optional<LodInfo> GetMeshLod(StringViewT uri) const;
		// All LOD markers for cook post-passes (see FinalizeLodWorldErrors).
		std::vector<std::pair<StringViewT, LodInfo>> ListMeshLods() const;

		// Per-mesh cooked meshlets (see Asset/meshlet.h). Stamped by the
		// importer when meshlet cooking is enabled, persisted in MeshBlobs,
		// and loaded into StaticMesh::meshlets on deserialize. The renderer
		// does not consume them yet (indirect path foundation only).
		void RegisterMeshMeshlets(StringViewT uri, const CookedMeshlets& meshlets);
		std::optional<CookedMeshlets> GetMeshMeshlets(StringViewT uri) const;

		// Enumerates registered texture URIs for serialization
		// ({uri, RHI handle} pairs, sorted by uri for deterministic paks).
		std::vector<std::pair<StringViewT, TextureHandle>> ListTextureUris() const;

		// glTF sampler parameters per texture uri (persisted into paks;
		// the renderer still uses its default sampler until sampler
		// selection is wired up — see Asset/asset_registry.h notes).
		void RegisterTextureSampler(StringViewT uri, const TextureSamplerDesc& desc);
		std::optional<TextureSamplerDesc> GetTextureSampler(StringViewT uri) const;

		// Embedded source bytes per texture uri (populated by the importer
		// so the cooker can embed .ktx2 payloads without re-reading the
		// host fs; also populated on pak load so re-cook works).
		// Call ReleaseTextureSources() to drop the copies at runtime.
		void SetTextureSource(StringViewT uri, std::vector<uint8_t> bytes);
		const std::vector<uint8_t>* GetTextureSource(StringViewT uri) const;
		void ReleaseTextureSources();

	private:
		// Owned URI key storage for all maps below; freed wholesale with
		// the Cluster (see asset_registry.cpp registration flow).
		Arena uriArena_;
		UriMap<ClusterStaticMeshHandle> meshByUri_;
		UriMap<ClusterMaterialHandle> materialByUri_;
		UriMap<ClusterLightHandle> lightByUri_;
		UriMap<TextureHandle> textureByUri_;
		UriMap<TextureSamplerDesc> textureSamplers_;
		UriMap<std::vector<uint8_t>> textureSources_;
		UriMap<LodInfo> meshLods_;
		UriMap<CookedMeshlets> meshMeshlets_;
		bool retainUploads_ = false;
		std::unordered_map<uint32_t, StaticMeshUpload> retainedUploads_;
		// Per-cluster static mesh allocations keyed by the local staticMeshes
		// pool index (pool indices are only unique within one Cluster).
		std::unordered_map<uint32_t, StaticMeshAllocation*> meshAllocations_;
		bool cookMode_ = false;
		uint32_t cookNextTextureId_ = 0;
	};

template<typename HandleT>
struct AssetRef {
	StringViewT uri;

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



